#include "readocs.hpp"
#include <esp_log.h>
#include <mutex>

namespace rd {
std::atomic<bool> power_off_requested{false};

static std::mutex pm1_lock;
static constexpr uint8_t kButtonStatus = 0x48;
static constexpr uint8_t kButtonConfig1 = 0x49;
static constexpr uint8_t kButtonConfig2 = 0x4A;
static constexpr int64_t kDoubleClickMs = 1000;

static void restore_power_key_locked(m5::M5PM1_Class &pm) {
  // PM1 must be able to power the board back on after a real shutdown. The
  // disabled single-click reset is only a while-awake setting used to receive
  // the click in software.
  const uint8_t cfg1 = pm.readRegister8(kButtonConfig1);
  pm.writeRegister8(kButtonConfig1, cfg1 & ~0x01);
  const uint8_t cfg2 = pm.readRegister8(kButtonConfig2);
  pm.writeRegister8(kButtonConfig2, cfg2 & ~0x01);
}

void configure_power_key() {
  std::lock_guard<std::mutex> g(pm1_lock);
  auto &pm = M5.Power.M5pm1;
  if (!pm.isEnabled()) {
    ESP_LOGW("readocs", "PM1 unavailable; power key stays hardware-managed");
    return;
  }
  // Keep the power key from resetting on the first click, widen the click
  // windows, and stop PM1 from cutting power before the cover is drawn.
  // The firmware consumes BTN_STATUS instead of the transient IRQ status:
  // PaperDIY does not route a PM1 IRQ output pin to the SoC.
  uint8_t cfg1 = pm.readRegister8(kButtonConfig1);
  cfg1 = (cfg1 & ~0x67) | 0x67;
  pm.writeRegister8(kButtonConfig1, cfg1);
  uint8_t cfg2 = pm.readRegister8(kButtonConfig2);
  pm.writeRegister8(kButtonConfig2, cfg2 | 0x01);
  pm.clearButtonIRQStatus();
  ESP_LOGI("readocs", "PM1 power key handled by firmware");
  ESP_LOGI("readocs", "PM1 BTN_CFG_1=0x%02x BTN_CFG_2=0x%02x",
           pm.readRegister8(kButtonConfig1),
           pm.readRegister8(kButtonConfig2));
}

void power_key_poll() {
  if (power_off_requested.load())
    return;
  static bool initialized = false;
  static bool previous_pressed = false;
  static int64_t last_press = 0;

  std::lock_guard<std::mutex> g(pm1_lock);
  auto &pm = M5.Power.M5pm1;
  if (!pm.isEnabled())
    return;
  const uint8_t status = pm.readRegister8(kButtonStatus);
  const bool pressed = (status & 0x01) != 0;
  if (!initialized) {
    // The power-on press is already recorded in BTN_FLAG; it is not a user
    // double click.
    initialized = true;
    previous_pressed = pressed;
    return;
  }
  // Poll at 10 ms and use the settled level. BTN_FLAG is read-cleared and is
  // deliberately not used as an extra edge: keeping the detector edge-only
  // prevents one click from being counted again when the flag is latched.
  if (pressed && !previous_pressed) {
    const int64_t now = now_ms();
    ESP_LOGI("readocs", "power key press");
    if (last_press && now - last_press <= kDoubleClickMs) {
      power_off_requested.store(true);
      ESP_LOGI("readocs", "power key double-click detected");
    }
    last_press = now;
  }
  previous_pressed = pressed;
}

void power_key_prepare_sleep() {
  std::lock_guard<std::mutex> g(pm1_lock);
  auto &pm = M5.Power.M5pm1;
  if (!pm.isEnabled())
    return;
  restore_power_key_locked(pm);
}

void power_key_shutdown() {
  std::lock_guard<std::mutex> g(pm1_lock);
  power_off_requested.store(true);
  auto &pm = M5.Power.M5pm1;
  if (pm.isEnabled())
    restore_power_key_locked(pm);
  M5.Power.powerOff();
}

uint16_t pm1_vbus_voltage() {
  std::lock_guard<std::mutex> g(pm1_lock);
  return M5.Power.M5pm1.getVBUSVoltage();
}

uint8_t pm1_register(uint8_t reg) {
  std::lock_guard<std::mutex> g(pm1_lock);
  return M5.Power.M5pm1.readRegister8(reg);
}
} // namespace rd
