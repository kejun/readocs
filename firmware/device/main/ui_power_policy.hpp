#pragma once
#include <cstdint>

namespace rd {
// See docs/device-refresh.md. Deep sleep destroys the EPD baseline in PSRAM.
inline constexpr int64_t ui_interaction_ms = 15000;
inline constexpr uint32_t ui_light_sleep_us = 25000;
enum class UiIdle { Awake, LightSleep, DeepSleep };
inline UiIdle ui_idle_policy(bool usb, bool idle, bool interactive,
                            int64_t idle_ms) {
  if (usb || !idle || idle_ms < 1000)
    return UiIdle::Awake;
  return interactive && idle_ms < ui_interaction_ms ? UiIdle::LightSleep
                                                   : UiIdle::DeepSleep;
}
} // namespace rd
