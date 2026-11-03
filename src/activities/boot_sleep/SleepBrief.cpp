#include "SleepBrief.h"

#include <ArduinoJson.h>
#include <LunarCalendar.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace sleep_brief {
namespace {

constexpr size_t MAX_SHORT = 32;
constexpr size_t MAX_LINE = 120;
constexpr size_t MAX_QUOTE_TEXT = 400;
constexpr size_t MAX_QUOTE_BY = 60;

// Control characters have no glyph; tabs and newlines become spaces. The cut
// never splits a UTF-8 sequence.
std::string cleanText(const char* text, size_t length, const size_t maxBytes) {
  std::string out;
  out.reserve(std::min(length, maxBytes));
  for (size_t i = 0; i < length; ++i) {
    unsigned char c = static_cast<unsigned char>(text[i]);
    if (c < 0x20) c = ' ';
    if (c == 0x7f) continue;
    if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
    out.push_back(static_cast<char>(c));
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  if (out.size() > maxBytes) {
    size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
    out.resize(cut);
    while (!out.empty() && out.back() == ' ') out.pop_back();
  }
  return out;
}

std::string textField(JsonVariantConst value, const size_t maxBytes) {
  if (!value.is<const char*>()) return {};
  const char* text = value.as<const char*>();
  return cleanText(text, std::strlen(text), maxBytes);
}

bool isDigits(const char* text, const size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (text[i] < '0' || text[i] > '9') return false;
  }
  return true;
}

// "2026-11-03T05:00:00+07:00" -> "05:00 03/11". The generator already wrote
// local time, so the offset is ignored rather than converted.
std::string formatUpdated(JsonVariantConst value, Brief& brief) {
  if (!value.is<const char*>()) return {};
  const char* s = value.as<const char*>();
  if (std::strlen(s) < 16) return {};
  if (!isDigits(s, 4) || s[4] != '-' || !isDigits(s + 5, 2) || s[7] != '-' || !isDigits(s + 8, 2) ||
      (s[10] != 'T' && s[10] != ' ') || !isDigits(s + 11, 2) || s[13] != ':' || !isDigits(s + 14, 2)) {
    return {};
  }
  const int year = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
  const int month = (s[5] - '0') * 10 + (s[6] - '0');
  const int day = (s[8] - '0') * 10 + (s[9] - '0');
  const int hour = (s[11] - '0') * 10 + (s[12] - '0');
  const int minute = (s[14] - '0') * 10 + (s[15] - '0');
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59) return {};
  brief.generatedYear = year;
  brief.generatedMonth = month;
  brief.generatedDay = day;
  char out[16];
  snprintf(out, sizeof(out), "%02d:%02d %02d/%02d", hour, minute, day, month);
  return out;
}

// Days since 1970-01-01 of a proleptic Gregorian date.
int64_t daysFromCivil(int64_t year, const int month, const int day) {
  year -= month <= 2;
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const int64_t yearOfEra = year - era * 400;
  const int64_t dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int64_t dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  return era * 146097 + dayOfEra - 719468;
}

struct BuiltinQuote {
  const char* text;
  const char* by;
};

