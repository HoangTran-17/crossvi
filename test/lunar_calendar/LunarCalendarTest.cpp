#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "lib/EpdFont/VietnameseFontContract.h"
#include "lib/LunarCalendar/LunarCalendar.h"

// Oracle sources (independent from this code):
// - Cook firmware screenshot, 2026-09-05: "24 tháng 7 năm Bính Ngọ", day Nhâm Ngọ,
//   month Bính Thân, trực Khai, Hắc đạo.
// - https://www.xemlicham.com/am-lich/nam/2026/thang/9/ngay/5   (Vị, Khai, Bạch Hổ Hắc Đạo)
// - https://www.xemlicham.com/am-lich/nam/2026/thang/10/ngay/7  (Sâm, Chấp, Thanh Long Hoàng Đạo)
// - https://www.xemlicham.com/am-lich/nam/2026/thang/10/ngay/8  (Tỉnh, Chấp, tiết Hàn Lộ)
// - https://www.xemlicham.com/am-lich/nam/2026/thang/10/ngay/9  (Quỷ, Phá)
// - https://www.xemlicham.com/am-lich/nam/2026/thang/10/ngay/17 (Đê, Mãn, Thiên Lao Hắc Đạo)
// Known disagreements, recorded on purpose:
// - baohatinh.vn gives 2026-10-08 trực Phá (moves the repeat to 10-09) and
//   2026-09-05 lunar day 25; xemlicham and ngaydep.com give Chấp and 24.
// - chogia.vn gives 2026-09-05 star Tâm, which breaks the star/weekday pairing.
// - Trực uses the solar-term month but the day deity uses the LUNAR month, as
//   xemlicham and ngaydep do (2026-10-08: trực Chấp repeats, deity Minh Đường).

namespace {

lunar::DayInfo day(int y, int m, int d) {
  lunar::DayInfo info{};
  EXPECT_TRUE(lunar::computeDay(y, m, d, info)) << y << "-" << m << "-" << d;
  return info;
}

bool nextDate(int& y, int& m, int& d) {
  lunar::DayInfo probe{};
  if (lunar::computeDay(y, m, d + 1, probe)) {
    ++d;
  } else if (m < 12) {
    ++m;
    d = 1;
  } else {
    ++y;
    m = 1;
    d = 1;
  }
  return lunar::computeDay(y, m, d, probe);
}

uint16_t hourMask(std::initializer_list<int> chis) {
  uint16_t mask = 0;
  for (int c : chis) mask |= 1u << c;
  return mask;
}

enum Chi { Ty = 0, Suu, Dan, Mao, Thin, Ti, Ngo, Mui, Than, Dau, Tuat, Hoi };

}  // namespace

TEST(LunarCalendarTest, JulianDayAndWeekday) {
  const auto info = day(2000, 1, 1);
  EXPECT_EQ(info.jdn, 2451545);
  EXPECT_EQ(info.weekday, 6);
  EXPECT_STREQ(lunar::weekdayName(info.weekday), "Thứ Bảy");
}

TEST(LunarCalendarTest, CookScreenshotAnchor) {
  const auto info = day(2026, 9, 5);
  EXPECT_STREQ(lunar::weekdayName(info.weekday), "Thứ Bảy");
  EXPECT_EQ(info.lunarDay, 24);
  EXPECT_EQ(info.lunarMonth, 7);
  EXPECT_FALSE(info.lunarLeap);
  EXPECT_EQ(info.lunarYear, 2026);
  EXPECT_STREQ(info.yearCanChi.name, "Bính Ngọ");
  EXPECT_STREQ(info.dayCanChi.name, "Nhâm Ngọ");
  EXPECT_STREQ(info.monthCanChi.name, "Bính Thân");
  EXPECT_STREQ(lunar::trucName(info.truc), "Khai");
  EXPECT_FALSE(info.hoangDao);
  EXPECT_STREQ(lunar::deityName(info.deity), "Bạch Hổ");
  EXPECT_STREQ(lunar::starName(info.star), "Vị");
  EXPECT_STREQ(lunar::solarTermName(info.solarTerm), "Xử Thử");
  EXPECT_EQ(info.hoangDaoHours, hourMask({Ty, Suu, Mao, Ngo, Than, Dau}));
}

