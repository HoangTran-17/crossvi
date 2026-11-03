#include <gtest/gtest.h>

#include <set>
#include <string>

#include "SleepBrief.h"

using namespace sleep_brief;

namespace {

const char* const FULL_JSON = R"({"v":1,"generated":"2026-11-03T05:00:00+07:00",
 "weather":[{"city":"TP.HCM","now":"Mưa rào","range":"25–32°"},{"city":"Hà Tĩnh","now":"Nhiều mây","range":"19–24°"},
            {"city":"Huế","now":"Nắng","range":"20–28°"}],
 "gold":{"name":"Nhẫn 9999","unit":"tr/lượng","buy":"145,2","sell":"148,2","d1m":"+3,1%","d3m":"+9,8%","d1y":"+41,5%"},
 "news":[{"t":"Một"},{"t":"Hai"},{"t":"Ba"},{"t":"Bốn"}],
 "events":[{"time":"09:00","t":"Họp nhóm"}],
 "quote":{"text":"Học, học nữa, học mãi.","by":"V. I. Lenin"},
 "extra":{"ignored":true}})";

ParseStatus parse(const std::string& json, Brief& out) { return parseBrief(json.data(), json.size(), out); }

}  // namespace

TEST(BriefParse, ValidFile) {
  Brief brief;
  ASSERT_EQ(parse(FULL_JSON, brief), ParseStatus::Ok);
  EXPECT_EQ(brief.updated, "05:00 03/11");
  ASSERT_EQ(brief.weather.size(), 2u);  // capped at two cities
  EXPECT_EQ(brief.weather[1].city, "Hà Tĩnh");
  EXPECT_EQ(brief.weather[0].range, "25–32°");
  ASSERT_TRUE(brief.hasGold);
  EXPECT_EQ(brief.gold.sell, "148,2");
  EXPECT_EQ(brief.gold.d1y, "+41,5%");
  ASSERT_EQ(brief.news.size(), 3u);  // capped at three items
  EXPECT_EQ(brief.news[2], "Ba");
  ASSERT_EQ(brief.events.size(), 1u);
  EXPECT_EQ(brief.events[0].time, "09:00");
  ASSERT_TRUE(brief.hasQuote);
  EXPECT_EQ(brief.quote.by, "V. I. Lenin");
  EXPECT_TRUE(brief.hasNetworkBlocks());
}

TEST(BriefParse, MissingFieldsDropOnlyTheirBlock) {
  Brief brief;
  ASSERT_EQ(parse(R"({"v":1,"weather":[{"now":"x"},{"city":"Huế"},{"city":"Hà Nội","now":"Mưa"}],
      "gold":{"buy":"1"},"news":[{"x":1},{"t":""},{"t":"Có tin"}],"events":[{"time":"09:00"}],"quote":{"by":"ai đó"}})",
                  brief),
            ParseStatus::Ok);
  EXPECT_TRUE(brief.updated.empty());
  ASSERT_EQ(brief.weather.size(), 1u);
  EXPECT_EQ(brief.weather[0].city, "Hà Nội");
  EXPECT_FALSE(brief.hasGold);  // sell is missing
  ASSERT_EQ(brief.news.size(), 1u);
  EXPECT_TRUE(brief.events.empty());
  EXPECT_FALSE(brief.hasQuote);
}

TEST(BriefParse, MinimalAndWrongTypes) {
  Brief brief;
  ASSERT_EQ(parse(R"({"v":1})", brief), ParseStatus::Ok);
  EXPECT_FALSE(brief.hasNetworkBlocks());
  ASSERT_EQ(parse(R"({"v":1,"weather":"x","gold":[1],"news":{"t":"a"},"events":5,"quote":"q","generated":7})", brief),
            ParseStatus::Ok);
  EXPECT_FALSE(brief.hasNetworkBlocks());
  EXPECT_FALSE(brief.hasQuote);
  ASSERT_EQ(parse(R"({"v":1,"generated":"2026-13-40T25:99:00"})", brief), ParseStatus::Ok);
  EXPECT_TRUE(brief.updated.empty());
}

