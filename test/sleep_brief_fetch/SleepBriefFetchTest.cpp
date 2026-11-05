#include <gtest/gtest.h>

#include <string>

#include "SleepBriefFetch.h"

using namespace sleep_brief_fetch;

namespace {

constexpr uint8_t OK_BATTERY = 80;

Inputs allowed() { return {true, true, true, OK_BATTERY}; }

ResolvedUrl resolve(const std::string& text) { return resolveUrl(text.data(), text.size()); }

}  // namespace

TEST(BriefFetchDecision, AllConditionsMetFetches) { EXPECT_EQ(skipReason(allowed()), Skip::None); }

TEST(BriefFetchDecision, EachMissingConditionSkips) {
  Inputs in = allowed();
  in.settingOn = false;
  EXPECT_EQ(skipReason(in), Skip::SettingOff);
  in = allowed();
  in.briefLayout = false;
  EXPECT_EQ(skipReason(in), Skip::NotBriefLayout);
  in = allowed();
  in.hasSavedWifi = false;
  EXPECT_EQ(skipReason(in), Skip::NoSavedWifi);
  in = allowed();
  in.batteryPercent = 9;
  EXPECT_EQ(skipReason(in), Skip::LowBattery);
}

TEST(BriefFetchDecision, BatteryThresholdIsInclusive) {
  Inputs in = allowed();
  in.batteryPercent = 10;
  EXPECT_EQ(skipReason(in), Skip::None);
  in.batteryPercent = 9;
  EXPECT_EQ(skipReason(in), Skip::LowBattery);
}

TEST(BriefFetchDecision, SkipNamesAreDistinctLogTokens) {
  EXPECT_STREQ(skipName(Skip::SettingOff), "setting_off");
  EXPECT_STREQ(skipName(Skip::NotBriefLayout), "not_brief_layout");
  EXPECT_STREQ(skipName(Skip::NoSavedWifi), "no_saved_wifi");
  EXPECT_STREQ(skipName(Skip::LowBattery), "low_battery");
}

TEST(BriefFetchBudget, StaysInsideTimerWakeDeadman) {
  // The deadman fires 90 s after wake; the draw and frame save need the rest.
  EXPECT_LE(WIFI_TIMEOUT_MS + HTTP_TIMEOUT_MS, 40u * 1000);
}

TEST(BriefFetchUrl, MissingFileUsesDefault) {
  const ResolvedUrl r = resolveUrl(nullptr, 0);
  EXPECT_EQ(r.url, DEFAULT_URL);
  EXPECT_EQ(r.source, UrlSource::Default);
}

TEST(BriefFetchUrl, EmptyOrBlankFileUsesDefault) {
  EXPECT_EQ(resolve("").source, UrlSource::Default);
  EXPECT_EQ(resolve("  \t\r\n").source, UrlSource::Default);
  EXPECT_EQ(resolve("").url, DEFAULT_URL);
}

TEST(BriefFetchUrl, FirstLineIsTrimmed) {
  const ResolvedUrl r = resolve("  https://example.com/s3cr3t/brief.json \r\nignored second line\n");
  EXPECT_EQ(r.source, UrlSource::File);
  EXPECT_EQ(r.url, "https://example.com/s3cr3t/brief.json");
}

TEST(BriefFetchUrl, Utf8BomIsSkipped) {
  const ResolvedUrl r = resolve("\xEF\xBB\xBFhttps://example.com/brief.json\n");
  EXPECT_EQ(r.source, UrlSource::File);
  EXPECT_EQ(r.url, "https://example.com/brief.json");
}

TEST(BriefFetchUrl, NonHttpsOrMalformedLineIsRejected) {
  for (const char* bad : {"http://example.com/brief.json", "ftp://example.com/x", "example.com/brief.json",
                          "https://exa mple.com/x", "javascript:alert(1)", "https://"}) {
    const ResolvedUrl r = resolve(bad);
    EXPECT_EQ(r.source, UrlSource::FileRejected) << bad;
    EXPECT_EQ(r.url, DEFAULT_URL) << bad;
  }
}

TEST(BriefFetchUrl, OverlongLineIsRejected) {
  const ResolvedUrl r = resolve("https://example.com/" + std::string(3000, 'a'));
  EXPECT_EQ(r.source, UrlSource::FileRejected);
}

TEST(BriefFetchUrl, HostOnlyLeavesNoSecret) {
  EXPECT_EQ(hostOf("https://hoangtran-17.github.io/x3-daily/brief.json"), "hoangtran-17.github.io");
  EXPECT_EQ(hostOf("https://example.com/private-token-123/brief.json?k=secret#frag"), "example.com");
  EXPECT_EQ(hostOf("https://user:pass@example.com:8443/x"), "example.com");
  EXPECT_EQ(hostOf("https://example.com?k=secret"), "example.com");
  EXPECT_EQ(hostOf("https://[::1]:8443/x"), "[::1]");
  EXPECT_EQ(hostOf("not a url"), "");
  EXPECT_LE(hostOf("https://" + std::string(200, 'h') + ".com/x").size(), 64u);
}

TEST(BriefFetchValidate, AcceptsMinimalAndFullFiles) {
  const std::string minimal = R"({"v":1})";
  EXPECT_EQ(validateBrief(minimal.data(), minimal.size()), Validation::Ok);
  const std::string full = R"({"v":1,"generated":"2026-11-03T05:00:00+07:00","news":[{"t":"Một"}]})";
  EXPECT_EQ(validateBrief(full.data(), full.size()), Validation::Ok);
}

