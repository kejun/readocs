#pragma once
#include "article_list.hpp"
#include "input_logic.hpp"
#include <M5Unified.h>
#include <atomic>
#include <cJSON.h>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <memory>
#include <mutex>
#include <nvs.h>
#include <string>
#include <vector>

namespace rd {
struct JsonDelete {
  void operator()(cJSON *j) const { cJSON_Delete(j); }
};
using Json = std::unique_ptr<cJSON, JsonDelete>;
inline std::string str(const cJSON *j, const char *k) {
  auto *v = cJSON_GetObjectItemCaseSensitive(j, k);
  return cJSON_IsString(v) ? v->valuestring : "";
}
inline int num(const cJSON *j, const char *k, int fallback = 0) {
  auto *v = cJSON_GetObjectItemCaseSensitive(j, k);
  return cJSON_IsNumber(v) ? v->valueint : fallback;
}
inline std::string serialize(cJSON *j) {
  char *p = cJSON_PrintUnformatted(j);
  std::string s = p ? p : "";
  cJSON_free(p);
  return s;
}
struct Profile {
  std::string ssid, password;
};
struct Config {
  std::vector<Profile> networks;
  std::string server, ca, token, device_id;
  unsigned interval = 3600;
  unsigned font_size = 24;
};
struct Entry {
  std::string source_id, document_id, title, snapshot_id;
  bool operator==(const Entry &other) const {
    return source_id == other.source_id && document_id == other.document_id &&
           title == other.title && snapshot_id == other.snapshot_id;
  }
};
struct Model {
  std::mutex mutex;
  Config config;
  std::vector<Entry> entries;
  std::string current_source, current_document, displayed_snapshot;
  int page = 0;
  View view = View::Articles;
  int list_index = 0, page_count = 0;
  unsigned render_count = 0;
  std::string selected_source, selected_document;
  std::string etag, status = "等待配置", ssid, ip, ap_ssid, ap_password,
                    ap_session;
  bool ap = false, initialized = false, sync_pending = false;
};
extern Model model;
extern std::atomic<bool> connected, networking, redraw, setup_requested;
extern std::atomic<bool> input_idle;
extern std::atomic<int64_t> last_input, ap_activity;
extern std::atomic<bool> power_off_requested;
extern QueueHandle_t actions, buttons;
void configure_power_key();
void power_key_poll();
void power_key_prepare_sleep();
void power_key_shutdown();
uint16_t pm1_vbus_voltage();
uint8_t pm1_register(uint8_t reg);
struct Action {
  std::string type, json;
};
struct Line {
  std::string text;
  int y = 0;
  bool heading = false;
};
struct Page {
  std::vector<Line> lines;
  std::string block;
  unsigned offset = 0;
};
struct Position {
  std::string document, block, snapshot;
  unsigned offset = 0;
  int page = 0;
};
extern Position position;
int64_t now_ms();
void set_status(const std::string &s, bool visible = true);
void enqueue(const std::string &type, const std::string &json = "");
std::string sha256(const std::string &s);
bool valid_hash(const std::string &s);
std::string read_file(const std::string &name, size_t limit = 310 * 1024);
bool write_file(const std::string &name, const std::string &body);
std::string directory_progress();
bool save_directory_progress_file(const std::string &body);
void clear_directory_progress();
bool load_storage();
bool save_config(const Config &cfg);
bool commit_cache(const std::vector<Entry> &entries, const std::string &etag);
void save_position(const Position &pos);
Position load_position(const std::string &document);
void collect_cache(bool reclaim_previous = false);
void display_marker(bool pending, const std::string &snapshot = "");
bool display_pending();
void init_network();
void network_task(void *);
void console_task(void *);
void input_task(void *);
std::vector<Page> layout(const std::string &snapshot, int size);
void draw_page(const Entry &entry, const std::vector<Page> &pages, int index);
void draw_menu(const std::vector<std::string> &options, int selected,
               const std::string &title);
void draw_articles(const std::vector<Entry> &entries, int selected);
void draw_message(const std::string &title,
                  const std::vector<std::string> &lines, int size = 24);
void draw_cover();
void request_display_probe(int mode, int area, int count);
bool run_display_probe();
bool quality_cleanup_due();
void request_quality_cleanup();
int display_partial_count();
int display_partial_limit();
void set_display_partial_limit(int limit);
void start_setup();
void stop_setup();
} // namespace rd
