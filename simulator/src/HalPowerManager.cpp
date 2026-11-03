#include "HalPowerManager.h"

#include <cstdio>

#include "HalGPIO.h"

HalPowerManager powerManager;

void HalPowerManager::begin() {}
void HalPowerManager::startDeepSleep(HalGPIO &gpio, const uint32_t timerWakeSeconds) const {
  std::printf("SIM_TIMER_WAKE_ARMED seconds=%u\n", static_cast<unsigned>(timerWakeSeconds));
  gpio.startDeepSleep();
}
void HalPowerManager::setPowerSaving(bool enable) {}
uint16_t HalPowerManager::getBatteryPercentage() const { return 100; }

HalPowerManager::Lock::Lock() {}
HalPowerManager::Lock::~Lock() {}
