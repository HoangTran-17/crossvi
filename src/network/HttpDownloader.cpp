#include "HttpDownloader.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <Version.h>
#include <base64.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <mbedtls/sha256.h>
#include <strings.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <limits>
#include <string>

#include "HttpTransportPolicy.h"

namespace {
// RX holds the response headers. Smaller buffers leave enough contiguous heap
// for mbedTLS on redirect-heavy OPDS feeds while still preserving the headers
// we read directly (Location, Content-Length).
constexpr int HTTP_RX_BUF = 2048;
constexpr int HTTP_TX_BUF = 512;
// Per-socket-op timeout. Some OPDS download endpoints are slow to send headers
// (>15s) and chunked catalogs stall mid-body, so 15s killed them. 60s gives
// slow servers room. esp_http_client's timeout_ms is uint32, so unlike Arduino
// HTTPClient's uint16 setTimeout it doesn't silently truncate.
constexpr int HTTP_TIMEOUT_MS = 60000;
constexpr size_t READ_CHUNK = 1024;
constexpr int MAX_REDIRECTS = 5;

struct Sink {
  std::function<bool(const uint8_t*, size_t)> write;  // returns false to abort the transfer
  HttpDownloader::ProgressCallback progress;
  bool* cancelFlag = nullptr;
  size_t total = 0;
  size_t downloaded = 0;
  size_t maxBytes = std::numeric_limits<size_t>::max();
  bool allowGithubReleaseHttpRedirect = false;
  int timeoutMs = HTTP_TIMEOUT_MS;
  bool hasDeadline = false;
  uint32_t deadlineMs = 0;  // millis() value; compared wrap-safe
};

// Time left before the sink's wall-clock deadline: INT32_MAX without one, <= 0 once it has passed.
int32_t remainingMs(const Sink& sink) {
  return sink.hasDeadline ? static_cast<int32_t>(sink.deadlineMs - millis()) : std::numeric_limits<int32_t>::max();
}

// X3 units have no USB data line, so a failed request is recorded on the SD card where the user can read it.
constexpr char NET_LOG_PATH[] = "/.crosspoint/net_log.txt";
constexpr size_t NET_LOG_MAX_BYTES = 16 * 1024;

struct HeapSample {
  uint32_t free = 0;
  uint32_t maxAlloc = 0;
};

HeapSample sampleHeap() { return {ESP.getFreeHeap(), ESP.getMaxAllocHeap()}; }

// Host only: unattended callers may use a URL whose path carries a secret.
std::string hostOf(const std::string& url) {
  const size_t schemeEnd = url.find("://");
  const size_t start = schemeEnd == std::string::npos ? 0 : schemeEnd + 3;
  const size_t end = url.find_first_of("/?#", start);
  std::string host = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
  const size_t at = host.rfind('@');
  return at == std::string::npos ? host : host.substr(at + 1);
}

void appendNetLog(const char* stage, const std::string& url, esp_http_client_handle_t client, const esp_err_t err,
                  const int status, const HeapSample& before) {
  int tlsCode = 0;
  int tlsFlags = 0;
  const esp_err_t tlsErr = client ? esp_http_client_get_and_clear_last_tls_error(client, &tlsCode, &tlsFlags) : ESP_OK;

  char when[24] = "no-clock";
  const time_t now = time(nullptr);
  tm local{};
  if (now > 1600000000 && localtime_r(&now, &local)) strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &local);

  wifi_ap_record_t ap{};
  const int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
  esp_netif_ip_info_t ip{};
  esp_netif_dns_info_t dns{};
  if (esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) {
    esp_netif_get_ip_info(sta, &ip);
    esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns);
  }

  const HeapSample after = sampleHeap();
  char line[320];
  const int len = snprintf(line, sizeof(line),
                           "%s up=%lu stage=%s host=%s err=%s tls_err=0x%x tls_code=-0x%x tls_flags=0x%x status=%d "
                           "heap_before=%u/%u heap_now=%u/%u minfree=%u rssi=%d ip=" IPSTR " dns=" IPSTR "\n",
                           when, millis(), stage, hostOf(url).c_str(), esp_err_to_name(err),
                           static_cast<unsigned>(tlsErr), static_cast<unsigned>(-tlsCode),
                           static_cast<unsigned>(tlsFlags), status, before.free, before.maxAlloc, after.free,
                           after.maxAlloc, ESP.getMinFreeHeap(), rssi, IP2STR(&ip.ip), IP2STR(&dns.ip.u_addr.ip4));
  if (len <= 0) return;
  LOG_ERR("HTTP", "%s", line);

  HalFile file = Storage.open(NET_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND);
  if (!file) return;
  if (file.fileSize() > NET_LOG_MAX_BYTES) {
    file.close();
    Storage.remove(NET_LOG_PATH);
    file = Storage.open(NET_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND);
    if (!file) return;
  }
  file.write(line, std::min<size_t>(len, sizeof(line) - 1));
  file.close();
}