// Public-domain proverbs and folk verses, plus Tôn Tử maxims in paraphrase.
constexpr BuiltinQuote BUILTIN_QUOTES[] = {
    {"Có công mài sắt, có ngày nên kim.", "Tục ngữ"},
    {"Đi một ngày đàng, học một sàng khôn.", "Tục ngữ"},
    {"Học ăn, học nói, học gói, học mở.", "Tục ngữ"},
    {"Uống nước nhớ nguồn.", "Tục ngữ"},
    {"Ăn quả nhớ kẻ trồng cây.", "Tục ngữ"},
    {"Thất bại là mẹ thành công.", "Tục ngữ"},
    {"Gần mực thì đen, gần đèn thì sáng.", "Tục ngữ"},
    {"Kiến tha lâu cũng đầy tổ.", "Tục ngữ"},
    {"Nước chảy đá mòn.", "Tục ngữ"},
    {"Đường dài mới biết ngựa hay.", "Tục ngữ"},
    {"Học thầy không tày học bạn.", "Tục ngữ"},
    {"Tốt gỗ hơn tốt nước sơn.", "Tục ngữ"},
    {"Đói cho sạch, rách cho thơm.", "Tục ngữ"},
    {"Lá lành đùm lá rách.", "Tục ngữ"},
    {"Ăn có nhai, nói có nghĩ.", "Tục ngữ"},
    {"Một điều nhịn, chín điều lành.", "Tục ngữ"},
    {"Có chí thì nên.", "Tục ngữ"},
    {"Không thầy đố mày làm nên.", "Tục ngữ"},
    {"Thua keo này, bày keo khác.", "Tục ngữ"},
    {"Một cây làm chẳng nên non, ba cây chụm lại nên hòn núi cao.", "Ca dao"},
    {"Bầu ơi thương lấy bí cùng, tuy rằng khác giống nhưng chung một giàn.", "Ca dao"},
    {"Lời nói chẳng mất tiền mua, lựa lời mà nói cho vừa lòng nhau.", "Ca dao"},
    {"Công cha như núi Thái Sơn, nghĩa mẹ như nước trong nguồn chảy ra.", "Ca dao"},
    {"Muốn sang thì bắc cầu kiều, muốn con hay chữ thì yêu lấy thầy.", "Ca dao"},
    {"Hiểu mình, hiểu người, trăm trận không lo thua.", "Tôn Tử"},
    {"Thắng mà không phải đánh mới là thượng sách.", "Tôn Tử"},
    {"Tính kỹ thì dễ thắng, tính sơ thì khó thắng, không tính thì thua.", "Tôn Tử"},
    {"Việc cần nhanh, nhưng nhanh trên nền chuẩn bị kỹ.", "Tôn Tử"},
    {"Người giỏi dắt được thế cuộc, không để thế cuộc dắt mình.", "Tôn Tử"},
    {"Đừng chờ đối thủ không đến; hãy lo mình luôn sẵn sàng.", "Tôn Tử"},
    {"Nước tránh chỗ cao mà chảy xuống thấp; việc khéo là tránh chỗ mạnh, đánh vào chỗ yếu.", "Tôn Tử"},
};
constexpr size_t BUILTIN_QUOTE_COUNT = sizeof(BUILTIN_QUOTES) / sizeof(BUILTIN_QUOTES[0]);

// Lunar 1/1 of the lunar year that starts in solar `year`; false when not found.
bool findTetJdn(const int year, int32_t& jdn) {
  // Tết always falls between 21 Jan and 21 Feb. Scan from 19 Jan; while still
  // in lunar month 12 no 1/1 can come before the 29th day, so jump there.
  int offset = 0;
  while (offset <= 36) {
    const int dayOfJan = 19 + offset;
    const int month = dayOfJan > 31 ? 2 : 1;
    const int day = dayOfJan > 31 ? dayOfJan - 31 : dayOfJan;
    lunar::DayInfo info{};
    if (!lunar::computeDay(year, month, day, info)) return false;
    if (info.lunarMonth == 1 && !info.lunarLeap && info.lunarDay == 1) {
      jdn = info.jdn;
      return true;
    }
    offset += (info.lunarMonth == 12 && info.lunarDay < 29) ? 29 - info.lunarDay : 1;
  }
  return false;
}

}  // namespace

ParseStatus parseBrief(const char* json, const size_t length, Brief& out) {
  out = Brief{};
  if (!json || length == 0) return ParseStatus::InvalidJson;
  if (length > MAX_BRIEF_BYTES) return ParseStatus::TooLarge;

  JsonDocument doc;
  if (deserializeJson(doc, json, length)) return ParseStatus::InvalidJson;
  if (!doc.is<JsonObject>()) return ParseStatus::InvalidJson;
  // Read through a const view so absent keys are never created in the document.
  const JsonVariantConst root = doc.as<JsonVariantConst>();
  if (!root["v"].is<int>() || root["v"].as<int>() != 1) return ParseStatus::UnsupportedVersion;

  out.updated = formatUpdated(root["generated"], out);

  for (JsonVariantConst item : root["weather"].as<JsonArrayConst>()) {
    if (out.weather.size() == MAX_WEATHER) break;
    Weather w{textField(item["city"], MAX_SHORT), textField(item["now"], MAX_SHORT),
              textField(item["range"], MAX_SHORT)};
    if (w.city.empty() || (w.now.empty() && w.range.empty())) continue;
    out.weather.push_back(std::move(w));
  }

  JsonVariantConst gold = root["gold"];
  if (gold.is<JsonObjectConst>()) {
    Gold g{textField(gold["name"], MAX_SHORT), textField(gold["unit"], MAX_SHORT), textField(gold["buy"], MAX_SHORT),
           textField(gold["sell"], MAX_SHORT), textField(gold["d1m"], MAX_SHORT),  textField(gold["d3m"], MAX_SHORT),
           textField(gold["d1y"], MAX_SHORT)};
    if (!g.buy.empty() && !g.sell.empty()) {
      if (g.name.empty()) g.name = "Nhẫn 9999";
      out.gold = std::move(g);
      out.hasGold = true;
    }
  }

  for (JsonVariantConst item : root["news"].as<JsonArrayConst>()) {
    if (out.news.size() == MAX_NEWS) break;
    std::string title = textField(item["t"], MAX_LINE);
    if (!title.empty()) out.news.push_back(std::move(title));
  }

  for (JsonVariantConst item : root["events"].as<JsonArrayConst>()) {
    if (out.events.size() == MAX_EVENTS) break;
    Event e{textField(item["time"], 8), textField(item["t"], MAX_LINE)};
    if (!e.text.empty()) out.events.push_back(std::move(e));
  }

  Quote quote{textField(root["quote"]["text"], MAX_QUOTE_TEXT), textField(root["quote"]["by"], MAX_QUOTE_BY)};
  if (!quote.text.empty()) {
    out.quote = std::move(quote);
    out.hasQuote = true;
  }
  return ParseStatus::Ok;
}

