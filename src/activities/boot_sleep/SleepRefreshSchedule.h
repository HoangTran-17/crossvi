#pragma once

#include <cstdint>

// Pure scheduling rules for the daily timer wake that redraws the lunar calendar sleep screen.
namespace sleep_refresh {

constexpr uint32_t SECONDS_PER_DAY = 24 * 60 * 60;
// RTC slow-clock drift can fire the timer slightly before the target; anything further out is treated as a drifted
// wake and the device goes back to sleep without drawing.
constexpr uint32_t EARLY_TOLERANCE_S = 3 * 60;
constexpr uint32_t LATE_WINDOW_S = 3 * 60 * 60;
constexpr uint16_t MIN_VALID_YEAR = 2025;
constexpr uint8_t MIN_BATTERY_PERCENT = 10;

// Index = CrossPointSettings::sleepRefreshTime. Index 0 is "Off".
constexpr uint32_t TARGET_SECOND_OF_DAY[] = {0, 5 * 60, 5 * 3600, 6 * 3600, 7 * 3600};
constexpr uint8_t SETTING_COUNT = sizeof(TARGET_SECOND_OF_DAY) / sizeof(TARGET_SECOND_OF_DAY[0]);

struct Inputs {
  uint8_t setting;
  bool lunarScreen;
  uint16_t year;
  uint32_t secondOfDay;  // local time, 0..86399
  uint8_t batteryPercent;
};

// Seconds from now to the next target. A target that is exactly now counts as already reached, so the next one is a
// full day away. Returns 0 when no timer must be armed.
inline uint32_t secondsToNextRefresh(const Inputs& in) {
  if (in.setting == 0 || in.setting >= SETTING_COUNT) return 0;
  if (!in.lunarScreen) return 0;
  if (in.year < MIN_VALID_YEAR) return 0;
  if (in.batteryPercent < MIN_BATTERY_PERCENT) return 0;
  if (in.secondOfDay >= SECONDS_PER_DAY) return 0;

  const uint32_t target = TARGET_SECOND_OF_DAY[in.setting];
  return in.secondOfDay < target ? target - in.secondOfDay : target + SECONDS_PER_DAY - in.secondOfDay;
}

enum class WakeAction : uint8_t {
  Refresh,    // at the target, slightly before it, or shortly after it: redraw
  SleepOnly,  // woke well before the target: sleep until it without drawing
  GpioOnly,   // preconditions no longer hold: sleep on the power button only
};

struct WakePlan {
  WakeAction action;
  uint32_t sleepSeconds;  // only meaningful for SleepOnly
};

// Decides what a timer wake does. A wake at most EARLY_TOLERANCE_S before the target, or at most LATE_WINDOW_S after
// it, is the wake that was armed. Anything else is drift and only sleeps on.
inline WakePlan planTimerWake(const Inputs& in) {
  const uint32_t untilNext = secondsToNextRefresh(in);
  if (untilNext == 0) return {WakeAction::GpioOnly, 0};
  if (untilNext <= EARLY_TOLERANCE_S || untilNext >= SECONDS_PER_DAY - LATE_WINDOW_S) return {WakeAction::Refresh, 0};
  return {WakeAction::SleepOnly, untilNext};
}

// Sleep length after a redraw. A wake that came slightly early leaves the clock just before the target, and sleeping
// only until then would redraw again a minute later, so it skips to the following day instead.
inline uint32_t secondsAfterRefresh(const Inputs& in) {
  const uint32_t untilNext = secondsToNextRefresh(in);
  if (untilNext != 0 && untilNext <= EARLY_TOLERANCE_S) return untilNext + SECONDS_PER_DAY;
  return untilNext;
}

}  // namespace sleep_refresh
