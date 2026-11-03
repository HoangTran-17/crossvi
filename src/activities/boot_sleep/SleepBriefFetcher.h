#pragma once

#include <cstdint>

namespace sleep_brief_fetch {

// Called on a timer wake that is about to draw the sleep screen. Downloads brief.json when the setting, layout, saved
// Wi-Fi and battery allow it. Keeps the saved file unless a valid replacement is ready, always leaves Wi-Fi off, and
// returns within WIFI_TIMEOUT_MS + HTTP_TIMEOUT_MS plus a few seconds of SD writes.
void runOnTimerWake(uint8_t batteryPercent);

}  // namespace sleep_brief_fetch
