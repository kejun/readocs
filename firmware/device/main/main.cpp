#include "readocs.hpp"
#include <algorithm>
#include <ctime>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <driver/usb_serial_jtag.h>
#include <driver/usb_serial_jtag_vfs.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <nvs_flash.h>

namespace rd {
Model model;
std::atomic<bool> connected{false}, networking{false}, redraw{true},
    setup_requested{false};
std::atomic<int64_t> last_input{0}, ap_activity{0};
QueueHandle_t actions, buttons;
static RTC_DATA_ATTR int retained_page = 0;
static RTC_DATA_ATTR char retained_document[129]{};
static RTC_DATA_ATTR int retained_view = static_cast<int>(View::Sources);
static RTC_DATA_ATTR int retained_selected = 0;
static RTC_DATA_ATTR char retained_list_source[129]{};
static RTC_DATA_ATTR char retained_list_document[129]{};
static RTC_DATA_ATTR int64_t next_sync_at = 0;
static RTC_DATA_ATTR unsigned failure_count = 0;
static RTC_DATA_ATTR char retained_current_source[129]{};
static RTC_DATA_ATTR SourceSelection retained_source_selections[retained_source_count]{};
static bool sleeping = false;

static bool keys_released() {
  return gpio_get_level(GPIO_NUM_3) != 0 &&
         gpio_get_level(GPIO_NUM_4) != 0;
}

static bool ui_idle() {
  if (networking || !input_idle.load() || !keys_released() ||
      uxQueueMessagesWaiting(buttons) > 0 ||
      uxQueueMessagesWaiting(actions) > 0)
    return false;
  std::lock_guard<std::mutex> g(model.mutex);
  return !model.ap;
}

static bool ready_to_leave_ui(bool require_idle = false) {
  if (sleeping || !ui_idle())
    return false;
  const auto before = last_input.load();
  if (require_idle && now_ms() - before < 250)
    return false;
  // A short press can start and end inside this window while its release is
  // still being debounced. Check the gesture state and activity again, rather
  // than treating a high GPIO at the end as a stable release.
  vTaskDelay(pdMS_TO_TICKS(50));
  return last_input.load() == before && !sleeping && ui_idle();
}

static void sleep_now(bool show_cover = false, bool require_idle = false) {
  if (!ready_to_leave_ui(require_idle))
    return;
  const auto before = last_input.load();
  unsigned interval;
  {
    std::lock_guard<std::mutex> g(model.mutex);
    interval = model.config.interval;
  }
  sleeping = true;
  esp_wifi_stop();
  if (show_cover) {
    // Explicit standby shows the shutdown cover; auto-sleep keeps the current
    // reading page so a one-second idle does not hide the text.
    draw_cover();
    retained_view = static_cast<int>(View::Reading);
  } else {
    M5.Display.waitDisplay();
    // Keep the e-paper image; driver state will be rebuilt with a full refresh
    // when needed.
  }
  // Drawing a cover can take seconds. Let any input or queued operation that
  // arrived during the waveform return to the UI before entering deep sleep.
  if (last_input.load() != before || !ui_idle()) {
    sleeping = false;
    return;
  }
  power_key_prepare_sleep();
  M5.Display.sleep();
  ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io((1ULL << 3) | (1ULL << 4),
                                                  ESP_EXT1_WAKEUP_ANY_LOW));
  rtc_gpio_pullup_en(GPIO_NUM_3);
  rtc_gpio_pullup_en(GPIO_NUM_4);
  rtc_gpio_pulldown_dis(GPIO_NUM_3);
  rtc_gpio_pulldown_dis(GPIO_NUM_4);
  auto now = std::time(nullptr);
  int64_t seconds = next_sync_at > now ? next_sync_at - now : interval;
  ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(std::max<int64_t>(30, seconds) *
                                                1000000ULL));
  esp_deep_sleep_start();
}

static void power_off_now() {
  sleeping = true;
  esp_wifi_stop();
  // The image is retained by the e-paper panel, so it must be drawn before the
  // PM1 cuts power.
  draw_cover();
  ESP_LOGI("readocs", "shutdown cover ready, cutting power");
  power_key_shutdown();
  while (true)
    vTaskDelay(pdMS_TO_TICKS(1000));
}

