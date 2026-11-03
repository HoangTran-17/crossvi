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
