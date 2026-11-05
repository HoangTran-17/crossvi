#include "SleepBriefFetch.h"

#include <ArduinoJson.h>
#include <HttpTransportPolicy.h>

#include <cstring>

#include "SleepBrief.h"
#include "SleepRefreshSchedule.h"

namespace sleep_brief_fetch {

Skip skipReason(const Inputs& in) {
  if (!in.settingOn) return Skip::SettingOff;
  if (!in.briefLayout) return Skip::NotBriefLayout;
  if (!in.hasSavedWifi) return Skip::NoSavedWifi;
  if (in.batteryPercent < sleep_refresh::MIN_BATTERY_PERCENT) return Skip::LowBattery;
  return Skip::None;
}

const char* skipName(const Skip reason) {
  switch (reason) {
    case Skip::None:
      return "none";
    case Skip::SettingOff:
      return "setting_off";
    case Skip::NotBriefLayout:
      return "not_brief_layout";
    case Skip::NoSavedWifi:
      return "no_saved_wifi";
    case Skip::LowBattery:
      return "low_battery";
  }
  return "unknown";
}

namespace {
bool isBlank(const char c) { return c == ' ' || c == '\t'; }
}  // namespace

ResolvedUrl resolveUrl(const char* text, const size_t length) {
  if (!text) return {DEFAULT_URL, UrlSource::Default};
  size_t begin = 0;
  // A UTF-8 byte order mark is common in files saved by Windows editors.
  if (length >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
      static_cast<unsigned char>(text[2]) == 0xBF) {
    begin = 3;
  }
  size_t end = begin;
  while (end < length && text[end] != '\n' && text[end] != '\r') ++end;
  while (begin < end && isBlank(text[begin])) ++begin;
  while (end > begin && isBlank(text[end - 1])) --end;
  if (begin == end) return {DEFAULT_URL, UrlSource::Default};

  const std::string line(text + begin, end - begin);
  bool plain = line.size() <= HttpTransportPolicy::MAX_URL_BYTES;
  for (const char c : line) plain = plain && static_cast<unsigned char>(c) > ' ' && c != 0x7F;
  if (!plain || !HttpTransportPolicy::isHttpsUrl(line)) return {DEFAULT_URL, UrlSource::FileRejected};
  return {line, UrlSource::File};
}

std::string toFetchUrl(const std::string& url) {
  constexpr char RAW_PREFIX[] = "https://gist.githubusercontent.com/";
  constexpr size_t RAW_PREFIX_LEN = sizeof(RAW_PREFIX) - 1;
  if (url.compare(0, RAW_PREFIX_LEN, RAW_PREFIX) != 0) return url;
  const size_t userEnd = url.find('/', RAW_PREFIX_LEN);
  if (userEnd == std::string::npos) return url;
  const size_t idEnd = url.find('/', userEnd + 1);
  if (idEnd == std::string::npos || url.compare(idEnd, 5, "/raw/") != 0) return url;
  const std::string id = url.substr(userEnd + 1, idEnd - userEnd - 1);
  if (id.empty() || id.find_first_not_of("0123456789abcdef") != std::string::npos) return url;
  return "https://api.github.com/gists/" + id;
}

bool isGistApiUrl(const std::string& url) { return url.rfind("https://api.github.com/gists/", 0) == 0; }

namespace {
size_t skipSpace(const char* json, size_t i, const size_t length) {
  while (i < length && (json[i] == ' ' || json[i] == '\n' || json[i] == '\r' || json[i] == '\t')) ++i;
  return i;
}
}  // namespace

JsonSpan gistFilesSpan(const char* json, const size_t length) {
  constexpr char KEY[] = "\"files\"";
  constexpr size_t KEY_LEN = sizeof(KEY) - 1;
  JsonSpan span;
  int depth = 0;
  bool inString = false;
  bool escaped = false;
  for (size_t i = 0; i < length; ++i) {
    const char c = json[i];
    if (inString) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        inString = false;
      }
      continue;
    }
    if (c == '"') {
      if (depth == 1 && span.begin == 0 && length - i >= KEY_LEN && std::memcmp(json + i, KEY, KEY_LEN) == 0) {
        // Only a key is followed by ':'; the same text as a value is skipped like any other string.
        size_t j = skipSpace(json, i + KEY_LEN, length);
        if (j < length && json[j] == ':') {
          j = skipSpace(json, j + 1, length);
          if (j >= length) return span;
          if (json[j] != '{') return {};
          span.begin = j;
          depth = 2;
          i = j;
          continue;
        }
      }
      inString = true;
    } else if (c == '{' || c == '[') {
      ++depth;
    } else if (c == '}' || c == ']') {
      --depth;
      if (span.begin != 0 && depth == 1) {
        span.end = i + 1;
        return span;
      }
    }
  }
  return span;
}

bool extractGistFile(const char* json, const size_t length, const char* name, std::string& out) {
  const JsonSpan span = gistFilesSpan(json, length);
  if (span.end == 0) return false;
  JsonDocument filter;
  filter[name]["content"] = true;
  filter[name]["truncated"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, json + span.begin, span.end - span.begin, DeserializationOption::Filter(filter))) {
    return false;
  }
  const JsonVariantConst file = doc[name];
  const char* content = file["content"];
  if (!content || file["truncated"].as<bool>()) return false;
  out = content;
  return true;
}

std::string hostOf(const std::string& url) {
  const size_t scheme = url.find("://");
  if (scheme == std::string::npos) return {};
  const size_t start = scheme + 3;
  const size_t authorityEnd = url.find_first_of("/?#", start);
  std::string authority = url.substr(start, authorityEnd == std::string::npos ? std::string::npos : authorityEnd - start);
  const size_t at = authority.rfind('@');
  if (at != std::string::npos) authority.erase(0, at + 1);
  size_t hostEnd = authority.size();
  if (!authority.empty() && authority[0] == '[') {
    const size_t close = authority.find(']');
    if (close != std::string::npos) hostEnd = close + 1;
  } else {
    const size_t colon = authority.find(':');
    if (colon != std::string::npos) hostEnd = colon;
  }
  authority.resize(hostEnd);
  if (authority.size() > 64) authority.resize(64);
  return authority;
}

Validation validateBrief(const char* json, const size_t length) {
  if (!json || length == 0) return Validation::Empty;
  if (length > sleep_brief::MAX_BRIEF_BYTES) return Validation::TooLarge;
  sleep_brief::Brief brief;
  switch (sleep_brief::parseBrief(json, length, brief)) {
    case sleep_brief::ParseStatus::Ok:
      return Validation::Ok;
    case sleep_brief::ParseStatus::TooLarge:
      return Validation::TooLarge;
    case sleep_brief::ParseStatus::InvalidJson:
    case sleep_brief::ParseStatus::UnsupportedVersion:
      break;
  }
  return Validation::Invalid;
}

const char* validationName(const Validation result) {
  switch (result) {
    case Validation::Ok:
      return "ok";
    case Validation::Empty:
      return "empty";
    case Validation::TooLarge:
      return "too_large";
    case Validation::Invalid:
      return "invalid";
  }
  return "unknown";
}

}  // namespace sleep_brief_fetch
