#pragma once

#include <cstdint>

// Vietnamese perpetual calendar (lịch vạn niên), offline and heap-free.
// Lunar conversion follows Ho Ngoc Duc's public algorithm at UTC+7.
namespace lunar {

inline constexpr int kMinYear = 1800;
inline constexpr int kMaxYear = 2199;

struct CanChi {
  uint8_t can;    // 0 = Giáp .. 9 = Quý
  uint8_t chi;    // 0 = Tý .. 11 = Hợi
  char name[16];  // NUL-terminated UTF-8 (NFC), e.g. "Nhâm Ngọ"
};

struct DayInfo {
  int32_t jdn;  // Julian day number of the solar date
  uint8_t weekday;  // 0 = Chủ Nhật .. 6 = Thứ Bảy

  uint8_t lunarDay;    // 1..30
  uint8_t lunarMonth;  // 1..12
  bool lunarLeap;      // true for the intercalary (nhuận) month
  int16_t lunarYear;

  CanChi dayCanChi;
  CanChi monthCanChi;  // by lunar month; a leap month reuses its base month
  CanChi yearCanChi;   // by lunar year

  uint8_t solarTerm;      // tiết khí, 0 = Xuân Phân (0°) .. 23 = Kinh Trập (345°)
  uint8_t solarMonthChi;  // chi of the solar-term month: Lập Xuân starts Dần (2)
  uint8_t truc;           // 0 = Kiến .. 11 = Bế
  uint8_t deity;          // 0 = Thanh Long .. 11 = Câu Trần, by lunar month
  bool hoangDao;          // deity is a hoàng đạo deity
  uint8_t star;           // nhị thập bát tú, 0 = Giác .. 27 = Chẩn
  uint16_t hoangDaoHours;  // bit i set = hour chi i is hoàng đạo for this day
};

// Fills `out` for the solar date. Returns false when the date is invalid or
// outside [kMinYear, kMaxYear]; `out` is left untouched then.
bool computeDay(int year, int month, int day, DayInfo& out);

struct HourSlot {
  uint8_t chi;        // 0 = Tý .. 11 = Hợi
  uint8_t startHour;  // local start hour: Tý = 23, Sửu = 1, .. Hợi = 21
  bool current;       // true when `hour` already falls inside this slot
};

// Hoàng đạo hour that contains local `hour` (0..23), else the next one.
// 23:00 opens the next day's Tý, so it is judged by the next day's chi.
HourSlot currentOrNextHoangDaoHour(const DayInfo& info, int hour);

const char* weekdayName(uint8_t weekday);
const char* canName(uint8_t can);
const char* chiName(uint8_t chi);
const char* solarTermName(uint8_t term);
const char* trucName(uint8_t truc);
const char* deityName(uint8_t deity);
const char* starName(uint8_t star);

}  // namespace lunar
