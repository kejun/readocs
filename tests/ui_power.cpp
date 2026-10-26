#include "ui_power_policy.hpp"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
using namespace rd;
static std::mutex ui_sleep_mutex, pm1_lock, network_radio_mutex;
static std::atomic<bool> networking{false}, input_idle{true}, power_off_requested{false};
static std::atomic<bool> light_sleep_ok{true};
static std::atomic<unsigned> light_sleeps{0};
static struct { std::mutex mutex; bool ap = false; } model;
static constexpr int actions = 1, buttons = 2;
static int queued = 0, raw = 0, configured = 0, disabled = 0, polls = 0;
using esp_err_t = int;
static constexpr int ESP_OK = 0, ESP_ERR_SLEEP_REJECT = 1;
static constexpr int GPIO_NUM_3 = 3, GPIO_NUM_4 = 4, GPIO_INTR_LOW_LEVEL = 0;
static constexpr int ESP_SLEEP_WAKEUP_GPIO = 1, ESP_SLEEP_WAKEUP_TIMER = 2;
static int sleep_result = ESP_OK;
static uint64_t timer = 0;
static int uxQueueMessagesWaiting(int) { return queued; }
static int gpio_get_level(int) { return !raw; }
static int gpio_wakeup_enable(int, int) { ++configured; return ESP_OK; }
static int gpio_wakeup_disable(int) { ++disabled; return ESP_OK; }
static int esp_sleep_enable_gpio_wakeup() { return ESP_OK; }
static int esp_sleep_enable_timer_wakeup(uint64_t us) { timer = us; return ESP_OK; }
static int esp_sleep_disable_wakeup_source(int) { ++disabled; return ESP_OK; }
static std::function<void()> sleeping;
static int esp_light_sleep_start() { if (sleeping) sleeping(); return sleep_result; }
static void power_key_poll() { ++polls; }
static const char *esp_err_to_name(int) { return "test error"; }
static void log_warning(const char *, const char *, ...) {}
#define ESP_LOGW(...) log_warning(__VA_ARGS__)
#include "light_sleep_under_test.hpp"
static bool light_sleep_available() { return light_sleep_ok; }
static std::atomic<int64_t> last_input{0};
static int64_t tick = 1000;
static bool usb_power = false, idle = true, cleanup = false;
static int deep_sleeps = 0, task_yields = 0;
static int64_t now_ms() { return tick; }
static int pm1_vbus_voltage() { return usb_power ? 5000 : 0; }
static bool ui_idle() { return idle; }
static bool keys_released() { return !raw; }
static bool quality_cleanup_due() { return cleanup; }
static void request_quality_cleanup() { cleanup = false; }
static void sleep_now(bool, bool) { ++deep_sleeps; }
static void vTaskDelay(int ticks) { assert(ticks == 1); ++task_yields; }
static void idle_round(bool interactive) {
  bool ap = model.ap, need_draw = false;
  for (int once = 0; once < 1; ++once) {
#include "idle_loop_under_test.hpp"
  }
  (void)need_draw;
}
int main() {
  assert(ui_idle_policy(true, true, true, 5000) == UiIdle::Awake);
  assert(ui_idle_policy(false, false, true, 5000) == UiIdle::Awake);
  assert(ui_idle_policy(false, true, true, 999) == UiIdle::Awake);
  assert(ui_idle_policy(false, true, true, 1000) == UiIdle::LightSleep);
  assert(ui_idle_policy(false, true, true, 14999) == UiIdle::LightSleep);
  assert(ui_idle_policy(false, true, true, 15000) == UiIdle::DeepSleep);
  assert(ui_idle_policy(false, true, false, 1000) == UiIdle::DeepSleep);
  // Each real operation resets idle_ms; timer-only wakes have no 15s session.
  assert(ui_idle_policy(false, true, true, 1100) == UiIdle::LightSleep);
  assert(light_sleep_ui());
  assert(light_sleeps == 1 && configured == 2 && disabled == 4);
  assert(timer == 25000 && polls == 1);
  const auto before = configured;
  networking = true;
  assert(!light_sleep_ui());
  networking = false;
  input_idle = false;
  assert(!light_sleep_ui());
  input_idle = true;
  raw = 1;
  assert(!light_sleep_ui());
  raw = 0;
  queued = 1;
  assert(!light_sleep_ui());
  queued = 0;
  model.ap = true;
  assert(!light_sleep_ui());
  model.ap = false;
  power_off_requested = true;
  assert(!light_sleep_ui());
  power_off_requested = false;
  assert(configured == before);
  sleep_result = ESP_ERR_SLEEP_REJECT;
  assert(!light_sleep_ui() && light_sleep_available());
  sleep_result = 99;
  assert(!light_sleep_ui() && !light_sleep_available());
  assert(ui_idle_policy(false, true, light_sleep_available(), 1000) == UiIdle::DeepSleep);
  light_sleep_ok = true;
  sleep_result = ESP_OK;
  std::atomic<bool> started{false};
  std::thread network;
  sleeping = [&] {
    // Execute the actual network start barrier while the sleep lock is held.
    network = std::thread([&] {
      assert(!pm1_lock.try_lock());
#include "network_awake_under_test.hpp"
      started = true;
    });
    while (!networking) std::this_thread::yield();
    assert(!started);
  };
  assert(light_sleep_ui());
  network.join();
  assert(started && light_sleeps == 2);
  sleeping = {};
  networking = false;
  idle_round(true);
  assert(light_sleeps == 3 && task_yields == 1 && deep_sleeps == 0);
  tick = 14999;
  idle_round(true);
  assert(light_sleeps == 4 && deep_sleeps == 0);
  tick = 15000;
  idle_round(true);
  assert(deep_sleeps == 1);
  tick = 1000;
  idle_round(false);
  assert(deep_sleeps == 2);
  usb_power = true;
  idle_round(true);
  assert(deep_sleeps == 2 && light_sleeps == 4);
  usb_power = false;
  cleanup = true;
  idle_round(true);
  assert(!cleanup && light_sleeps == 4);
  light_sleep_ok = false;
  last_input = 0;
  idle_round(true);
  assert(deep_sleeps == 3);
}