static void shutdown_now() {
  if (!ready_to_leave_ui())
    return;
  power_off_now();
}
} // namespace rd
extern "C" void app_main() {
  using namespace rd;
  // Never erase factory or user NVS on an initialization error.
  ESP_ERROR_CHECK(nvs_flash_init());
  auto cfg = M5.config();
  cfg.clear_display = false;
  cfg.internal_imu = false;
  cfg.internal_mic = false;
  cfg.internal_spk = false;
  M5.begin(cfg);
  if (M5.getDisplayCount() == 0 || M5.Display.width() != 540) {
    ESP_LOGE("readocs", "PaperDIY display not detected");
    return;
  }
  M5.Display.setAutoDisplay(false);
  M5.Display.setEpdMode(m5gfx::epd_mode_t::epd_quality);
  ESP_LOGI("readocs", "reset reason=%d", static_cast<int>(esp_reset_reason()));
  configure_power_key();
  const bool storage_ok = load_storage();
  actions = xQueueCreate(8, sizeof(Action *));
  buttons = xQueueCreate(32, sizeof(Key));
  assert(actions && buttons);
  last_input = now_ms();
  ESP_ERROR_CHECK(xTaskCreatePinnedToCore(input_task, "keys", 4096, nullptr, 5,
                                          nullptr, 1) == pdPASS
                      ? ESP_OK
                      : ESP_ERR_NO_MEM);
  init_network();
  usb_serial_jtag_driver_config_t usb{.tx_buffer_size = 4096,
                                      .rx_buffer_size = 8192};
  ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
  usb_serial_jtag_vfs_use_driver();
  ESP_ERROR_CHECK(xTaskCreate(console_task, "usb_config", 8192, nullptr, 3,
                              nullptr) == pdPASS
                      ? ESP_OK
                      : ESP_ERR_NO_MEM);
  ESP_ERROR_CHECK(
      xTaskCreate(network_task, "sync", 20480, nullptr, 3, nullptr) == pdPASS
          ? ESP_OK
          : ESP_ERR_NO_MEM);
  auto wake = esp_sleep_get_wakeup_cause();
  bool cold = wake == ESP_SLEEP_WAKEUP_UNDEFINED;
  if (!storage_ok)
    draw_message("缓存需要维护", {"旧分区已保留，请通过 USB 检查"});
  if (model.config.networks.empty())
    enqueue("setup");
  else if (wake == ESP_SLEEP_WAKEUP_TIMER || cold) {
    enqueue("sync");
    next_sync_at = std::time(nullptr) + model.config.interval;
  }
  if (storage_ok)
    esp_ota_mark_app_valid_cancel_rollback();
  View view = View::Sources;
  if (!cold && retained_view >= static_cast<int>(View::Reading) &&
      retained_view <= static_cast<int>(View::Sources))
    view = static_cast<View>(retained_view);
  // A confirmation screen is never restored by a timer or wake-up key.
  if (view == View::Pair)
    view = View::Sources;
  SourceBrowser browser;
  std::vector<Entry> all_entries;
  std::vector<Source> sources;
  {
    std::lock_guard<std::mutex> g(model.mutex);
    all_entries = model.entries;
    sources = model.sources;
  }
  if (!cold)
    browser.restore(retained_source_selections, retained_source_count, all_entries);
  browser.reconcile(sources);
  const bool restored_source = cold || browser.focus(sources, retained_list_source);
  if (!restored_source && view == View::Articles)
    view = View::Sources;
  std::vector<Entry> entries = articles_for_source(all_entries, browser.source());
  auto refresh_source_articles = [&] {
    entries = articles_for_source(all_entries, browser.source());
    browser.cursor().reconcile(entries);
  };
  bool deferred_pair_confirm = false;
  auto discard_invalid_source_backlog = [&] {
    Key queued;
    while (xQueueReceive(buttons, &queued, 0) == pdTRUE) {
      last_input = now_ms();
      if (queued == Key::PairConfirm)
        deferred_pair_confirm = true;
      browser.source_input_activity();
    }
  };
  Entry current;
  std::vector<Page> pages;
  const int selected_max = view == View::Fonts ? 2 : view == View::Menu ? 6 : 5;
  int page = 0, selected = !cold ? std::clamp(retained_selected, 0, selected_max)
                                 : 0;
  auto open_entry = [&](const Entry &e, bool reflow = false) {
    if (current.source_id != e.source_id || current.document_id != e.document_id)
      position = load_position(e.document_id);
    current = e;
    {
      std::lock_guard<std::mutex> g(model.mutex);
      model.current_source = e.source_id;
      model.current_document = e.document_id;
    }
    if (e.snapshot_id.empty()) {
      pages.clear();
      Json request(cJSON_CreateObject());
      cJSON_AddStringToObject(request.get(), "source_id", e.source_id.c_str());
      cJSON_AddStringToObject(request.get(), "document_id",
                              e.document_id.c_str());
      enqueue("download", serialize(request.get()));
      return;
    }
    unsigned size;
    {
      std::lock_guard<std::mutex> g(model.mutex);
      size = model.config.font_size;
    }
    pages = layout(e.snapshot_id, size);
    page = 0;
    if (position.document == e.document_id) {
      // Prefer the content anchor so a layout change (for example a line-height
      // adjustment) does not restore an old page number against a new layout.
      int anchored = -1;
      for (int i = 0; i < static_cast<int>(pages.size()); ++i)
        if (pages[i].block == position.block &&
            pages[i].offset <= position.offset)
          anchored = i;
      if (anchored >= 0)
        page = anchored;
      else if (position.snapshot == e.snapshot_id && !reflow)
        page = position.page;
    }
    if (std::string(retained_document) == e.document_id &&
        std::string(retained_current_source) == e.source_id &&
        wake != ESP_SLEEP_WAKEUP_UNDEFINED &&
        position.snapshot == e.snapshot_id && !reflow)
      page = retained_page;
    page = std::clamp(page, 0, std::max(0, static_cast<int>(pages.size()) - 1));
  };
  if (!all_entries.empty()) {
    auto it = std::find_if(all_entries.begin(), all_entries.end(), [&](const Entry &e) {
      return e.document_id == position.document &&
             (cold || !retained_current_source[0] ||
              e.source_id == retained_current_source);
    });
    if (it != all_entries.end()) {
      current = *it;
      if (cold || view == View::Reading)
        browser.focus(sources, current.source_id);
      refresh_source_articles();
      if ((cold || view == View::Reading) && browser.source() == current.source_id)
        browser.cursor().focus(entries, current.source_id, current.document_id);
      if (view == View::Reading)
        open_entry(current);
    }
  }
  if (!cold && browser.cursor().document().empty())
    browser.cursor().focus(entries, browser.source(), retained_list_document);
  else
    browser.cursor().reconcile(entries);
  if (view == View::Reading && current.document_id.empty())
    view = View::Sources;
  bool need_draw = cold || !restored_source || display_pending();
  std::string last_status;
  bool previous_busy = false, ap_was = false;
  int64_t last_check = 0;
  while (true) {
    const bool changed = redraw.exchange(false);
    if (power_off_requested.exchange(false)) {
      ESP_LOGI("readocs", "PM1 power key double-click");
      power_off_now();
    }
    if (run_display_probe())
      need_draw = true;
    std::string status, ssid, ap_ssid, ap_password;
    bool ap, sync_pending;
    bool catalog_changed = false, selected_source_preserved = true;
    Config config;
    {
      std::lock_guard<std::mutex> g(model.mutex);
      if ((changed || all_entries.empty() || previous_busy != networking) &&
          (all_entries != model.entries || sources != model.sources)) {
        all_entries = model.entries;
        sources = model.sources;
        catalog_changed = true;
      }
      status = model.status;
      ssid = model.ssid;
      ap = model.ap;
      sync_pending = model.sync_pending;
      ap_ssid = model.ap_ssid;
      ap_password = model.ap_password;
      config = model.config;
    }
    if (catalog_changed) {
      selected_source_preserved = browser.reconcile(sources);
      refresh_source_articles();
      // Losing a source returns to source selection; a queued B cannot open
      // an article from whichever source moved into the old list position.
      if (!selected_source_preserved && view == View::Articles)
        view = View::Sources;
      if (view == View::Articles || view == View::Sources)
        need_draw = true;
      // A completed on-demand download can be followed immediately by sync.
      // Open its committed snapshot without waiting for a sampled idle edge.
      if (view == View::Reading && pages.empty() &&
          !current.document_id.empty()) {
        auto updated =
            std::find_if(all_entries.begin(), all_entries.end(), [&](const Entry &e) {
              return e.source_id == current.source_id &&
                     e.document_id == current.document_id;
            });
        if (updated != all_entries.end() && !updated->snapshot_id.empty()) {
          open_entry(*updated);
          need_draw = true;
        }
      }
    }
    if (ap != ap_was) {
      need_draw = true;
      ap_was = ap;
    }
    if (previous_busy && !networking) {
      next_sync_at = std::time(nullptr) +
                     (sync_pending ? 60
                                   : (pm1_vbus_voltage() > 4000
                                          ? 300
                                          : config.interval));
      if (status == "已同步" || sync_pending)
        failure_count = 0;
      else
        ++failure_count;
      if (failure_count > 1)
        next_sync_at = std::time(nullptr) +
                       std::min<unsigned>(
                           21600, config.interval *
                                      (1U << std::min(failure_count - 1, 5U)));
    }
    previous_busy = networking;
    Key event;
    bool handled = false;
    while (xQueueReceive(buttons, &event, 0) == pdTRUE) {
      handled = true;
      const int64_t lag = now_ms() - last_input;
      if (lag > 250)
        ESP_LOGI("readocs", "key lag=%lld ms", static_cast<long long>(lag));
      last_input = now_ms();
      if (view == View::Sources && browser.source_input_fenced()) {
        // A source disappeared while old navigation was in flight. Discard
        // that navigation until its waveform and physical gesture have ended.
        if (event == Key::PairConfirm)
          deferred_pair_confirm = true;
        browser.source_input_activity();
        continue;
      }
      if (event == Key::PairConfirm) {
        view = View::Pair;
        selected = 0;
        need_draw = true;
        continue;
      }
      if (view == View::Pair) {
        // Require all keys to be released after the combination before
        // accepting a fresh confirmation.
        if (event == Key::Previous) {
          enqueue("setup");
          view = View::Sources;
        } else if (event == Key::Next || event == Key::Menu)
          view = View::Sources;
        need_draw = true;
        continue;
      }
      if (event == Key::Check) {
        if (view == View::Articles) {
          browser.cursor().next_page(entries);
          need_draw = true;
          continue;
        }
        if (now_ms() - last_check >= 30000 || !last_check) {
          enqueue("check");
          last_check = now_ms();
        }
        continue;
      }
      if (view == View::Reading) {
        if (event == Key::Menu) {
          if (browser.focus(sources, current.source_id)) {
            refresh_source_articles();
            browser.cursor().focus(entries, current.source_id, current.document_id);
            view = View::Articles;
          } else
            view = View::Sources;
          need_draw = true;
        } else if (!pages.empty() &&
                   (event == Key::Next || event == Key::Previous)) {
          int next = std::clamp(page + (event == Key::Next ? 1 : -1), 0,
                                static_cast<int>(pages.size()) - 1);
          if (next != page) {
            page = next;
            need_draw = true;
          }
        }
      } else if (view == View::Menu) {
        if (event == Key::Menu) {
          view = View::Sources;
          need_draw = true;
        } else if (event == Key::Next) {
          selected = (selected + 1) % 7;
          need_draw = true;
        } else if (event == Key::Previous) {
          if (selected == 0) {
            view = View::Sources;
          } else if (selected == 1) {
            enqueue("check");
            view = View::Status;
          } else if (selected == 2)
            view = View::Status;
          else if (selected == 3) {
            view = View::Fonts;
            selected = config.font_size == 16   ? 0
                       : config.font_size == 32 ? 2
                                                : 1;
          } else if (selected == 4)
            view = View::Pair;
          else if (selected == 5)
            sleep_now(true);
          else
            shutdown_now();
          need_draw = true;
        }
      } else if (view == View::Fonts) {
        if (event == Key::Menu) {
          view = View::Menu;
          selected = 0;
        } else if (event == Key::Next)
          selected = (selected + 1) % 3;
        else if (event == Key::Previous) {
          auto candidate = config;
          candidate.font_size = selected == 0 ? 16 : selected == 1 ? 24 : 32;
          if (save_config(candidate)) {
            {
              std::lock_guard<std::mutex> g(model.mutex);
              model.config = candidate;
            }
          }
          if (!current.document_id.empty()) {
            open_entry(current, true);
            view = View::Reading;
          } else
            view = View::Sources;
        }
        need_draw = true;
      } else if (view == View::Articles) {
        if (event == Key::Menu) {
          view = View::Sources;
          need_draw = true;
        } else if (!entries.empty()) {
          if (event == Key::Next)
            browser.cursor().next(entries);
          else if (event == Key::PreviousItem) {
            browser.cursor().previous(entries);
            ESP_LOGI("readocs", "previous item -> %d/%u",
                     browser.cursor().index() + 1,
                     static_cast<unsigned>(entries.size()));
          } else if (event == Key::Previous) {
            browser.cursor().reconcile(entries);
            open_entry(entries[browser.cursor().index()]);
            view = View::Reading;
          }
          need_draw = true;
        }
      } else if (view == View::Sources) {
        if (event == Key::Menu) {
          view = View::Menu;
          selected = 0;
        } else if (event == Key::Next || event == Key::PreviousItem) {
          browser.move(sources, event == Key::Next ? 1 : -1);
          refresh_source_articles();
        } else if (event == Key::Previous && browser.can_open_source()) {
          refresh_source_articles();
          view = View::Articles;
        }
        need_draw = true;
      } else if (event == Key::Menu || event == Key::Previous) {
        view = View::Menu;
        selected = 0;
        need_draw = true;
      }
    }
    if (view == View::Sources && browser.source_input_fenced()) {
      browser.settle_source_input(input_idle.load(), keys_released(),
                                   uxQueueMessagesWaiting(buttons) == 0, now_ms());
      if (!browser.source_input_fenced() && deferred_pair_confirm) {
        deferred_pair_confirm = false;
        view = View::Pair;
        selected = 0;
        need_draw = true;
      }
    }
    if (changed && (view == View::Status || ap ||
                    (view == View::Reading && pages.empty())))
      need_draw = true;
    if (view == View::Reading && !current.document_id.empty() &&
        now_ms() - last_input >= 30000 && !networking) {
      auto it =
          std::find_if(all_entries.begin(), all_entries.end(), [&](const Entry &e) {
            return e.source_id == current.source_id &&
                   e.document_id == current.document_id;
          });
      if (it != all_entries.end() && it->snapshot_id != current.snapshot_id &&
          !it->snapshot_id.empty()) {
        open_entry(*it);
        need_draw = true;
      }
    }
    if (need_draw) {
      if (view == View::Pair)
        draw_message("进入配网？", {"释放按键后，按 B 确认", "按 A 取消"});
      else if (ap)
        draw_message("连接 Wi-Fi",
                     {ap_ssid, ap_password, "http://192.168.4.1", status});
      else if (view == View::Fonts)
        draw_message(
            "字号",
            {"春日有信，阅读有时。", "AaBbCc 0123456789", "A 切换    B 使用"},
            selected == 0   ? 16
            : selected == 1 ? 24
                            : 32);
      else if (view == View::Menu)
        draw_menu({"内容来源", "检查更新", "状态", "字号", "配网", "休眠",
                   "关机"},
                  selected, "菜单");
      else if (view == View::Sources) {
        draw_sources(sources, browser.index());
        browser.source_rendered();
        if (browser.source_input_fenced())
          discard_invalid_source_backlog();
      }
      else if (view == View::Articles) {
        auto source = std::find_if(sources.begin(), sources.end(), [&](const Source &s) {
          return s.id == browser.source();
        });
        draw_articles(entries, browser.cursor().index(),
                       source != sources.end() ? source->title : "文章列表",
                       source != sources.end() ? source->count : 0);
      }
      else if (view == View::Status)
        draw_message("状态",
                     {status, ssid,
                      config.token.empty() ? "设备尚未绑定" : "设备已绑定"});
      else if (!pages.empty())
        draw_page(current, pages, page);
      else
        draw_message("正文尚未下载",
                     {status, "按住 A 检查更新", "按住 B 返回文章列表"});
      {
        std::lock_guard<std::mutex> g(model.mutex);
        ++model.render_count;
      }
      need_draw = false;
    }
    {
      std::lock_guard<std::mutex> g(model.mutex);
      model.view = view;
      model.source_index = browser.index();
      model.list_index = view == View::Sources ? browser.index() : browser.cursor().index();
      model.selected_source = browser.source();
      model.selected_document = browser.cursor().document();
      model.page = page;
      model.page_count = pages.size();
    }
    retained_view = static_cast<int>(view);
    retained_selected = selected;
    std::snprintf(retained_list_source, sizeof(retained_list_source), "%s",
                  browser.source().c_str());
    std::snprintf(retained_list_document, sizeof(retained_list_document), "%s",
                  browser.cursor().document().c_str());
    browser.retain(retained_source_selections, retained_source_count);
    if (!current.document_id.empty() && !pages.empty()) {
      retained_page = page;
      std::snprintf(retained_document, sizeof(retained_document), "%s",
                    current.document_id.c_str());
      std::snprintf(retained_current_source, sizeof(retained_current_source), "%s",
                    current.source_id.c_str());
    }
    const bool usb = pm1_vbus_voltage() > 4000;
    if (!ap && !networking && now_ms() - last_input >= 1000) {
      if (keys_released() && uxQueueMessagesWaiting(buttons) == 0 &&
          quality_cleanup_due()) {
        request_quality_cleanup();
        need_draw = true;
        last_input = now_ms();
        continue;
      }
      if (!usb)
        sleep_now(false, true);
    }
    if (usb && !ap && !networking && std::time(nullptr) >= next_sync_at &&
        config.token.size()) {
      enqueue("sync");
      next_sync_at = std::time(nullptr) + 300;
    }
    if (!handled)
      vTaskDelay(pdMS_TO_TICKS(15));
  }
}
