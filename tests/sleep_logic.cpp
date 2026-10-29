#include "article_list.hpp"
#include "input_logic.hpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <ctime>
#include <functional>
#include <mutex>

struct Model {
  std::mutex mutex;
  bool ap = false;
  struct {
    unsigned interval = 3600;
  } config;
} model;
static std::atomic<bool> networking{false}, input_idle{true};
static std::atomic<int64_t> last_input{0};
static bool sleeping = false;
static int button_count = 0, action_count = 0;
static constexpr int buttons = 1, actions = 2;
static unsigned raw = 0;
static int64_t tick = 1500;
static std::function<void()> during_delay;
static std::function<void()> during_cover;
static int retained_view = static_cast<int>(rd::View::Articles);
static int64_t next_sync_at = 0;
static uint64_t timer_wakeup_us = 0;
static bool entered_sleep = false, covered = false;
static struct {
  struct {
    void waitDisplay() {}
    void sleep() {}
  } Display;
} M5;
using rd::View;
static void draw_cover() {
  covered = true;
  if (during_cover)
    during_cover();
}
static void power_key_prepare_sleep() {}
static void esp_wifi_stop() {}
static void esp_deep_sleep_start() { entered_sleep = true; }
static int esp_sleep_enable_ext1_wakeup_io(uint64_t, int) { return 0; }
static int esp_sleep_enable_timer_wakeup(uint64_t us) {
  timer_wakeup_us = us;
  return 0;
}
static void rtc_gpio_pullup_en(int) {}
static void rtc_gpio_pulldown_dis(int) {}
#define GPIO_NUM_3 3
#define GPIO_NUM_4 4
#define ESP_EXT1_WAKEUP_ANY_LOW 0
#define ESP_ERROR_CHECK(expression) assert((expression) == 0)
static int64_t now_ms() { return tick; }
static bool keys_released() { return raw == 0; }
static int uxQueueMessagesWaiting(int queue) {
  return queue == buttons ? button_count : action_count;
}
#define pdMS_TO_TICKS(ms) (ms)
static void vTaskDelay(int delay) {
  const auto end = tick + delay;
  if (during_delay)
    during_delay();
  tick = end;
}
#include "sleep_guard_under_test.hpp"

int main() {
  assert(ready_to_leave_ui(true));
  const auto reset = []() {
    networking = false;
    input_idle = true;
    last_input = 0;
    model.ap = false;
    raw = button_count = action_count = 0;
    tick = 1500;
    during_delay = {};
    during_cover = {};
    sleeping = entered_sleep = covered = false;
    next_sync_at = 0;
    timer_wakeup_us = 0;
  };
  reset();
  rd::InputLogic input;
  during_delay = [&]() {
    auto emit = [](rd::Key) { ++button_count; };
    tick = 1505;
    raw = 1;
    last_input = tick;
    input.update(raw, tick, emit);
    tick = 1530;
    last_input = tick;
    input.update(raw, tick, emit);
    tick = 1535;
    raw = 0;
    input.update(raw, tick, emit);
    input_idle = input.idle();
  };
  assert(!ready_to_leave_ui(true));
  assert(button_count ==
         0); // A valid press is still awaiting release debounce.
  input.update(0, 1560, [](rd::Key) { ++button_count; });
  assert(button_count == 1 && input.idle());

  reset();
  during_delay = []() { ++button_count; };
  assert(!ready_to_leave_ui(false));
  reset();
  during_delay = []() { ++action_count; };
  assert(!ready_to_leave_ui(true));
  reset();
  during_delay = []() { networking = true; };
  assert(!ready_to_leave_ui(true));
  reset();
  during_delay = []() { model.ap = true; };
  assert(!ready_to_leave_ui(false));
  reset();
  input_idle = false;
  assert(!ready_to_leave_ui(true));

  reset();
  during_cover = []() {
    last_input = tick;
    ++button_count;
  };
  sleep_now(true);
  assert(covered && !entered_sleep && !sleeping && button_count == 1);
  reset();
  sleep_now(true);
  assert(covered && entered_sleep);
  assert(retained_view == static_cast<int>(View::Reading));

  reset();
  next_sync_at = std::time(nullptr) - 1;
  sleep_now();
  assert(entered_sleep && timer_wakeup_us == 30 * 1000000ULL);
  reset();
  next_sync_at = std::time(nullptr) + 90;
  sleep_now();
  assert(entered_sleep && timer_wakeup_us >= 89 * 1000000ULL &&
         timer_wakeup_us <= 90 * 1000000ULL);
  reset();
  sleep_now();
  assert(entered_sleep && timer_wakeup_us == model.config.interval * 1000000ULL);
}