int generatedAgeDays(const Brief& brief, const int year, const int month, const int day) {
  if (brief.generatedYear == 0) return -1;
  const int64_t age =
      daysFromCivil(year, month, day) - daysFromCivil(brief.generatedYear, brief.generatedMonth, brief.generatedDay);
  return age < 0 || age > 36500 ? -1 : static_cast<int>(age);
}

std::string updatedLabel(const Brief& brief, const int year, const int month, const int day) {
  if (brief.updated.empty()) return {};
  const int age = generatedAgeDays(brief, year, month, day);
  if (age < 2) return "Cập nhật " + brief.updated;
  return "Cập nhật " + std::to_string(age) + " ngày trước - " + brief.updated;
}

bool pickQuoteFromText(const char* text, size_t length, const uint32_t dayOfYear, Quote& out) {
  if (!text || length == 0) return false;
  if (length >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
      static_cast<unsigned char>(text[2]) == 0xBF) {
    text += 3;
    length -= 3;
  }

  const auto forEachLine = [&](auto&& visit) {
    size_t start = 0;
    while (start < length) {
      size_t end = start;
      while (end < length && text[end] != '\n' && text[end] != '\r') ++end;
      const std::string line = cleanText(text + start, end - start, MAX_QUOTE_TEXT + MAX_QUOTE_BY + 8);
      if (!line.empty() && !visit(line)) return;
      start = end + 1;
    }
  };

  size_t count = 0;
  forEachLine([&](const std::string&) {
    ++count;
    return true;
  });
  if (count == 0) return false;

  const size_t wanted = (dayOfYear == 0 ? 0 : dayOfYear - 1) % count;
  size_t index = 0;
  std::string chosen;
  forEachLine([&](const std::string& line) {
    if (index++ != wanted) return true;
    chosen = line;
    return false;
  });

  static constexpr char SEPARATOR[] = " \xE2\x80\x94 ";  // " — "
  const size_t split = chosen.rfind(SEPARATOR);
  if (split != std::string::npos && split > 0 && split + 5 < chosen.size()) {
    out.text = cleanText(chosen.data(), split, MAX_QUOTE_TEXT);
    out.by = cleanText(chosen.data() + split + 5, chosen.size() - split - 5, MAX_QUOTE_BY);
  } else {
    out.text = cleanText(chosen.data(), chosen.size(), MAX_QUOTE_TEXT);
    out.by.clear();
  }
  return !out.text.empty();
}

size_t builtinQuoteCount() { return BUILTIN_QUOTE_COUNT; }

Quote builtinQuote(const uint32_t dayOfYear) {
  const BuiltinQuote& q = BUILTIN_QUOTES[(dayOfYear == 0 ? 0 : dayOfYear - 1) % BUILTIN_QUOTE_COUNT];
  return Quote{q.text, q.by};
}

uint32_t dayOfYear(const int year, const int month, const int day) {
  static constexpr int DAYS_BEFORE[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  if (month < 1 || month > 12 || day < 1 || day > 31) return 0;
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  return static_cast<uint32_t>(DAYS_BEFORE[month - 1] + day + (leap && month > 2 ? 1 : 0));
}

bool tetStatus(const int year, const int month, const int day, TetStatus& out) {
  out = TetStatus{};
  lunar::DayInfo today{};
  if (!lunar::computeDay(year, month, day, today)) return false;
  if (today.lunarMonth == 1 && !today.lunarLeap && today.lunarDay <= 3) {
    out.greeting = true;
    return true;
  }
  int32_t tet = 0;
  if (!findTetJdn(year, tet)) return false;
  // Past this year's Tết: count to the next one (a lunar 1/4 or later date).
  if (tet < today.jdn && !findTetJdn(year + 1, tet)) return false;
  out.daysUntil = static_cast<int>(tet - today.jdn);
  return true;
}

}  // namespace sleep_brief
