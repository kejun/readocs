#include "readocs.hpp"
#include "ui_power_policy.hpp"
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <mutex>

namespace rd {
std::atomic<bool> power_off_requested{false};

static std::mutex pm1_lock;
static std::atomic<bool> light_sleep_ok{true}, light_probe{false};
static std::atomic<unsigned> light_sleeps{0};
static int probe_render = -1, probe_partials = 0;
static int64_t probe_elapsed = 0;
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

bool light_sleep_available() { return light_sleep_ok.load(); }
unsigned light_sleep_count() { return light_sleeps.load(); }
bool light_sleep_ui() {
  std::lock_guard<std::mutex> sleep_guard(ui_sleep_mutex);
  {
    std::lock_guard<std::mutex> model_guard(model.mutex);
    if (model.ap)
      return false;
  }
  // PM1 has no wake IRQ. Do not suspend an I2C transaction on the other core;
  // the short timer and explicit poll after wake also preserve its power key.
  esp_err_t err;
  {
    std::lock_guard<std::mutex> power_guard(pm1_lock);
    if (!light_sleep_ok || networking || !input_idle || power_off_requested ||
        uxQueueMessagesWaiting(actions) || uxQueueMessagesWaiting(buttons) ||
        !gpio_get_level(GPIO_NUM_3) || !gpio_get_level(GPIO_NUM_4))
      return false;
    err = gpio_wakeup_enable(GPIO_NUM_3, GPIO_INTR_LOW_LEVEL);
    if (err == ESP_OK)
      err = gpio_wakeup_enable(GPIO_NUM_4, GPIO_INTR_LOW_LEVEL);
    if (err == ESP_OK)
      err = esp_sleep_enable_gpio_wakeup();
    if (err == ESP_OK)
      err = esp_sleep_enable_timer_wakeup(ui_light_sleep_us);
    if (err == ESP_OK)
      err = esp_light_sleep_start();
    // Deep sleep later configures EXT1 and its own next-sync timer.
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    gpio_wakeup_disable(GPIO_NUM_3);
    gpio_wakeup_disable(GPIO_NUM_4);
    if (err == ESP_OK)
      ++light_sleeps;
    else if (err != ESP_ERR_SLEEP_REJECT) {
      light_sleep_ok = false;
      ESP_LOGW("readocs", "UI light sleep unavailable: %s", esp_err_to_name(err));
    }
  }
  // Manual sleep advances esp_timer, not FreeRTOS ticks. Poll the unrouted PM1
  // button here as well instead of depending on a delayed task's tick count.
  power_key_poll();
  return err == ESP_OK;
}
void request_light_sleep_probe() { light_probe = true; }
static bool store_probe(bool complete, bool ok, bool retained, bool fast) {
  Json result(cJSON_CreateObject());
  cJSON_AddBoolToObject(result.get(), "complete", complete);
  cJSON_AddBoolToObject(result.get(), "ok", ok);
  cJSON_AddBoolToObject(result.get(), "psram_retained", retained);
  cJSON_AddBoolToObject(result.get(), "fast_refresh_retained", fast);
  cJSON_AddNumberToObject(result.get(), "elapsed_ms", probe_elapsed);
  cJSON_AddNumberToObject(result.get(), "partial_before", probe_partials);
  cJSON_AddNumberToObject(result.get(), "partial_after", display_partial_count());
  cJSON_AddNumberToObject(result.get(), "light_sleeps", light_sleep_count());
  return save_sleep_probe(serialize(result.get()));
}
void run_light_sleep_probe() {
  if (probe_render >= 0) {
    unsigned rendered;
    {
      std::lock_guard<std::mutex> g(model.mutex);
      rendered = model.render_count;
    }
    if (rendered > static_cast<unsigned>(probe_render)) {
      const bool fast = display_partial_count() == probe_partials + 1;
      const bool stored = store_probe(true, fast, true, fast);
      std::printf("RESULT {\"command\":\"light_sleep_probe\",\"ok\":%s}\n",
                  fast && stored ? "true" : "false");
      probe_render = -1;
    }
    return;
  }
  if (!light_probe || networking || !input_idle)
    return;
  light_probe = false;
  probe_partials = display_partial_count();
  probe_elapsed = 0;
  if (!store_probe(false, false, false, false)) {
    std::printf("RESULT {\"command\":\"light_sleep_probe\",\"ok\":false}\n");
    return;
  }
  auto *witness = static_cast<uint32_t *>(
      heap_caps_malloc(8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!witness) {
    store_probe(true, false, false, false);
    std::printf("RESULT {\"command\":\"light_sleep_probe\",\"ok\":false}\n");
    return;
  }
  for (unsigned i = 0; i < 2048; ++i)
    witness[i] = 0xabc00000U + i;
  const auto before = now_ms();
  const bool slept = light_sleep_ui();
  probe_elapsed = now_ms() - before;
  bool retained = true;
  for (unsigned i = 0; i < 2048; ++i)
    retained &= witness[i] == 0xabc00000U + i;
  heap_caps_free(witness);
  if (slept && retained) {
    // USB can disappear during sleep on S3. Have the UI verify its retained
    // baseline itself, then persist the result for retrieval after reconnect.
    {
      std::lock_guard<std::mutex> g(model.mutex);
      probe_render = model.render_count;
    }
    Key next = Key::Next;
    if (xQueueSend(buttons, &next, 0) == pdTRUE)
      return;
    probe_render = -1;
  }
  store_probe(true, false, retained, false);
  std::printf("RESULT {\"command\":\"light_sleep_probe\",\"ok\":false}\n");
}
} // namespace rd