TEST(LunarCalendarTest, XemLichAmReferenceDays) {
  auto info = day(2026, 10, 7);
  EXPECT_EQ(info.lunarDay, 27);
  EXPECT_EQ(info.lunarMonth, 8);
  EXPECT_STREQ(info.dayCanChi.name, "Giáp Dần");
  EXPECT_STREQ(info.monthCanChi.name, "Đinh Dậu");
  EXPECT_STREQ(lunar::starName(info.star), "Sâm");
  EXPECT_STREQ(lunar::trucName(info.truc), "Chấp");
  EXPECT_STREQ(lunar::deityName(info.deity), "Thanh Long");
  EXPECT_TRUE(info.hoangDao);
  EXPECT_EQ(info.hoangDaoHours, hourMask({Ty, Suu, Thin, Ti, Mui, Tuat}));

  info = day(2026, 10, 17);
  EXPECT_EQ(info.lunarDay, 8);
  EXPECT_EQ(info.lunarMonth, 9);
  EXPECT_STREQ(info.dayCanChi.name, "Giáp Tý");
  EXPECT_STREQ(info.monthCanChi.name, "Mậu Tuất");
  EXPECT_STREQ(lunar::starName(info.star), "Đê");
  EXPECT_STREQ(lunar::trucName(info.truc), "Mãn");
  EXPECT_STREQ(lunar::deityName(info.deity), "Thiên Lao");
  EXPECT_FALSE(info.hoangDao);
  EXPECT_STREQ(lunar::solarTermName(info.solarTerm), "Hàn Lộ");
}

TEST(LunarCalendarTest, TrucRepeatsOnHanLoTermDay) {
  // Hàn Lộ (195°) falls during 2026-10-08 local time.
  const auto before = day(2026, 10, 7);
  const auto termDay = day(2026, 10, 8);
  const auto after = day(2026, 10, 9);
  EXPECT_STREQ(lunar::solarTermName(before.solarTerm), "Thu Phân");
  EXPECT_STREQ(lunar::solarTermName(termDay.solarTerm), "Hàn Lộ");
  EXPECT_EQ(before.solarMonthChi, Dau);
  EXPECT_EQ(termDay.solarMonthChi, Tuat);
  EXPECT_STREQ(lunar::trucName(before.truc), "Chấp");
  EXPECT_STREQ(lunar::trucName(termDay.truc), "Chấp");
  EXPECT_STREQ(lunar::trucName(after.truc), "Phá");
  EXPECT_STREQ(lunar::starName(termDay.star), "Tỉnh");
  EXPECT_STREQ(lunar::starName(after.star), "Quỷ");
  EXPECT_STREQ(lunar::deityName(termDay.deity), "Minh Đường");
  EXPECT_TRUE(termDay.hoangDao);
  EXPECT_EQ(termDay.hoangDaoHours, hourMask({Ty, Dan, Mao, Ngo, Mui, Dau}));
  EXPECT_EQ(after.hoangDaoHours, hourMask({Dan, Thin, Ti, Than, Dau, Hoi}));
}