TEST(BriefFetchValidate, RejectsEmptyOversizeAndBadJson) {
  EXPECT_EQ(validateBrief(nullptr, 0), Validation::Empty);
  EXPECT_EQ(validateBrief("", 0), Validation::Empty);
  const std::string big = R"({"v":1,"pad":")" + std::string(9000, 'x') + R"("})";
  EXPECT_EQ(validateBrief(big.data(), big.size()), Validation::TooLarge);
  const std::string exactlyMax = R"({"v":1,"pad":")" + std::string(8192 - 16, 'x') + R"("})";
  ASSERT_EQ(exactlyMax.size(), 8192u);
  EXPECT_EQ(validateBrief(exactlyMax.data(), exactlyMax.size()), Validation::Ok);
  const std::string truncated = R"({"v":1,"news":[{"t":"Mot")";
  EXPECT_EQ(validateBrief(truncated.data(), truncated.size()), Validation::Invalid);
  const std::string html = "<html><body>404 Not Found</body></html>";
  EXPECT_EQ(validateBrief(html.data(), html.size()), Validation::Invalid);
}

TEST(BriefFetchValidate, RejectsOtherSchemaVersions) {
  const std::string v2 = R"({"v":2})";
  EXPECT_EQ(validateBrief(v2.data(), v2.size()), Validation::Invalid);
  const std::string noVersion = R"({"news":[]})";
  EXPECT_EQ(validateBrief(noVersion.data(), noVersion.size()), Validation::Invalid);
  const std::string array = "[1,2]";
  EXPECT_EQ(validateBrief(array.data(), array.size()), Validation::Invalid);
}

namespace {

// Shape of GET /gists/<id>: "files" comes before "history". The description repeats the key text as a value.
const std::string GIST_RESPONSE = R"json({"url":"https://api.github.com/gists/8a4e84","id":"8a4e84",
  "description":"files","files":{"brief.json":{"filename":"brief.json","raw_url":"https://gist.githubusercontent.com/u/8a4e84/raw/brief.json",
  "size":40,"truncated":false,"content":"{\"v\":1,\"news\":[{\"t\":\"Một \\\"tin\\\" {mới}\"}]}"}},
  "public":false,"owner":{"login":"u"},"history":[{"version":"a"},{"version":"b"}],"truncated":false})json";

}  // namespace

TEST(BriefFetchGist, RawLinkBecomesApiUrl) {
  EXPECT_EQ(toFetchUrl("https://gist.githubusercontent.com/HoangTran-17/8a4e84ff/raw/brief.json"),
            "https://api.github.com/gists/8a4e84ff");
  EXPECT_EQ(toFetchUrl("https://gist.githubusercontent.com/u/8a4e84ff/raw/0123abcd/brief.json"),
            "https://api.github.com/gists/8a4e84ff");
  EXPECT_EQ(toFetchUrl("https://gist.githubusercontent.com/u/not-hex/raw/brief.json"),
            "https://gist.githubusercontent.com/u/not-hex/raw/brief.json");
  EXPECT_EQ(toFetchUrl("https://example.com/brief.json"), "https://example.com/brief.json");
  EXPECT_TRUE(isGistApiUrl("https://api.github.com/gists/8a4e84ff"));
  EXPECT_FALSE(isGistApiUrl(DEFAULT_URL));
}

TEST(BriefFetchGist, FilesSpanEndsBeforeHistory) {
  const JsonSpan span = gistFilesSpan(GIST_RESPONSE.data(), GIST_RESPONSE.size());
  ASSERT_NE(span.end, 0u);
  EXPECT_EQ(GIST_RESPONSE[span.begin], '{');
  EXPECT_EQ(GIST_RESPONSE[span.end - 1], '}');
  EXPECT_LT(span.end, GIST_RESPONSE.find("\"history\""));
}

TEST(BriefFetchGist, FilesSpanWaitsForMoreData) {
  // Ends after the brief.json object closes but before "files" closes.
  const size_t cut = GIST_RESPONSE.find("}},") + 1;
  EXPECT_EQ(gistFilesSpan(GIST_RESPONSE.data(), cut).end, 0u);
  EXPECT_EQ(gistFilesSpan(GIST_RESPONSE.data(), 10).end, 0u);
  // A complete prefix is enough: the tail after "files" never needs to arrive.
  const size_t complete = GIST_RESPONSE.find("\"public\"");
  EXPECT_NE(gistFilesSpan(GIST_RESPONSE.data(), complete).end, 0u);
}

TEST(BriefFetchGist, ExtractsAndUnescapesTheBrief) {
  std::string brief;
  ASSERT_TRUE(extractGistFile(GIST_RESPONSE.data(), GIST_RESPONSE.size(), GIST_FILE_NAME, brief));
  EXPECT_EQ(brief, R"({"v":1,"news":[{"t":"Một \"tin\" {mới}"}]})");
  EXPECT_EQ(validateBrief(brief.data(), brief.size()), Validation::Ok);
  EXPECT_FALSE(extractGistFile(GIST_RESPONSE.data(), GIST_RESPONSE.size(), "other.json", brief));
}

TEST(BriefFetchGist, RejectsTruncatedFileAndMissingFiles) {
  std::string truncated = GIST_RESPONSE;
  truncated.replace(truncated.find("\"truncated\":false"), 17, "\"truncated\":true");
  std::string brief;
  EXPECT_FALSE(extractGistFile(truncated.data(), truncated.size(), GIST_FILE_NAME, brief));
  const std::string noFiles = R"({"message":"Not Found","files":"none"})";
  EXPECT_FALSE(extractGistFile(noFiles.data(), noFiles.size(), GIST_FILE_NAME, brief));
}
