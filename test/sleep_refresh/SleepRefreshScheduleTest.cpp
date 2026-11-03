#include <gtest/gtest.h>

#include "SleepRefreshSchedule.h"

using namespace sleep_refresh;

namespace {

constexpr uint32_t hms(const uint32_t h, const uint32_t m = 0, const uint32_t s = 0) { return h * 3600 + m * 60 + s; }

Inputs at(const uint8_t setting, const uint32_t secondOfDay) { return Inputs{setting, true, 2026, secondOfDay, 80}; }

}  // namespace

TEST(SleepRefreshSchedule, OffNeverArms) {
  EXPECT_EQ(secondsToNextRefresh(at(0, hms(3))), 0u);
  EXPECT_EQ(planTimerWake(at(0, hms(3))).action, WakeAction::GpioOnly);
}

TEST(SleepRefreshSchedule, EachSettingTargetsItsClockTime) {
  EXPECT_EQ(secondsToNextRefresh(at(1, hms(0))), 5u * 60);
  EXPECT_EQ(secondsToNextRefresh(at(2, hms(0))), hms(5));
  EXPECT_EQ(secondsToNextRefresh(at(3, hms(0))), hms(6));
  EXPECT_EQ(secondsToNextRefresh(at(4, hms(0))), hms(7));
}

TEST(SleepRefreshSchedule, UnknownSettingNeverArms) {
  EXPECT_EQ(secondsToNextRefresh(at(SETTING_COUNT, hms(3))), 0u);
  EXPECT_EQ(secondsToNextRefresh(at(255, hms(3))), 0u);
}

TEST(SleepRefreshSchedule, BeforeTargetSameDay) {
  EXPECT_EQ(secondsToNextRefresh(at(3, hms(5, 30))), hms(0, 30));
  EXPECT_EQ(secondsToNextRefresh(at(3, hms(5, 59, 59))), 1u);
}

TEST(SleepRefreshSchedule, AfterTargetWaitsUntilTomorrow) {
  EXPECT_EQ(secondsToNextRefresh(at(3, hms(6, 0, 1))), SECONDS_PER_DAY - 1);
  EXPECT_EQ(secondsToNextRefresh(at(3, hms(22))), hms(8));
  EXPECT_EQ(secondsToNextRefresh(at(3, hms(23, 59, 59))), hms(6, 0, 1));
}

TEST(SleepRefreshSchedule, ExactlyAtTargetWaitsAFullDay) {
  EXPECT_EQ(secondsToNextRefresh(at(3, hms(6))), SECONDS_PER_DAY);
  EXPECT_EQ(secondsToNextRefresh(at(1, hms(0, 5))), SECONDS_PER_DAY);
}

TEST(SleepRefreshSchedule, CrossesMidnight) {
  EXPECT_EQ(secondsToNextRefresh(at(1, hms(23, 59, 30))), 5u * 60 + 30);
  EXPECT_EQ(secondsToNextRefresh(at(2, hms(23, 0))), hms(6));
}

TEST(SleepRefreshSchedule, MonthAndYearEndOnlyDependOnTimeOfDay) {
  // 31 Dec 23:59:50 -> 00:05 on 1 Jan; the date rolls over inside that span.
  Inputs yearEnd = at(1, hms(23, 59, 50));
  yearEnd.year = 2026;
  EXPECT_EQ(secondsToNextRefresh(yearEnd), 5u * 60 + 10);
  // The year check uses the year the device wakes up in, so the first valid year still arms.
  Inputs firstYear = at(1, hms(12));
  firstYear.year = MIN_VALID_YEAR;
  EXPECT_NE(secondsToNextRefresh(firstYear), 0u);
}

TEST(SleepRefreshSchedule, InvalidClockDoesNotArm) {
  Inputs in = at(3, hms(3));
  in.year = MIN_VALID_YEAR - 1;
  EXPECT_EQ(secondsToNextRefresh(in), 0u);
  in.year = 1970;
  EXPECT_EQ(secondsToNextRefresh(in), 0u);
  in = at(3, SECONDS_PER_DAY);
  EXPECT_EQ(secondsToNextRefresh(in), 0u);
}