TEST(LunarCalendarTest, VietnameseNewYearDays) {
  const int tet[][3] = {{1985, 1, 21}, {2007, 2, 17}, {2023, 1, 22}, {2024, 2, 10}, {2025, 1, 29},
                        {2026, 2, 17}, {2027, 2, 6},  {2028, 1, 26}, {2030, 2, 2}};
  for (const auto& t : tet) {
    const auto info = day(t[0], t[1], t[2]);
    EXPECT_EQ(info.lunarDay, 1) << t[0];
    EXPECT_EQ(info.lunarMonth, 1) << t[0];
    EXPECT_FALSE(info.lunarLeap) << t[0];
    EXPECT_EQ(info.lunarYear, t[0]) << t[0];
  }
  // Chinese Tết dates (UTC+8) differ from the UTC+7 result.
  auto info = day(1985, 2, 20);
  EXPECT_EQ(info.lunarMonth, 2);
  EXPECT_EQ(info.lunarDay, 1);
  info = day(2007, 2, 18);
  EXPECT_EQ(info.lunarMonth, 1);
  EXPECT_EQ(info.lunarDay, 2);
  // 2030-02-03 is the Chinese date; VN is 2030-02-02 per
  // https://www.xemlicham.com/am-lich/nam/2030/thang/2/ngay/2 (new moon ~23:07 UTC+7).
  info = day(2030, 2, 3);
  EXPECT_EQ(info.lunarMonth, 1);
  EXPECT_EQ(info.lunarDay, 2);
}

TEST(LunarCalendarTest, NewMoonLaggingMeanEstimate) {
  // Ho Ngoc Duc's single step-back yields lunar day 0 here.
  auto info = day(2054, 5, 7);
  EXPECT_EQ(info.lunarDay, 30);
  EXPECT_EQ(info.lunarMonth, 3);
  info = day(2054, 5, 8);
  EXPECT_EQ(info.lunarDay, 1);
  EXPECT_EQ(info.lunarMonth, 4);
}

TEST(LunarCalendarTest, YearCanChi) {
  EXPECT_STREQ(day(1985, 6, 1).yearCanChi.name, "Ất Sửu");
  EXPECT_STREQ(day(2024, 6, 1).yearCanChi.name, "Giáp Thìn");
  EXPECT_STREQ(day(2025, 6, 1).yearCanChi.name, "Ất Tỵ");
  // Before Tết the lunar year is still the previous one.
  EXPECT_STREQ(day(2026, 2, 16).yearCanChi.name, "Ất Tỵ");
}

TEST(LunarCalendarTest, LeapMonths) {
  const int expected[][2] = {{2020, 4}, {2023, 2}, {2025, 6}, {2028, 5}, {2024, 0}, {2026, 0}};
  for (const auto& e : expected) {
    int leapMonth = 0;
    int leapDays = 0;
    int y = e[0], m = 1, d = 1;
    while (y == e[0]) {
      const auto info = day(y, m, d);
      if (info.lunarLeap) {
        leapMonth = info.lunarMonth;
        ++leapDays;
      }
      nextDate(y, m, d);
    }
    EXPECT_EQ(leapMonth, e[1]) << e[0];
    if (e[1] != 0) {
      EXPECT_GE(leapDays, 29) << e[0];
      EXPECT_LE(leapDays, 30) << e[0];
    }
  }
}

TEST(LunarCalendarTest, LeapMonthKeepsBaseMonthCanChi) {
  // 2025 leap month 6 starts 2025-07-25.
  const auto info = day(2025, 7, 25);
  EXPECT_TRUE(info.lunarLeap);
  EXPECT_EQ(info.lunarMonth, 6);
  EXPECT_EQ(info.lunarDay, 1);
  EXPECT_STREQ(info.monthCanChi.name, day(2025, 7, 1).monthCanChi.name);
}

