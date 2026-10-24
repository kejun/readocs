#include "readocs.hpp"
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

namespace rd {
void input_task(void *) {
  gpio_config_t cfg{};
  cfg.pin_bit_mask = (1ULL << 3) | (1ULL << 4);
  cfg.mode = GPIO_MODE_INPUT;
  cfg.pull_up_en = GPIO_PULLUP_ENABLE;
  rtc_gpio_deinit(GPIO_NUM_3);
  rtc_gpio_deinit(GPIO_NUM_4);
  ESP_ERROR_CHECK(gpio_config(&cfg));
  auto sample = []() {
    return (gpio_get_level(GPIO_NUM_4) == 0 ? 1U : 0U) |
           (gpio_get_level(GPIO_NUM_3) == 0 ? 2U : 0U);
  };
  auto emit = [](Key key) { xQueueSend(buttons, &key, 0); };
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1) {
    auto cause = esp_sleep_get_ext1_wakeup_status();
    if (!sample() && (cause == (1ULL << 4) || cause == (1ULL << 3)))
      emit(cause == (1ULL << 4) ? Key::Next : Key::Previous);
  }
  InputLogic input;
  while (true) {
    auto raw = sample();
    if (raw)
      last_input = now_ms();
    input.update(raw, now_ms(), emit);
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}
} // namespace rd