bool isRedirect(const int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

bool decodeSha256(const std::string& hex, std::array<uint8_t, 32>& digest) {
  if (hex.size() != digest.size() * 2) return false;
  for (size_t i = 0; i < digest.size(); ++i) {
    const auto nibble = [](const char value) -> int {
      if (value >= '0' && value <= '9') return value - '0';
      if (value >= 'a' && value <= 'f') return value - 'a' + 10;
      if (value >= 'A' && value <= 'F') return value - 'A' + 10;
      return -1;
    };
    const int high = nibble(hex[i * 2]);
    const int low = nibble(hex[i * 2 + 1]);
    if (high < 0 || low < 0) return false;
    digest[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

struct RedirectHeader {
  std::array<char, HttpTransportPolicy::MAX_URL_BYTES + 1> value{};
  bool present = false;
  bool truncated = false;
};

esp_err_t captureRedirectHeader(esp_http_client_event_t* event) {
  if (!event || event->event_id != HTTP_EVENT_ON_HEADER || !event->header_key || !event->header_value ||
      strcasecmp(event->header_key, "Location") != 0) {
    return ESP_OK;
  }

  auto* redirect = static_cast<RedirectHeader*>(event->user_data);
  if (!redirect) return ESP_OK;

  const size_t length = strnlen(event->header_value, redirect->value.size());
  redirect->present = true;
  redirect->truncated = length >= redirect->value.size();
  if (!redirect->truncated) {
    memcpy(redirect->value.data(), event->header_value, length + 1);
  }
  return ESP_OK;
}

// Streams a response body through sink.write in READ_CHUNK pieces. Uses the manual
// open/fetch_headers/read path rather than esp_http_client_perform(): perform()
// pushes the whole body through an event callback and reports a chunked body
// that ends early as ESP_ERR_HTTP_INCOMPLETE_DATA, whereas the read loop streams
// large/slow files and surfaces a short read directly.
HttpDownloader::DownloadError runGetSecure(const std::string& url, const std::string& username,
                                           const std::string& password, Sink& sink) {
  const bool hasCredentials = !username.empty() || !password.empty();
  if (!HttpTransportPolicy::isSupportedUrl(url) || (hasCredentials && !HttpTransportPolicy::credentialsAllowed(url))) {
    LOG_ERR("HTTP", "Rejected URL or credential transport policy");
    appendNetLog("policy", url, nullptr, ESP_OK, 0, sampleHeap());
    return HttpDownloader::HTTP_ERROR;
  }

  const std::string userAgent = std::string("CrossVi-ESP32-") + CROSSPOINT_VERSION;
  std::string authorization;
  if (hasCredentials) {
    const std::string credentials = username + ":" + password;
    const String encodedCredentials = base64::encode(credentials.c_str());
    authorization.reserve(6 + encodedCredentials.length());
    authorization = "Basic ";
    authorization.append(encodedCredentials.c_str(), encodedCredentials.length());
  }

  std::string currentUrl = url;
  esp_http_client_handle_t client = nullptr;
  int64_t contentLength = -1;
  int status = 0;
  for (int hop = 0; hop <= MAX_REDIRECTS; ++hop) {
    const int32_t remaining = remainingMs(sink);
    if (remaining <= 0) {
      LOG_ERR("HTTP", "deadline reached before request");
      return HttpDownloader::HTTP_ERROR;
    }
    RedirectHeader redirect;
    esp_http_client_config_t config = {};
    config.url = currentUrl.c_str();
    config.buffer_size = HTTP_RX_BUF;
    // Signed GitHub release redirects can be close to 1 KiB. Leave room for
    // the request line and headers without paying that cost for short URLs.
    config.buffer_size_tx = static_cast<int>(currentUrl.size() + HTTP_TX_BUF);
    config.timeout_ms = std::min<int32_t>(sink.timeoutMs, remaining);
    config.disable_auto_redirect = true;
    // Verify HTTPS against the bundled CA roots. This build has esp-tls
    // CONFIG_ESP_TLS_INSECURE off, so an unverified TLS handshake can't be set
    // up at all; the model is public servers over verified https and local
    // servers over plain http (esp_http_client picks the transport from the URL
    // scheme, so http:// needs no cert config). The prior setInsecure() worked
    // only because Arduino's ssl_client drives mbedtls directly.
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.keep_alive_enable = true;
    config.event_handler = captureRedirectHeader;
    config.user_data = &redirect;

    client = esp_http_client_init(&config);
    if (!client) {
      LOG_ERR("HTTP", "client init failed");
      return HttpDownloader::HTTP_ERROR;
    }
    if (esp_http_client_set_header(client, "User-Agent", userAgent.c_str()) != ESP_OK ||
        (hasCredentials && esp_http_client_set_header(client, "Authorization", authorization.c_str()) != ESP_OK)) {
      LOG_ERR("HTTP", "Failed to set request headers");
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }

    const HeapSample beforeOpen = sampleHeap();
    const esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
      LOG_ERR("HTTP", "%s open failed: %s", hop == 0 ? "initial" : "redirect", esp_err_to_name(err));
      appendNetLog(hop == 0 ? "open" : "redirect_open", currentUrl, client, err, 0, beforeOpen);
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
    contentLength = esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);
    if (!isRedirect(status)) break;
    if (hop == MAX_REDIRECTS || redirect.truncated) {
      LOG_ERR("HTTP", "Too many redirects or missing redirect target");
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }

    std::array<char, HttpTransportPolicy::MAX_URL_BYTES + 1> redirectedUrl{};
    if (redirect.present && HttpTransportPolicy::isSupportedUrl(redirect.value.data())) {
      memcpy(redirectedUrl.data(), redirect.value.data(), redirect.value.size());
    } else if (esp_http_client_set_redirection(client) != ESP_OK ||
               esp_http_client_get_url(client, redirectedUrl.data(), redirectedUrl.size()) != ESP_OK) {
      LOG_ERR("HTTP", "Missing redirect target");
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
    std::string nextUrl(redirectedUrl.data());
    if (sink.allowGithubReleaseHttpRedirect && HttpTransportPolicy::isGithubReleaseAssetRedirect(currentUrl, nextUrl)) {
      // Integrity and authenticity are enforced by fetchGithubReleaseAsset's
      // SHA-256 from the verified GitHub API response. Avoid the CDN's RSA TLS
      // handshake, which exhausts the X3's contiguous heap.
      nextUrl.replace(0, 8, "http://");
    } else if (!HttpTransportPolicy::redirectAllowed(currentUrl, nextUrl, hasCredentials)) {
      LOG_ERR("HTTP", "Rejected unsafe redirect");
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
    // A closed esp_http_client still retains its parser, response headers and
    // redirect URL. Destroy it before the next TLS handshake so both hops do
    // not compete for the X3's small contiguous heap.
    esp_http_client_cleanup(client);
    client = nullptr;
    currentUrl = std::move(nextUrl);
    LOG_DBG("HTTP", "Following %zu-byte redirect: free=%u maxalloc=%u", currentUrl.size(), ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
  }

  if (contentLength < 0 || status != 200) {
    LOG_ERR("HTTP", "unexpected status: %d, headers=%lld, errno=%d, free=%u, maxalloc=%u", status,
            static_cast<long long>(contentLength), esp_http_client_get_errno(client), ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    appendNetLog("status", currentUrl, client, ESP_OK, status, sampleHeap());
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }

  if (contentLength > static_cast<int64_t>(std::numeric_limits<size_t>::max()) ||
      (contentLength > 0 && static_cast<size_t>(contentLength) > sink.maxBytes)) {
    LOG_ERR("HTTP", "response exceeds size limit");
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }

  // fetch_headers returns 0 for a chunked response (no Content-Length); leave
  // total at 0 so progress stays silent and the size check is skipped.
  sink.total = contentLength > 0 ? static_cast<size_t>(contentLength) : 0;

  auto buf = makeUniqueNoThrow<char[]>(READ_CHUNK);
  if (!buf) {
    LOG_ERR("HTTP", "OOM: %u byte read buffer", (unsigned)READ_CHUNK);
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }

  while (true) {
    if (sink.cancelFlag && *sink.cancelFlag) {
      esp_http_client_cleanup(client);
      return HttpDownloader::ABORTED;
    }
    if (remainingMs(sink) <= 0) {
      LOG_ERR("HTTP", "deadline reached after %zu bytes", sink.downloaded);
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
    const int read = esp_http_client_read(client, buf.get(), READ_CHUNK);
    if (read < 0) {
      LOG_ERR("HTTP", "read error after %zu bytes", sink.downloaded);
      appendNetLog("read", currentUrl, client, ESP_FAIL, status, sampleHeap());
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
    if (read == 0) break;  // all data received
    if (static_cast<size_t>(read) > sink.maxBytes - sink.downloaded) {
      LOG_ERR("HTTP", "response exceeds size limit");
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
    if (!sink.write(reinterpret_cast<const uint8_t*>(buf.get()), read)) {
      esp_http_client_cleanup(client);
      return HttpDownloader::FILE_ERROR;
    }
    sink.downloaded += read;
    if (sink.progress && sink.total > 0) sink.progress(sink.downloaded, sink.total);
  }

  const bool complete = esp_http_client_is_complete_data_received(client);
  esp_http_client_cleanup(client);
  if (!complete) {
    LOG_ERR("HTTP", "incomplete: got %zu of %zu bytes", sink.downloaded, sink.total);
    return HttpDownloader::HTTP_ERROR;
  }
  return HttpDownloader::OK;
}

HttpDownloader::DownloadError runGithubReleaseAsset(const std::string& url, const std::string& expectedSha256,
                                                    Sink& sink) {
  std::array<uint8_t, 32> expected{};
  if (!decodeSha256(expectedSha256, expected)) {
    LOG_ERR("HTTP", "Rejected invalid GitHub release digest");
    return HttpDownloader::INTEGRITY_ERROR;
  }

  mbedtls_sha256_context hash;
  mbedtls_sha256_init(&hash);
  mbedtls_sha256_starts(&hash, 0);

  auto destinationWrite = std::move(sink.write);
  sink.allowGithubReleaseHttpRedirect = true;
  sink.write = [&hash, &destinationWrite](const uint8_t* data, const size_t len) {
    mbedtls_sha256_update(&hash, data, len);
    return destinationWrite(data, len);
  };
  const HttpDownloader::DownloadError result = runGetSecure(url, "", "", sink);
  sink.write = std::move(destinationWrite);

  std::array<uint8_t, 32> actual{};
  mbedtls_sha256_finish(&hash, actual.data());
  mbedtls_sha256_free(&hash);
  if (result != HttpDownloader::OK) return result;

  uint8_t difference = 0;
  for (size_t i = 0; i < actual.size(); ++i) difference |= actual[i] ^ expected[i];
  if (difference != 0) {
    LOG_ERR("HTTP", "GitHub release SHA-256 mismatch");
    return HttpDownloader::INTEGRITY_ERROR;
  }
  return HttpDownloader::OK;
}

}  // namespace

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  Sink sink;
  sink.write = [&outContent](const uint8_t* data, size_t len) { return outContent.write(data, len) == len; };
  return runGetSecure(url, username, password, sink) == OK;
}

bool HttpDownloader::fetchUrl(const std::string& url, std::string& outContent, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  outContent.clear();  // start clean; the sink appends, so don't carry prior content
  Sink sink;
  sink.write = [&outContent](const uint8_t* data, size_t len) {
    outContent.append(reinterpret_cast<const char*>(data), len);
    return true;
  };
  return runGetSecure(url, username, password, sink) == OK;
}

bool HttpDownloader::fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  Sink sink;
  sink.write = onData;
  return runGetSecure(url, username, password, sink) == OK;
}

HttpDownloader::DownloadError HttpDownloader::fetchBounded(const std::string& url, const uint32_t budgetMs,
                                                           const size_t maxBytes, const DataCallback& onData) {
  // No URL logging here: unattended callers may use a link that carries a secret path.
  Sink sink;
  sink.write = onData;
  sink.maxBytes = maxBytes;
  sink.timeoutMs = static_cast<int>(std::min<uint32_t>(budgetMs, HTTP_TIMEOUT_MS));
  sink.hasDeadline = true;
  sink.deadlineMs = millis() + budgetMs;
  return runGetSecure(url, "", "", sink);
}

bool HttpDownloader::fetchGithubReleaseAsset(const std::string& url, const std::string& expectedSha256,
                                             const DataCallback& onData) {
  Sink sink;
  sink.write = onData;
  return runGithubReleaseAsset(url, expectedSha256, sink) == OK;
}

HttpDownloader::DownloadError HttpDownloader::downloadGithubReleaseAssetToFile(
    const std::string& url, const std::string& expectedSha256, const std::string& destPath, ProgressCallback progress,
    bool* cancelFlag, const bool overwriteExisting, const size_t maxBytes) {
  LOG_DBG("HTTP", "Downloading verified GitHub asset: %s -> %s", url.c_str(), destPath.c_str());

  if (Storage.exists(destPath.c_str())) {
    if (!overwriteExisting) {
      LOG_ERR("HTTP", "Refusing to replace existing download destination");
      return FILE_ERROR;
    }
    if (!Storage.remove(destPath.c_str())) {
      LOG_ERR("HTTP", "Failed to remove stale destination before download");
      return FILE_ERROR;
    }
  }

  HalFile file;
  if (!Storage.openFileForWrite("HTTP", destPath.c_str(), file)) {
    LOG_ERR("HTTP", "Failed to open file for writing");
    return FILE_ERROR;
  }

  Sink sink;
  sink.progress = std::move(progress);
  sink.cancelFlag = cancelFlag;
  sink.maxBytes = maxBytes;
  sink.write = [&file](const uint8_t* data, const size_t len) { return file.write(data, len) == len; };

  const DownloadError result = runGithubReleaseAsset(url, expectedSha256, sink);
  bool synced = false;
  if (result == OK) {
    synced = file.sync();
  }
  const bool closed = file.close();

  if (result != OK || !synced || !closed) {
    Storage.remove(destPath.c_str());
    return result == OK ? FILE_ERROR : result;
  }
  if (sink.downloaded == 0) {
    LOG_ERR("HTTP", "no data received");
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }
  LOG_DBG("HTTP", "Downloaded and verified %zu bytes", sink.downloaded);
  return OK;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress, bool* cancelFlag,
                                                             const std::string& username, const std::string& password,
                                                             const bool overwriteExisting, const size_t maxBytes) {
  LOG_DBG("HTTP", "Downloading: %s -> %s", url.c_str(), destPath.c_str());

  if (Storage.exists(destPath.c_str())) {
    if (!overwriteExisting) {
      LOG_ERR("HTTP", "Refusing to replace existing download destination");
      return FILE_ERROR;
    }
    if (!Storage.remove(destPath.c_str())) {
      LOG_ERR("HTTP", "Failed to remove stale destination before download");
      return FILE_ERROR;
    }
  }
  HalFile file;
  if (!Storage.openFileForWrite("HTTP", destPath.c_str(), file)) {
    LOG_ERR("HTTP", "Failed to open file for writing");
    return FILE_ERROR;
  }

  Sink sink;
  sink.progress = std::move(progress);
  sink.cancelFlag = cancelFlag;
  sink.maxBytes = maxBytes;
  sink.write = [&file](const uint8_t* data, size_t len) { return file.write(data, len) == len; };

  const DownloadError result = runGetSecure(url, username, password, sink);
  // Close before any remove() on the same path; DESTRUCTOR_CLOSES_FILE would
  // otherwise close only after the remove.
  bool synced = false;
  if (result == OK) {
    synced = file.sync();
  }
  const bool closed = file.close();

  if (result != OK || !synced || !closed) {
    Storage.remove(destPath.c_str());
    return result == OK ? FILE_ERROR : result;
  }
  if (sink.downloaded == 0) {
    LOG_ERR("HTTP", "no data received");
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }
  LOG_DBG("HTTP", "Downloaded %zu bytes", sink.downloaded);
  return OK;
}