TEST(BriefParse, GarbageAndBadVersion) {
  Brief brief;
  EXPECT_EQ(parse("not json at all", brief), ParseStatus::InvalidJson);
  EXPECT_EQ(parse("", brief), ParseStatus::InvalidJson);
  EXPECT_EQ(parse(R"({"v":1,"weather":[)", brief), ParseStatus::InvalidJson);  // truncated
  EXPECT_EQ(parse("[1,2,3]", brief), ParseStatus::InvalidJson);
  EXPECT_EQ(parse(std::string("\x00\xff\xfe{", 4), brief), ParseStatus::InvalidJson);
  EXPECT_EQ(parse(R"({"weather":[]})", brief), ParseStatus::UnsupportedVersion);
  EXPECT_EQ(parse(R"({"v":2})", brief), ParseStatus::UnsupportedVersion);
  EXPECT_EQ(parse(R"({"v":"1"})", brief), ParseStatus::UnsupportedVersion);
  EXPECT_FALSE(brief.hasNetworkBlocks());
  EXPECT_EQ(parseBrief(nullptr, 5, brief), ParseStatus::InvalidJson);
}

TEST(BriefParse, OverCapIsRejectedBeforeParsing) {
  std::string big = R"({"v":1,"pad":")";
  big.append(MAX_BRIEF_BYTES, 'a');
  big += R"("})";
  Brief brief;
  EXPECT_EQ(parse(big, brief), ParseStatus::TooLarge);
  // Exactly at the cap still parses.
  std::string edge = R"({"v":1,"pad":")";
  edge.append(MAX_BRIEF_BYTES - edge.size() - 2, 'a');
  edge += R"("})";
  ASSERT_EQ(edge.size(), MAX_BRIEF_BYTES);
  EXPECT_EQ(parse(edge, brief), ParseStatus::Ok);
}

TEST(BriefParse, LongTextIsClippedOnACharacterBoundary) {
  std::string json = R"({"v":1,"news":[{"t":")";
  for (int i = 0; i < 100; ++i) json += "ệ";  // 3 bytes each
  json += R"("}]})";
  Brief brief;
  ASSERT_EQ(parse(json, brief), ParseStatus::Ok);
  ASSERT_EQ(brief.news.size(), 1u);
  EXPECT_LE(brief.news[0].size(), 120u);
  EXPECT_EQ(brief.news[0].size() % 3, 0u);
}

TEST(BriefParse, ControlCharactersBecomeSpaces) {
  Brief brief;
  ASSERT_EQ(parse(R"({"v":1,"news":[{"t":"a\nb\t\tc"}]})", brief), ParseStatus::Ok);
  ASSERT_EQ(brief.news.size(), 1u);
  EXPECT_EQ(brief.news[0], "a b c");
}

TEST(QuotePicker, StableWithinADayAndAdvancesDaily) {
  const std::string file = "Một\nHai — Tác giả\r\n\n  \nBa\n";
  Quote a, b, c;
  ASSERT_TRUE(pickQuoteFromText(file.data(), file.size(), 100, a));
  ASSERT_TRUE(pickQuoteFromText(file.data(), file.size(), 100, b));
  EXPECT_EQ(a.text, b.text);
  EXPECT_EQ(a.by, b.by);
  std::set<std::string> seen;
  for (uint32_t doy = 1; doy <= 3; ++doy) {
    ASSERT_TRUE(pickQuoteFromText(file.data(), file.size(), doy, c));
    seen.insert(c.text);
  }
  EXPECT_EQ(seen, (std::set<std::string>{"Một", "Hai", "Ba"}));
  ASSERT_TRUE(pickQuoteFromText(file.data(), file.size(), 2, c));
  EXPECT_EQ(c.text, "Hai");
  EXPECT_EQ(c.by, "Tác giả");
  ASSERT_TRUE(pickQuoteFromText(file.data(), file.size(), 4, c));  // wraps
  EXPECT_EQ(c.text, "Một");
}

TEST(QuotePicker, BomCrlfAndEmptyInput) {
  const std::string file = "\xEF\xBB\xBFXin chào\r\nTạm biệt\r\n";
  Quote q;
  ASSERT_TRUE(pickQuoteFromText(file.data(), file.size(), 1, q));
  EXPECT_EQ(q.text, "Xin chào");
  EXPECT_FALSE(pickQuoteFromText("", 0, 1, q));
  const std::string blank = "\n\r\n   \n";
  EXPECT_FALSE(pickQuoteFromText(blank.data(), blank.size(), 1, q));
  EXPECT_FALSE(pickQuoteFromText(nullptr, 4, 1, q));
}

TEST(QuotePicker, AuthorSuffixNeedsSpacesAroundTheDash) {
  const std::string file = "Có-gạch—liền không tách\n";
  Quote q;
  ASSERT_TRUE(pickQuoteFromText(file.data(), file.size(), 1, q));
  EXPECT_EQ(q.text, "Có-gạch—liền không tách");
  EXPECT_TRUE(q.by.empty());
}