TEST(SleepRefreshSchedule, OtherSleepScreenDoesNotArm) {
  Inputs in = at(3, hms(3));
  in.lunarScreen = false;
  EXPECT_EQ(secondsToNextRefresh(in), 0u);
}

TEST(SleepRefreshSchedule, LowBatteryDoesNotArm) {
  Inputs in = at(3, hms(3));
  in.batteryPercent = MIN_BATTERY_PERCENT - 1;
  EXPECT_EQ(secondsToNextRefresh(in), 0u);
  in.batteryPercent = MIN_BATTERY_PERCENT;
  EXPECT_NE(secondsToNextRefresh(in), 0u);
}

TEST(SleepRefreshSchedule, WakeOnTimeRefreshes) {
  EXPECT_EQ(planTimerWake(at(3, hms(6))).action, WakeAction::Refresh);
  EXPECT_EQ(planTimerWake(at(3, hms(6, 0, 20))).action, WakeAction::Refresh);
}

TEST(SleepRefreshSchedule, WakeWithinEarlyToleranceRefreshes) {
  EXPECT_EQ(planTimerWake(at(3, hms(6) - EARLY_TOLERANCE_S)).action, WakeAction::Refresh);
  EXPECT_EQ(planTimerWake(at(3, hms(6) - 1)).action, WakeAction::Refresh);
}

TEST(SleepRefreshSchedule, WakeBeyondEarlyToleranceSleepsUntilTarget) {
  const WakePlan plan = planTimerWake(at(3, hms(6) - EARLY_TOLERANCE_S - 1));
  EXPECT_EQ(plan.action, WakeAction::SleepOnly);
  EXPECT_EQ(plan.sleepSeconds, EARLY_TOLERANCE_S + 1);
}

TEST(SleepRefreshSchedule, EarlyWakeAcrossMidnightForMidnightTarget) {
  // 00:05 target, drifted wake at 23:50 the day before.
  const WakePlan plan = planTimerWake(at(1, hms(23, 50)));
  EXPECT_EQ(plan.action, WakeAction::SleepOnly);
  EXPECT_EQ(plan.sleepSeconds, 15u * 60);
}

TEST(SleepRefreshSchedule, LateWakeWithinWindowRefreshes) {
  EXPECT_EQ(planTimerWake(at(3, hms(6) + LATE_WINDOW_S)).action, WakeAction::Refresh);
  EXPECT_EQ(planTimerWake(at(3, hms(6) + LATE_WINDOW_S + 1)).action, WakeAction::SleepOnly);
}

TEST(SleepRefreshSchedule, WakeWithBadPreconditionsGoesGpioOnly) {
  Inputs in = at(3, hms(6));
  in.batteryPercent = 3;
  EXPECT_EQ(planTimerWake(in).action, WakeAction::GpioOnly);
  in = at(3, hms(6));
  in.year = 2000;
  EXPECT_EQ(planTimerWake(in).action, WakeAction::GpioOnly);
  in = at(3, hms(6));
  in.lunarScreen = false;
  EXPECT_EQ(planTimerWake(in).action, WakeAction::GpioOnly);
}

TEST(SleepRefreshSchedule, AfterRefreshSleepsUntilNextDay) {
  EXPECT_EQ(secondsAfterRefresh(at(3, hms(6, 0, 30))), SECONDS_PER_DAY - 30);
  EXPECT_EQ(secondsAfterRefresh(at(3, hms(6))), SECONDS_PER_DAY);
}

TEST(SleepRefreshSchedule, AfterEarlyRefreshSkipsToTomorrow) {
  // Woke 40 s early, redrawing took 10 s: still 30 s before the target.
  EXPECT_EQ(secondsAfterRefresh(at(3, hms(6) - 30)), SECONDS_PER_DAY + 30);
}

TEST(SleepRefreshSchedule, AfterRefreshWithBadPreconditionsIsZero) {
  Inputs in = at(3, hms(6, 1));
  in.batteryPercent = 0;
  EXPECT_EQ(secondsAfterRefresh(in), 0u);
}
