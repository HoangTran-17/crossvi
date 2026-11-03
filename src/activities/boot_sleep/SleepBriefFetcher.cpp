#include "SleepBriefFetcher.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstring>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "Memory.h"
#include "SleepBrief.h"
#include "SleepBriefFetch.h"
#include "WifiCredentialStore.h"
#include "network/HttpDownloader.h"

namespace sleep_brief_fetch {
namespace {

void wifiOff() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

// Saved networks in the order to try: the last connected one first. No scan, so a hidden SSID still works.
std::vector<WifiCredential> savedNetworksInOrder() {
  std::vector<WifiCredential> order;
  const std::string last = WIFI_STORE.getLastConnectedSsid();
  if (!last.empty()) {
    if (auto cred = WIFI_STORE.findCredential(last)) order.push_back(std::move(*cred));
  }
  for (size_t i = 0; i < WIFI_STORE.getCredentialCount(); ++i) {
    auto cred = WIFI_STORE.getCredentialAt(i);
    if (cred && cred->ssid != last) order.push_back(std::move(*cred));
  }
  return order;
}

// One shared budget for all networks. A network that is out of range fails fast (WL_NO_SSID_AVAIL) and the next one
// gets the remaining time.
bool connectSavedWifi(const uint32_t budgetMs) {
  const uint32_t start = millis();
  WiFi.persistent(false);  // Credentials live in WifiCredentialStore; keep the SDK from writing NVS
  for (const WifiCredential& cred : savedNetworksInOrder()) {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true, true);
    delay(100);
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    if (cred.password.empty()) {
      WiFi.begin(cred.ssid.c_str());
    } else {
      WiFi.begin(cred.ssid.c_str(), cred.password.c_str());
    }
    while (static_cast<uint32_t>(millis() - start) < budgetMs) {
      const wl_status_t status = WiFi.status();
      if (status == WL_CONNECTED) return true;
      if (status == WL_NO_SSID_AVAIL || status == WL_CONNECT_FAILED) break;
      delay(100);
    }
    if (static_cast<uint32_t>(millis() - start) >= budgetMs) break;
  }
  return false;
}

// Replaces BRIEF_PATH with a file that already passed validation. The temp file is removed on every failure.
bool replaceBriefFile(const char* json, const size_t length) {
  if (Storage.exists(TEMP_PATH)) Storage.remove(TEMP_PATH);
  HalFile file;
  if (!Storage.openFileForWrite("SLW", TEMP_PATH, file)) return false;
  const bool written = file.write(json, length) == length && file.sync();
  const bool closed = file.close();
  if (!written || !closed) {
    Storage.remove(TEMP_PATH);
    return false;
  }
  if (Storage.exists(BRIEF_PATH) && !Storage.remove(BRIEF_PATH)) {
    Storage.remove(TEMP_PATH);
    return false;
  }
  if (!Storage.rename(TEMP_PATH, BRIEF_PATH)) {
    Storage.remove(TEMP_PATH);
    return false;
  }
  return true;
}

ResolvedUrl loadUrl() {
  auto buffer = makeUniqueNoThrow<char[]>(MAX_URL_FILE_BYTES + 1);
  if (!buffer || !Storage.exists(URL_FILE_PATH)) return resolveUrl(nullptr, 0);
  const size_t length = Storage.readFileToBuffer(URL_FILE_PATH, buffer.get(), MAX_URL_FILE_BYTES + 1);
  return resolveUrl(buffer.get(), length);
}

}  // namespace

void runOnTimerWake(const uint8_t batteryPercent) {
  const bool settingOn = SETTINGS.sleepBriefFetch != 0;
  const bool briefLayout = SETTINGS.sleepCalendarLayout == CrossPointSettings::CALENDAR_LAYOUT_BRIEF;
  bool hasSavedWifi = false;
  if (settingOn && briefLayout) {
    WIFI_STORE.loadFromFile();
    hasSavedWifi = WIFI_STORE.getCredentialCount() > 0;
  }
  const Skip skip = skipReason({settingOn, briefLayout, hasSavedWifi, batteryPercent});
  if (skip != Skip::None) {
    LOG_INF("SLW", "brief fetch skip reason=%s", skipName(skip));
    return;
  }

  const uint32_t start = millis();
  const ResolvedUrl target = loadUrl();
  if (target.source == UrlSource::FileRejected) {
    LOG_ERR("SLW", "brief_url.txt is not a plain https URL, using the default");
  }
  const std::string host = hostOf(target.url);

  if (!connectSavedWifi(WIFI_TIMEOUT_MS)) {
    wifiOff();
    LOG_ERR("SLW", "brief fetch fail reason=wifi host=%s ms=%lu", host.c_str(),
            static_cast<unsigned long>(millis() - start));
    return;
  }

  auto body = makeUniqueNoThrow<char[]>(sleep_brief::MAX_BRIEF_BYTES);
  if (!body) {
    wifiOff();
    LOG_ERR("SLW", "brief fetch fail reason=oom");
    return;
  }
  size_t length = 0;
  bool overflow = false;
  const HttpDownloader::DownloadError result = HttpDownloader::fetchBounded(
      target.url, HTTP_TIMEOUT_MS, sleep_brief::MAX_BRIEF_BYTES, [&](const uint8_t* data, const size_t count) {
        if (count > sleep_brief::MAX_BRIEF_BYTES - length) {
          overflow = true;
          return false;
        }
        std::memcpy(body.get() + length, data, count);
        length += count;
        return true;
      });
  // Radio off before anything slow: SD writes and the e-paper refresh must not run with Wi-Fi powered.
  wifiOff();

  if (result != HttpDownloader::OK) {
    LOG_ERR("SLW", "brief fetch fail reason=%s host=%s bytes=%u ms=%lu", overflow ? "too_large" : "http", host.c_str(),
            static_cast<unsigned>(length), static_cast<unsigned long>(millis() - start));
    return;
  }
  const Validation validation = validateBrief(body.get(), length);
  if (validation != Validation::Ok) {
    LOG_ERR("SLW", "brief fetch fail reason=%s host=%s bytes=%u", validationName(validation), host.c_str(),
            static_cast<unsigned>(length));
    return;
  }
  if (!replaceBriefFile(body.get(), length)) {
    LOG_ERR("SLW", "brief fetch fail reason=write host=%s bytes=%u", host.c_str(), static_cast<unsigned>(length));
    return;
  }
  LOG_INF("SLW", "brief fetch ok host=%s bytes=%u ms=%lu", host.c_str(), static_cast<unsigned>(length),
          static_cast<unsigned long>(millis() - start));
}

}  // namespace sleep_brief_fetch