TEST(QuotePicker, BuiltinListIsStableAndComplete) {
  EXPECT_GE(builtinQuoteCount(), 30u);
  EXPECT_EQ(builtinQuote(50).text, builtinQuote(50).text);
  std::set<std::string> distinct;
  for (uint32_t doy = 1; doy <= builtinQuoteCount(); ++doy) {
    const Quote q = builtinQuote(doy);
    EXPECT_FALSE(q.text.empty());
    EXPECT_FALSE(q.by.empty());
    distinct.insert(q.text);
  }
  EXPECT_EQ(distinct.size(), builtinQuoteCount());
}

TEST(QuotePicker, DayOfYear) {
  EXPECT_EQ(dayOfYear(2026, 1, 1), 1u);
  EXPECT_EQ(dayOfYear(2026, 12, 31), 365u);
  EXPECT_EQ(dayOfYear(2028, 12, 31), 366u);
  EXPECT_EQ(dayOfYear(2028, 3, 1), 61u);
  EXPECT_EQ(dayOfYear(2026, 3, 1), 60u);
  EXPECT_EQ(dayOfYear(2026, 0, 1), 0u);
}

TEST(TetCountdown, KnownTetDates) {
  TetStatus s;
  // Tết Đinh Mùi 2027 is 2027-02-06.
  ASSERT_TRUE(tetStatus(2027, 2, 6, s));
  EXPECT_TRUE(s.greeting);
  ASSERT_TRUE(tetStatus(2027, 2, 5, s));
  EXPECT_FALSE(s.greeting);
  EXPECT_EQ(s.daysUntil, 1);
  ASSERT_TRUE(tetStatus(2027, 1, 30, s));
  EXPECT_EQ(s.daysUntil, 7);
  // Tết Bính Ngọ 2026 is 2026-02-17.
  ASSERT_TRUE(tetStatus(2026, 2, 16, s));
  EXPECT_EQ(s.daysUntil, 1);
  // Tết Mậu Thân 2028 is 2028-01-26.
  ASSERT_TRUE(tetStatus(2028, 1, 25, s));
  EXPECT_EQ(s.daysUntil, 1);
}

TEST(TetCountdown, GreetingCoversLunarFirstThreeDays) {
  TetStatus s;
  for (int day = 17; day <= 19; ++day) {
    ASSERT_TRUE(tetStatus(2026, 2, day, s));
    EXPECT_TRUE(s.greeting) << day;
    EXPECT_EQ(s.daysUntil, 0);
  }
  ASSERT_TRUE(tetStatus(2026, 2, 20, s));  // lunar 1/4
  EXPECT_FALSE(s.greeting);
  EXPECT_EQ(s.daysUntil, 351);
}

TEST(TetCountdown, FromTodayAndAcrossYearEnd) {
  TetStatus s;
  ASSERT_TRUE(tetStatus(2026, 11, 3, s));
  EXPECT_EQ(s.daysUntil, 95);
  ASSERT_TRUE(tetStatus(2026, 12, 31, s));
  EXPECT_EQ(s.daysUntil, 37);
  ASSERT_TRUE(tetStatus(2027, 1, 1, s));
  EXPECT_EQ(s.daysUntil, 36);
}

TEST(TetCountdown, CountsDownByOneEveryDayBeforeTet) {
  // Walks 2 Jan - 16 Feb 2026 (Tết is 17 Feb). Catches any gap in the
  // skip-ahead scan around the 19 Jan - 21 Feb window.
  TetStatus prev;
  ASSERT_TRUE(tetStatus(2026, 1, 1, prev));
  int month = 1;
  int day = 2;
  while (!(month == 2 && day == 17)) {
    TetStatus now;
    ASSERT_TRUE(tetStatus(2026, month, day, now));
    EXPECT_EQ(now.daysUntil, prev.daysUntil - 1) << month << "/" << day;
    prev = now;
    if (++day > (month == 1 ? 31 : 28)) {
      day = 1;
      ++month;
    }
  }
  EXPECT_EQ(prev.daysUntil, 1);
}

TEST(TetCountdown, OutOfRangeDates) {
  TetStatus s;
  EXPECT_FALSE(tetStatus(1799, 6, 1, s));
  EXPECT_FALSE(tetStatus(2199, 12, 31, s));  // next Tết is beyond the library range
  EXPECT_FALSE(tetStatus(2026, 2, 30, s));
}
