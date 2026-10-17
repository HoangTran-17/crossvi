#include "LunarCalendar.h"

#include <cmath>
#include <cstring>

namespace lunar {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTimeZone = 7.0;  // Vietnam, UTC+7. China uses +8, so Tết can differ.

// Names must stay inside lib/EpdFont/VietnameseFontContract.h (NFC only).
constexpr const char* kWeekdays[7] = {"Chủ Nhật", "Thứ Hai", "Thứ Ba", "Thứ Tư", "Thứ Năm", "Thứ Sáu", "Thứ Bảy"};
constexpr const char* kCan[10] = {"Giáp", "Ất", "Bính", "Đinh", "Mậu", "Kỷ", "Canh", "Tân", "Nhâm", "Quý"};
constexpr const char* kChi[12] = {"Tý", "Sửu", "Dần", "Mão", "Thìn", "Tỵ", "Ngọ", "Mùi", "Thân", "Dậu", "Tuất", "Hợi"};
constexpr const char* kSolarTerms[24] = {
    "Xuân Phân", "Thanh Minh", "Cốc Vũ",   "Lập Hạ",    "Tiểu Mãn",    "Mang Chủng", "Hạ Chí",    "Tiểu Thử",
    "Đại Thử",   "Lập Thu",    "Xử Thử",   "Bạch Lộ",   "Thu Phân",    "Hàn Lộ",     "Sương Giáng", "Lập Đông",
    "Tiểu Tuyết", "Đại Tuyết", "Đông Chí", "Tiểu Hàn",  "Đại Hàn",     "Lập Xuân",   "Vũ Thủy",   "Kinh Trập"};
constexpr const char* kTruc[12] = {"Kiến", "Trừ", "Mãn", "Bình", "Định", "Chấp",
                                   "Phá",  "Nguy", "Thành", "Thu", "Khai", "Bế"};
constexpr const char* kDeities[12] = {"Thanh Long", "Minh Đường", "Thiên Hình", "Chu Tước",  "Kim Quỹ",  "Kim Đường",
                                      "Bạch Hổ",    "Ngọc Đường", "Thiên Lao",  "Nguyên Vũ", "Tư Mệnh",  "Câu Trần"};
constexpr const char* kStars[28] = {"Giác", "Cang", "Đê",   "Phòng", "Tâm",  "Vĩ",    "Cơ",    "Đẩu",  "Ngưu", "Nữ",
                                    "Hư",   "Nguy", "Thất", "Bích",  "Khuê", "Lâu",   "Vị",    "Mão",  "Tất",  "Chủy",
                                    "Sâm",  "Tỉnh", "Quỷ",  "Liễu",  "Tinh", "Trương", "Dực",  "Chẩn"};

// Thanh Long, Minh Đường, Kim Quỹ, Kim Đường, Ngọc Đường, Tư Mệnh.
constexpr uint16_t kHoangDaoDeityMask = (1u << 0) | (1u << 1) | (1u << 4) | (1u << 5) | (1u << 7) | (1u << 10);
constexpr int kLapXuanTerm = 21;
// Calibrated so (jdn + kStarOffset) % 28 gives Tỉnh on 2026-10-08; this also
// pins Giác to Thursday, the fixed star/weekday pairing of the 28-day cycle.
constexpr int32_t kStarOffset = 11;

int floorDiv(double v) { return static_cast<int>(std::floor(v)); }

int32_t jdFromDate(int dd, int mm, int yy) {
  const int a = (14 - mm) / 12;
  const int y = yy + 4800 - a;
  const int m = mm + 12 * a - 3;
  int32_t jd = dd + (153 * m + 2) / 5 + 365 * y + y / 4 - y / 100 + y / 400 - 32045;
  if (jd < 2299161) jd = dd + (153 * m + 2) / 5 + 365 * y + y / 4 - 32083;  // Julian calendar
  return jd;
}

// Julian date (UT) of the k-th new moon after 1900-01-01.
double newMoon(int k) {
  const double T = k / 1236.85;
  const double T2 = T * T;
  const double T3 = T2 * T;
  const double dr = kPi / 180;
  double jd1 = 2415020.75933 + 29.53058868 * k + 0.0001178 * T2 - 0.000000155 * T3;
  jd1 += 0.00033 * std::sin((166.56 + 132.87 * T - 0.009173 * T2) * dr);
  const double M = 359.2242 + 29.10535608 * k - 0.0000333 * T2 - 0.00000347 * T3;
  const double Mpr = 306.0253 + 385.81691806 * k + 0.0107306 * T2 + 0.00001236 * T3;
  const double F = 21.2964 + 390.67050646 * k - 0.0016528 * T2 - 0.00000239 * T3;
  double c1 = (0.1734 - 0.000393 * T) * std::sin(M * dr) + 0.0021 * std::sin(2 * dr * M);
  c1 = c1 - 0.4068 * std::sin(Mpr * dr) + 0.0161 * std::sin(dr * 2 * Mpr);
  c1 = c1 - 0.0004 * std::sin(dr * 3 * Mpr);
  c1 = c1 + 0.0104 * std::sin(dr * 2 * F) - 0.0051 * std::sin(dr * (M + Mpr));
  c1 = c1 - 0.0074 * std::sin(dr * (M - Mpr)) + 0.0004 * std::sin(dr * (2 * F + M));
  c1 = c1 - 0.0004 * std::sin(dr * (2 * F - M)) - 0.0006 * std::sin(dr * (2 * F + Mpr));
  c1 = c1 + 0.0010 * std::sin(dr * (2 * F - Mpr)) + 0.0005 * std::sin(dr * (2 * Mpr + M));
  double deltat;
  if (T < -11) {
    deltat = 0.001 + 0.000839 * T + 0.0002261 * T2 - 0.00000845 * T3 - 0.000000081 * T * T3;
  } else {
    deltat = -0.000278 + 0.000265 * T + 0.000262 * T2;
  }
  return jd1 + c1 - deltat;
}

// Apparent solar longitude in degrees [0, 360) at Julian date `jd` (UT).
double sunLongitudeDeg(double jd) {
  const double T = (jd - 2451545.0) / 36525;
  const double T2 = T * T;
  const double dr = kPi / 180;
  const double M = 357.52910 + 35999.05030 * T - 0.0001559 * T2 - 0.00000048 * T * T2;
  const double L0 = 280.46645 + 36000.76983 * T + 0.0003032 * T2;
  double dl = (1.914600 - 0.004817 * T - 0.000014 * T2) * std::sin(dr * M);
  dl += (0.019993 - 0.000101 * T) * std::sin(dr * 2 * M) + 0.000290 * std::sin(dr * 3 * M);
  const double l = L0 + dl;
  return l - 360.0 * std::floor(l / 360.0);
}

// Major-term sector (0..11, 30° each) at the local midnight that starts `dayNumber`.
int sunSectorAtDayStart(int32_t dayNumber) {
  return floorDiv(sunLongitudeDeg(dayNumber - 0.5 - kTimeZone / 24) / 30.0);
}

int32_t newMoonDay(int k) { return floorDiv(newMoon(k) + 0.5 + kTimeZone / 24); }

int32_t lunarMonth11(int yy) {
  const int32_t off = jdFromDate(31, 12, yy) - 2415021;
  const int k = floorDiv(off / 29.530588853);
  int32_t nm = newMoonDay(k);
  if (sunSectorAtDayStart(nm) >= 9) nm = newMoonDay(k - 1);
  return nm;
}

int leapMonthOffset(int32_t a11) {
  const int k = floorDiv((a11 - 2415021.076998695) / 29.530588853 + 0.5);
  int last;
  int i = 1;
  int arc = sunSectorAtDayStart(newMoonDay(k + i));
  do {
    last = arc;
    ++i;
    arc = sunSectorAtDayStart(newMoonDay(k + i));
  } while (arc != last && i < 14);
  return i - 1;
}

void solarToLunar(int32_t dayNumber, int yy, DayInfo& out) {
  int k = floorDiv((dayNumber - 2415021.076998695) / 29.530588853) + 1;
  int32_t monthStart = newMoonDay(k);
  // The original steps back once; a true new moon lagging the mean estimate
  // needs a second step (e.g. 2054-05-07 would otherwise be lunar day 0).
  while (monthStart > dayNumber) monthStart = newMoonDay(--k);
  int32_t a11 = lunarMonth11(yy);
  int32_t b11 = a11;
  int lunarYear;
  if (a11 >= monthStart) {
    lunarYear = yy;
    a11 = lunarMonth11(yy - 1);
  } else {
    lunarYear = yy + 1;
    b11 = lunarMonth11(yy + 1);
  }
  const int diff = (monthStart - a11) / 29;
  bool leap = false;
  int lunarMonth = diff + 11;
  if (b11 - a11 > 365) {
    const int leapDiff = leapMonthOffset(a11);
    if (diff >= leapDiff) {
      lunarMonth = diff + 10;
      leap = diff == leapDiff;
    }
  }
  if (lunarMonth > 12) lunarMonth -= 12;
  if (lunarMonth >= 11 && diff < 4) lunarYear -= 1;

  out.lunarDay = static_cast<uint8_t>(dayNumber - monthStart + 1);
  out.lunarMonth = static_cast<uint8_t>(lunarMonth);
  out.lunarLeap = leap;
  out.lunarYear = static_cast<int16_t>(lunarYear);
}

void setCanChi(CanChi& cc, int can, int chi) {
  cc.can = static_cast<uint8_t>(can);
  cc.chi = static_cast<uint8_t>(chi);
  const size_t canLen = std::strlen(kCan[can]);
  const size_t chiLen = std::strlen(kChi[chi]);
  static_assert(sizeof(CanChi::name) >= 13, "longest pair is 12 bytes + NUL");
  std::memcpy(cc.name, kCan[can], canLen);
  cc.name[canLen] = ' ';
  std::memcpy(cc.name + canLen + 1, kChi[chi], chiLen + 1);
}

// Chi of the hour whose Thanh Long deity sits on day/month chi `chi`.
// Pairs Dần/Thân -> Tý, Mão/Dậu -> Dần, Thìn/Tuất -> Thìn, Tỵ/Hợi -> Ngọ, Ngọ/Tý -> Thân, Mùi/Sửu -> Tuất.
int thanhLongStart(int chi) { return ((chi + 10) % 6) * 2; }

uint16_t hoangDaoHourMask(int dayChi) {
  const int start = thanhLongStart(dayChi);
  uint16_t mask = 0;
  for (int deity = 0; deity < 12; ++deity) {
    if (kHoangDaoDeityMask & (1u << deity)) mask |= 1u << ((start + deity) % 12);
  }
  return mask;
}

bool isLeapYear(int y) {
  return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

int daysInMonth(int y, int m) {
  static constexpr uint8_t kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return (m == 2 && isLeapYear(y)) ? 29 : kDays[m - 1];
}

}  // namespace

bool computeDay(int year, int month, int day, DayInfo& out) {
  if (year < kMinYear || year > kMaxYear || month < 1 || month > 12) return false;
  if (day < 1 || day > daysInMonth(year, month)) return false;

  DayInfo info{};
  info.jdn = jdFromDate(day, month, year);
  info.weekday = static_cast<uint8_t>((info.jdn + 1) % 7);
  solarToLunar(info.jdn, year, info);

  const int dayChi = (info.jdn + 1) % 12;
  setCanChi(info.dayCanChi, (info.jdn + 9) % 10, dayChi);
  setCanChi(info.monthCanChi, (info.lunarYear * 12 + info.lunarMonth + 3) % 10, (info.lunarMonth + 1) % 12);
  setCanChi(info.yearCanChi, (info.lunarYear + 6) % 10, (info.lunarYear + 8) % 12);

  // Solar-term convention: sample the sun at the local midnight that ENDS the
  // day, so the day containing a term's instant (UTC+7) already belongs to the
  // new term and new solar month. That is the day trực repeats the previous one.
  const double endOfDayUt = info.jdn + 0.5 - kTimeZone / 24;
  info.solarTerm = static_cast<uint8_t>(floorDiv(sunLongitudeDeg(endOfDayUt) / 15.0));
  const int monthIndex = ((info.solarTerm + 24 - kLapXuanTerm) % 24) / 2;  // 0 = Dần month
  info.solarMonthChi = static_cast<uint8_t>((monthIndex + 2) % 12);

  info.truc = static_cast<uint8_t>((dayChi - info.solarMonthChi + 12) % 12);
  // Day deity uses the LUNAR month (leap month = base month), unlike trực: this
  // matches common Vietnamese calendars (xemlicham, ngaydep), which users compare against.
  const int lunarMonthChi = (info.lunarMonth + 1) % 12;
  info.deity = static_cast<uint8_t>((dayChi - thanhLongStart(lunarMonthChi) + 12) % 12);
  info.hoangDao = (kHoangDaoDeityMask & (1u << info.deity)) != 0;
  info.star = static_cast<uint8_t>((info.jdn + kStarOffset) % 28);
  info.hoangDaoHours = hoangDaoHourMask(dayChi);

  out = info;
  return true;
}

HourSlot currentOrNextHoangDaoHour(const DayInfo& info, int hour) {
  // Slot positions across the civil day: 0 = Tý (00-01), .., 11 = Hợi, 12 = next day's Tý (23-24).
  const int first = (hour + 1) / 2;
  for (int p = first; p < first + 12; ++p) {
    const int chi = p % 12;
    const uint16_t mask = p < 12 ? info.hoangDaoHours : hoangDaoHourMask((info.dayCanChi.chi + 1) % 12);
    if (mask & (1u << chi)) {
      return {static_cast<uint8_t>(chi), static_cast<uint8_t>(chi == 0 ? 23 : chi * 2 - 1), p == first};
    }
  }
  return {0, 23, false};  // unreachable: every day has 6 hoàng đạo hours
}

const char* weekdayName(uint8_t weekday) { return weekday < 7 ? kWeekdays[weekday] : ""; }
const char* canName(uint8_t can) { return can < 10 ? kCan[can] : ""; }
const char* chiName(uint8_t chi) { return chi < 12 ? kChi[chi] : ""; }
const char* solarTermName(uint8_t term) { return term < 24 ? kSolarTerms[term] : ""; }
const char* trucName(uint8_t truc) { return truc < 12 ? kTruc[truc] : ""; }
const char* deityName(uint8_t deity) { return deity < 12 ? kDeities[deity] : ""; }
const char* starName(uint8_t star) { return star < 28 ? kStars[star] : ""; }

}  // namespace lunar