TEST(LunarCalendarTest, ContinuityAndCycles1900To2150) {
  int y = 1900, m = 1, d = 1;
  lunar::DayInfo prev = day(y, m, d);
  int monthLength = 0;
  int termChanges = 0;
  while (y <= 2150) {
    ASSERT_TRUE(nextDate(y, m, d));
    const lunar::DayInfo cur = day(y, m, d);
    ASSERT_EQ(cur.jdn, prev.jdn + 1);
    ASSERT_EQ(cur.weekday, (prev.weekday + 1) % 7);
    ASSERT_EQ(cur.star, (prev.star + 1) % 28);
    ASSERT_EQ(cur.dayCanChi.chi, (prev.dayCanChi.chi + 1) % 12);
    ASSERT_EQ(cur.dayCanChi.can, (prev.dayCanChi.can + 1) % 10);
    // Giác is always Thursday; Phòng, Hư, Mão, Tinh always Sunday.
    if (cur.star == 0) {
      ASSERT_EQ(cur.weekday, 4);
    }
    if (cur.star == 3 || cur.star == 10 || cur.star == 17 || cur.star == 24) {
      ASSERT_EQ(cur.weekday, 0);
    }

    ++monthLength;
    if (cur.lunarDay == 1) {
      ASSERT_TRUE(monthLength == 29 || monthLength == 30) << y << "-" << m << "-" << d;
      monthLength = 0;
    } else {
      ASSERT_EQ(cur.lunarDay, prev.lunarDay + 1) << y << "-" << m << "-" << d;
      ASSERT_EQ(cur.lunarMonth, prev.lunarMonth);
      ASSERT_EQ(cur.lunarLeap, prev.lunarLeap);
    }

    if (cur.solarTerm == prev.solarTerm) {
      ASSERT_EQ(cur.truc, (prev.truc + 1) % 12);
    } else {
      ASSERT_EQ(cur.solarTerm, (prev.solarTerm + 1) % 24);
      ++termChanges;
      // A new solar month (every other term) makes trực repeat the previous day.
      if (cur.solarMonthChi != prev.solarMonthChi) {
        ASSERT_EQ(cur.truc, prev.truc) << y << "-" << m << "-" << d;
      } else {
        ASSERT_EQ(cur.truc, (prev.truc + 1) % 12);
      }
    }
    ASSERT_EQ(cur.hoangDao, (cur.deity == 0 || cur.deity == 1 || cur.deity == 4 || cur.deity == 5 ||
                             cur.deity == 7 || cur.deity == 10));
    prev = cur;
  }
  EXPECT_NEAR(termChanges, 251 * 24, 2);
}

TEST(LunarCalendarTest, CurrentOrNextHoangDaoHour) {
  const auto termDay = day(2026, 10, 8);  // Ất Mão: Tý, Dần, Mão, Ngọ, Mùi, Dậu
  auto slot = lunar::currentOrNextHoangDaoHour(termDay, 4);
  EXPECT_EQ(slot.chi, Dan);
  EXPECT_EQ(slot.startHour, 3);
  EXPECT_TRUE(slot.current);

  slot = lunar::currentOrNextHoangDaoHour(termDay, 8);  // Thìn -> next Ngọ
  EXPECT_EQ(slot.chi, Ngo);
  EXPECT_EQ(slot.startHour, 11);
  EXPECT_FALSE(slot.current);

  slot = lunar::currentOrNextHoangDaoHour(termDay, 0);
  EXPECT_EQ(slot.chi, Ty);
  EXPECT_TRUE(slot.current);

  // 22:00 Hợi is not hoàng đạo; the next day (Bính Thìn) has no Tý/Sửu, so Dần.
  slot = lunar::currentOrNextHoangDaoHour(termDay, 22);
  EXPECT_EQ(slot.chi, Dan);
  EXPECT_FALSE(slot.current);

  // 23:00 already belongs to the next day's Tý.
  slot = lunar::currentOrNextHoangDaoHour(day(2026, 10, 7), 23);  // next day Ất Mão has Tý
  EXPECT_EQ(slot.chi, Ty);
  EXPECT_EQ(slot.startHour, 23);
  EXPECT_TRUE(slot.current);
  slot = lunar::currentOrNextHoangDaoHour(termDay, 23);
  EXPECT_EQ(slot.chi, Dan);
  EXPECT_FALSE(slot.current);
}

