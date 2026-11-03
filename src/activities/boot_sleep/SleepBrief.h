#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Data side of the "Brief" and "Daily review" sleep presets. Everything here is
// pure (no storage, no display) so host tests can cover it.
namespace sleep_brief {

// brief.json is a cache written by a companion tool; anything bigger is not ours.
inline constexpr size_t MAX_BRIEF_BYTES = 8 * 1024;
inline constexpr size_t MAX_WEATHER = 2;
inline constexpr size_t MAX_NEWS = 3;
inline constexpr size_t MAX_EVENTS = 3;

struct Quote {
  std::string text;
  std::string by;
};

struct Weather {
  std::string city;
  std::string now;
  std::string range;
};

struct Gold {
  std::string name;
  std::string unit;
  std::string buy;
  std::string sell;
  std::string d1m;
  std::string d3m;
  std::string d1y;
};

struct Event {
  std::string time;
  std::string text;
};

struct Brief {
  std::string updated;  // "HH:MM dd/mm", empty when `generated` is absent or malformed
  std::vector<Weather> weather;
  bool hasGold = false;
  Gold gold;
  std::vector<std::string> news;
  std::vector<Event> events;
  bool hasQuote = false;
  Quote quote;

  bool hasNetworkBlocks() const { return !weather.empty() || hasGold || !news.empty() || !events.empty(); }
};

enum class ParseStatus : uint8_t { Ok, TooLarge, InvalidJson, UnsupportedVersion };

// Schema v1. Unknown keys are ignored; a block with missing required fields is
// dropped on its own and never invalidates the rest.
ParseStatus parseBrief(const char* json, size_t length, Brief& out);

// Picks one non-empty line of a UTF-8 quotes file. The pick depends only on
// `dayOfYear`, so it is stable for the whole day. Optional " — author" suffix.
bool pickQuoteFromText(const char* text, size_t length, uint32_t dayOfYear, Quote& out);

size_t builtinQuoteCount();
Quote builtinQuote(uint32_t dayOfYear);

// 1..366; 0 for an invalid date.
uint32_t dayOfYear(int year, int month, int day);

struct TetStatus {
  bool greeting = false;  // lunar 1/1 - 1/3
  int daysUntil = 0;      // to the next lunar 1/1; 0 while greeting
};

// False when the date is outside the lunar library's range.
bool tetStatus(int year, int month, int day, TetStatus& out);

}  // namespace sleep_brief
