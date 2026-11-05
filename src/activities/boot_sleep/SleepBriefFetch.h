#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Host-testable rules for the morning download of brief.json on a timer wake. The device-side flow lives in
// SleepBriefFetcher.cpp.
namespace sleep_brief_fetch {

// The X3 runs out of heap verifying the RSA chains of github.io and gist.githubusercontent.com; api.github.com
// serves an ECDSA chain that fits. The contents API returns the raw file because HttpDownloader asks for it.
constexpr char DEFAULT_URL[] = "https://api.github.com/repos/HoangTran-17/x3-daily/contents/brief.json?ref=device";
// One line. Lets the user point the device at a private calendar link without putting it in the firmware.
constexpr char URL_FILE_PATH[] = "/.crosspoint/brief_url.txt";
constexpr char BRIEF_PATH[] = "/.crosspoint/brief.json";
constexpr char TEMP_PATH[] = "/.crosspoint/brief.json.tmp";

// Wi-Fi connect plus download must leave most of the 90 s timer-wake deadman for drawing.
constexpr uint32_t WIFI_TIMEOUT_MS = 15 * 1000;
constexpr uint32_t HTTP_TIMEOUT_MS = 20 * 1000;
constexpr size_t MAX_URL_FILE_BYTES = 2048 + 64;
// The gist API wraps the brief in metadata. The "files" object comes before the history, which grows every day, so
// the download stops once "files" is complete and never needs more than this.
constexpr size_t MAX_GIST_RESPONSE_BYTES = 16 * 1024;
constexpr char GIST_FILE_NAME[] = "brief.json";

enum class Skip : uint8_t { None, SettingOff, NotBriefLayout, NoSavedWifi, LowBattery };

struct Inputs {
  bool settingOn;
  bool briefLayout;
  bool hasSavedWifi;
  uint8_t batteryPercent;
};

// The caller has already decided that this timer wake draws the sleep screen.
Skip skipReason(const Inputs& in);
const char* skipName(Skip reason);

enum class UrlSource : uint8_t { Default, File, FileRejected };

struct ResolvedUrl {
  std::string url;
  UrlSource source;
};

// `text` is the content of URL_FILE_PATH, or nullptr when the file does not exist. Uses the first line. A line that is
// not a plain https URL is rejected and the default URL is used instead.
ResolvedUrl resolveUrl(const char* text, size_t length);

// A gist raw link (https://gist.githubusercontent.com/<user>/<id>/raw/...) becomes the gist API URL
// https://api.github.com/gists/<id>. Any other URL is returned unchanged.
std::string toFetchUrl(const std::string& url);
bool isGistApiUrl(const std::string& url);

struct JsonSpan {
  size_t begin = 0;
  size_t end = 0;  // 0 while the object is not complete
};

// Locates the value of the top-level "files" key of a gist API response, which may still be arriving.
JsonSpan gistFilesSpan(const char* json, size_t length);

// Copies the content of `name` from a gist "files" object. Fails when the file is missing or truncated by GitHub.
bool extractGistFile(const char* json, size_t length, const char* name, std::string& out);

// Host name only, safe to log: userinfo, port, path and query are dropped, and the result is capped at 64 bytes.
std::string hostOf(const std::string& url);

enum class Validation : uint8_t { Ok, Empty, TooLarge, Invalid };

// A download replaces the saved file only when this returns Ok.
Validation validateBrief(const char* json, size_t length);
const char* validationName(Validation result);

}  // namespace sleep_brief_fetch