TEST(LunarCalendarTest, RejectsInvalidDates) {
  lunar::DayInfo info{};
  EXPECT_FALSE(lunar::computeDay(2026, 2, 29, info));
  EXPECT_FALSE(lunar::computeDay(2026, 13, 1, info));
  EXPECT_FALSE(lunar::computeDay(2026, 4, 31, info));
  EXPECT_FALSE(lunar::computeDay(2026, 1, 0, info));
  EXPECT_FALSE(lunar::computeDay(lunar::kMinYear - 1, 12, 31, info));
  EXPECT_FALSE(lunar::computeDay(lunar::kMaxYear + 1, 1, 1, info));
  EXPECT_FALSE(lunar::computeDay(1900, 2, 29, info));
  EXPECT_TRUE(lunar::computeDay(2000, 2, 29, info));
  EXPECT_TRUE(lunar::computeDay(2024, 2, 29, info));
  EXPECT_TRUE(lunar::computeDay(lunar::kMinYear, 1, 1, info));
  EXPECT_TRUE(lunar::computeDay(lunar::kMaxYear, 12, 31, info));
}

namespace {

bool inFontContract(uint32_t cp) {
  for (const auto& r : VietnameseFontContract::REQUIRED_RANGES) {
    if (cp >= r.first && cp <= r.last) return true;
  }
  for (uint32_t s : VietnameseFontContract::REQUIRED_SINGLETONS) {
    if (cp == s) return true;
  }
  return false;
}

// Requires precomposed (NFC) text: combining marks are in the contract but
// would mean the table was saved decomposed.
void expectRenderable(const char* text) {
  ASSERT_NE(text, nullptr);
  ASSERT_GT(std::strlen(text), 0u);
  const auto* p = reinterpret_cast<const unsigned char*>(text);
  while (*p) {
    uint32_t cp;
    int extra;
    if (*p < 0x80) {
      cp = *p;
      extra = 0;
    } else if ((*p & 0xE0) == 0xC0) {
      cp = *p & 0x1F;
      extra = 1;
    } else if ((*p & 0xF0) == 0xE0) {
      cp = *p & 0x0F;
      extra = 2;
    } else {
      FAIL() << "unexpected UTF-8 lead byte in " << text;
    }
    ++p;
    for (int i = 0; i < extra; ++i, ++p) {
      ASSERT_EQ(*p & 0xC0, 0x80) << text;
      cp = (cp << 6) | (*p & 0x3F);
    }
    EXPECT_TRUE(inFontContract(cp)) << "U+" << std::hex << cp << " in " << text;
    EXPECT_FALSE(cp >= 0x0300 && cp <= 0x036F) << "combining mark in " << text;
  }
}

}  // namespace

TEST(LunarCalendarTest, NamesFitVietnameseFontContract) {
  for (uint8_t i = 0; i < 7; ++i) expectRenderable(lunar::weekdayName(i));
  for (uint8_t i = 0; i < 10; ++i) expectRenderable(lunar::canName(i));
  for (uint8_t i = 0; i < 12; ++i) {
    expectRenderable(lunar::chiName(i));
    expectRenderable(lunar::trucName(i));
    expectRenderable(lunar::deityName(i));
  }
  for (uint8_t i = 0; i < 24; ++i) expectRenderable(lunar::solarTermName(i));
  for (uint8_t i = 0; i < 28; ++i) expectRenderable(lunar::starName(i));
  EXPECT_STREQ(lunar::starName(28), "");

  // Every can chi pair fits CanChi::name; 60 consecutive days cover all pairs.
  int y = 2026, m = 1, d = 1;
  for (int i = 0; i < 60; ++i) {
    const auto info = day(y, m, d);
    expectRenderable(info.dayCanChi.name);
    EXPECT_LT(std::strlen(info.dayCanChi.name), sizeof(info.dayCanChi.name));
    nextDate(y, m, d);
  }
}
