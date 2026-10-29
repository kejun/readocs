#include "cache_merge.hpp"
#include "cache_policy.hpp"
#include "external_storage.hpp"
#include "readocs.hpp"
#include "source_metadata.hpp"
#include "ui_power_policy.hpp"
#include "reader_fonts.hpp"
#include <algorithm>
#include <cstring>
#include <ctime>
#include <driver/usb_serial_jtag.h>
#include <driver/usb_serial_jtag_vfs.h>
#include <esp_crt_bundle.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_netif_sntp.h>
#include <esp_random.h>
#include <esp_spiffs.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/event_groups.h>
#include <lwip/sockets.h>
#include <sys/socket.h>
#include "setup_access.hpp"
#include "setup_page.hpp"
#include <utility>

namespace rd {
static esp_netif_t *sta_netif, *ap_netif;
static EventGroupHandle_t wifi_events;
static std::atomic<int> reason{0};
static constexpr EventBits_t GOT_IP = BIT0, LOST_IP = BIT1;
static int64_t deadline = 0;
static std::string error_text = "";
static bool sync_made_progress = false, storage_full = false;
static std::atomic<bool> local_proxy_failed{false};
static std::atomic<bool> associated{false};
static const char *connection_error = "connection_failed";
struct SetupOperation {
  std::string state = "idle", code, message, kind;
  unsigned id = 0;
  bool pending = false;
  int64_t close_at = 0;
};
static SetupOperation setup_operation; // Protected by model.mutex, including admission.
static std::mutex network_radio_mutex;
int64_t now_ms() { return esp_timer_get_time() / 1000; }
void set_status(const std::string &s, bool visible) {
  {
    std::lock_guard<std::mutex> g(model.mutex);
    model.status = s;
  }
  if (visible)
    redraw = true;
}
bool enqueue(const std::string &type, const std::string &json) {
  auto *a = new Action{type, json};
  if (xQueueSend(actions, &a, 0) != pdTRUE) {
    std::fill(a->json.begin(), a->json.end(), 0);
    delete a;
    return false;
  }
  return true;
}
bool request_check_update() {
  const auto now = now_ms();
  bool duplicate;
  {
    std::lock_guard<std::mutex> g(model.mutex);
    duplicate = model.last_check_ms >= 0 && now - model.last_check_ms < 30000;
    model.check_notice = true;
    model.status = duplicate ? "刚刚已提交检查，请稍候" :
        model.refresh_id.empty() ? "已排队：同步所有来源" : "正在继续同步所有来源";
    if (!duplicate) {
      model.last_check_ms = now;
      model.check_queued = true;
    }
  }
  redraw = true;
  if (duplicate || enqueue("check")) return true;
  {
    std::lock_guard<std::mutex> g(model.mutex);
    model.last_check_ms = -1;
    model.check_queued = false;
  }
  set_status("操作队列已满，请稍后重试");
  return false;
}
static Config config_copy() {
  std::lock_guard<std::mutex> g(model.mutex);
  return model.config;
}
static std::pair<std::string, std::string> current_article() {
  std::lock_guard<std::mutex> g(model.mutex);
  return {model.current_source, model.current_document};
}
static void prepare_download_space() {
  collect_cache();
  if (external_cache_enabled()) return;
  size_t total = 0, used = 0;
  // SPIFFS only guarantees about 75% usable space. Leave room for a body,
  // directory transaction and filesystem GC before retaining older fallbacks.
  if (esp_spiffs_info("cache", &total, &used) == ESP_OK &&
      used + 1024 * 1024 > total * 3 / 4)
    collect_cache(true);
}
static void wifi_event(void *, esp_event_base_t base, int32_t id, void *data) {
  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    connected = true;
    reason = 0;
    auto *info = static_cast<ip_event_got_ip_t *>(data);
    char ip[16]{};
    std::snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info->ip_info.ip));
    {
      std::lock_guard<std::mutex> g(model.mutex);
      model.ip = ip;
    }
    xEventGroupSetBits(wifi_events, GOT_IP);
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
    associated = true;
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    associated = false;
    connected = false;
    reason = static_cast<wifi_event_sta_disconnected_t *>(data)->reason;
    xEventGroupSetBits(wifi_events, LOST_IP);
  }
}
static bool connect_profile(const Profile &p) {
  connection_error = "connection_failed";
  associated = connected = false;
  reason = 0;
  esp_wifi_disconnect();
  vTaskDelay(pdMS_TO_TICKS(100));
  xEventGroupClearBits(wifi_events, GOT_IP | LOST_IP);
  wifi_config_t w{};
  std::memcpy(w.sta.ssid, p.ssid.data(), p.ssid.size());
  std::memcpy(w.sta.password, p.password.data(), p.password.size());
  w.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  w.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  w.sta.threshold.authmode =
      p.password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
  w.sta.pmf_cfg.capable = true;
  w.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
  auto err = esp_wifi_set_config(WIFI_IF_STA, &w);
  std::memset(&w, 0, sizeof(w));
  if (err != ESP_OK || esp_wifi_connect() != ESP_OK) {
    connection_error = "connect_start_failed";
    return false;
  }
  int64_t remaining = std::min<int64_t>(12000, deadline - now_ms());
  if (remaining <= 0)
    return false;
  auto bits = xEventGroupWaitBits(wifi_events, GOT_IP | LOST_IP, pdTRUE,
                                  pdFALSE, pdMS_TO_TICKS(remaining));
  if (bits & GOT_IP) {
    std::lock_guard<std::mutex> g(model.mutex);
    model.ssid = p.ssid;
    return true;
  }
  connection_error = reason == WIFI_REASON_AUTH_FAIL ||
                             reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                             reason == WIFI_REASON_HANDSHAKE_TIMEOUT
                         ? "auth_failed"
                     : reason == WIFI_REASON_NO_AP_FOUND ? "network_not_found"
                     : associated ? "dhcp_timeout" : "connection_timeout";
  return false;
}
static bool ensure_wifi() {
  auto c = config_copy();
  wifi_mode_t mode;
  esp_wifi_get_mode(&mode);
  if (mode == WIFI_MODE_NULL)
    esp_wifi_set_mode(WIFI_MODE_STA);
  esp_wifi_start();
  // Bulk SD transfers run while networking is active. Avoid modem-sleep
  // latency; the task still stops Wi-Fi before the ordinary idle/sleep path.
  if (external_cache_enabled()) {
    const auto err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
      error_text = "Wi-Fi 传输模式设置失败";
      return false;
    }
  }
  if (connected)
    return true;
  // Choose saved networks actually visible here, so profiles 3–5 remain
  // reachable.
  std::vector<Profile> available;
  wifi_scan_config_t scan{};
  scan.show_hidden = true;
  scan.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  scan.scan_time.active.min = 30;
  scan.scan_time.active.max = 90;
  if (esp_wifi_scan_start(&scan, true) == ESP_OK) {
    wifi_ap_record_t found[32]{};
    uint16_t count = 32;
    if (esp_wifi_scan_get_ap_records(&count, found) == ESP_OK)
      for (const auto &saved : c.networks)
        for (unsigned i = 0; i < count; ++i)
          if (saved.ssid == reinterpret_cast<char *>(found[i].ssid)) {
            available.push_back(saved);
            break;
          }
  }
  if (available.empty())
    available = c.networks;
  if (available.size() == 1)
    available.push_back(available.front());
  int attempts = 0;
  for (const auto &p : available) {
    if (++attempts > 2 || now_ms() >= deadline)
      break;
    if (connect_profile(p))
      return true;
    vTaskDelay(pdMS_TO_TICKS(300));
  }
  error_text = reason == WIFI_REASON_AUTH_FAIL ||
                       reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
                   ? "Wi-Fi 密码错误，请重新配网"
                   : "网络不可用，可继续离线阅读";
  return false;
}
static bool ensure_time() {
  if (std::time(nullptr) > 1735689600)
    return true;
  esp_sntp_config_t s = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
  if (esp_netif_sntp_init(&s) != ESP_OK) {
    error_text = "校时失败，请稍后检查";
    return false;
  }
  auto remaining =
      std::max<int64_t>(1, std::min<int64_t>(5000, deadline - now_ms()));
  auto err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(remaining));
  esp_netif_sntp_deinit();
  if (err != ESP_OK) {
    error_text = "校时失败，已保留离线内容";
    return false;
  }
  return true;
}
struct Http {
  std::string body, etag;
  size_t limit;
  bool overflow = false;
};
// Requests in one network action share TLS; release it before stopping Wi-Fi.
static esp_http_client_handle_t http_client = nullptr;
static std::string http_server, http_ca, http_name;
static bool http_retry_used = false;
static void close_http() {
  if (http_client) {
    esp_http_client_set_user_data(http_client, nullptr);
    esp_http_client_cleanup(http_client);
    http_client = nullptr;
  }
  http_server.clear();
  http_ca.clear();
  http_name.clear();
}
static esp_err_t http_event(esp_http_client_event_t *e) {
  auto *out = static_cast<Http *>(e->user_data);
  if (!out)
    return ESP_OK;
  if (e->event_id == HTTP_EVENT_ON_HEADER && e->header_key &&
      strcasecmp(e->header_key, "ETag") == 0)
    out->etag = e->header_value;
  if (e->event_id == HTTP_EVENT_ON_DATA) {
    if (out->body.size() + e->data_len > out->limit) {
      out->overflow = true;
      return ESP_FAIL;
    }
    out->body.append(static_cast<char *>(e->data), e->data_len);
  }
  return ESP_OK;
}
static void log_http_result(int status, esp_err_t err, size_t bytes,
                            int64_t started) {
  ESP_LOGI("readocs-net",
           "http status=%d err=%s bytes=%u elapsed_ms=%lld remaining_ms=%lld",
           status, esp_err_to_name(err), static_cast<unsigned>(bytes),
           static_cast<long long>(now_ms() - started),
           static_cast<long long>(deadline - now_ms()));
}
static int request(const Config &c, const std::string &path, Http &out,
                   const std::string &body = "", const std::string &etag = "",
                   bool retried_local = false) {
  const auto started = now_ms();
  if (now_ms() >= deadline) {
    error_text = "联网超时，已保留离线内容";
    log_http_result(-1, ESP_ERR_TIMEOUT, 0, started);
    return -1;
  }
  const bool local = !local_proxy_failed && !c.local_server.empty();
  const auto &server = local ? c.local_server : c.server;
  const auto &ca = local ? c.local_ca : c.ca;
  const std::string name = local ? c.local_name : "";
  const std::string url = server + path;
  if (http_client && (http_server != server || http_ca != ca || http_name != name))
    close_http();
  if (!http_client) {
    http_server = server;
    http_ca = ca;
    http_name = name;
    esp_http_client_config_t h{};
    h.url = url.c_str();
    h.timeout_ms = std::clamp<int64_t>(deadline - now_ms(), 1, local ? 2500 : external_cache_enabled() ? 15000 : 5000);
    h.user_agent = "Readocs/0.2.10 (PaperDIY; ESP32-S3)";
    h.common_name = http_name.empty() ? nullptr : http_name.c_str();
    h.event_handler = http_event;
    h.disable_auto_redirect = true;
    h.keep_alive_enable = true;
    if (http_ca.empty())
      h.crt_bundle_attach = esp_crt_bundle_attach;
    else
      h.cert_pem = http_ca.c_str();
    http_client = esp_http_client_init(&h);
  }
  if (!http_client) {
    error_text = "代理连接初始化失败，旧内容已保留";
    log_http_result(-1, ESP_ERR_NO_MEM, 0, started);
    return -1;
  }
  auto client = http_client;
  auto header = [client](const char *key, const char *value) {
    auto err = esp_http_client_set_header(client, key, value);
    return err == ESP_OK || (!value && err == ESP_ERR_NOT_FOUND);
  };
  auto post_error = esp_http_client_set_post_field(
      client, body.empty() ? nullptr : body.data(), body.size());
  bool configured =
      esp_http_client_set_url(client, url.c_str()) == ESP_OK &&
      esp_http_client_set_timeout_ms(
          client, std::clamp<int64_t>(deadline - now_ms(), 1, local ? 2500 : external_cache_enabled() ? 15000 : 5000)) ==
          ESP_OK &&
      esp_http_client_set_user_data(client, &out) == ESP_OK &&
      header("X-Protocol-Version", "1") &&
      header("Accept-Encoding", "identity") &&
      header("Authorization",
             c.token.empty() ? nullptr : ("Bearer " + c.token).c_str()) &&
      header("If-None-Match", etag.empty() ? nullptr : etag.c_str()) &&
      esp_http_client_set_method(client, body.empty()
                                             ? HTTP_METHOD_GET
                                             : HTTP_METHOD_POST) == ESP_OK &&
      header("Content-Type", body.empty() ? nullptr : "application/json") &&
      (post_error == ESP_OK ||
       (body.empty() && post_error == ESP_ERR_NOT_FOUND));
  if (!configured) {
    close_http();
    error_text = "代理请求初始化失败，旧内容已保留";
    log_http_result(-1, ESP_FAIL, 0, started);
    return -1;
  }
  auto err = esp_http_client_perform(client);
  int status = err == ESP_OK && !out.overflow
                   ? esp_http_client_get_status_code(client)
                   : -1;
  log_http_result(status, err, out.body.size(), started);
  // Event callbacks must never retain the address of this stack response.
  esp_http_client_set_user_data(client, nullptr);
  esp_http_client_set_post_field(client, nullptr, 0);
  if (err != ESP_OK || out.overflow)
    close_http();
  if (err != ESP_OK && !out.overflow && body.empty() && local) {
    out.body.clear(); out.etag.clear();
    // Publishing a checkpoint can outlast the proxy's keep-alive timeout.
    // Retry a safe GET on a fresh local connection before abandoning LAN.
    if (!retried_local && now_ms() < deadline)
      return request(c, path, out, body, etag, true);
    local_proxy_failed = true;
    return request(c, path, out, body, etag);
  }
  if (err != ESP_OK && !out.overflow && body.empty() && !http_retry_used &&
      deadline - now_ms() > 5000) {
    http_retry_used = true;
    out.body.clear();
    out.etag.clear();
    return request(c, path, out, body, etag);
  }
  if (status == 401)
    error_text = "设备绑定已失效，请重新绑定";
  else if (status == 429)
    error_text = "检查过于频繁，请稍后重试";
  else if (status < 0)
    error_text = out.overflow ? "代理响应超过设备限制，旧内容已保留"
                             : "Wi-Fi 已连接，代理未连接";
  else if (status >= 400) {
    Json j(cJSON_Parse(out.body.c_str()));
    error_text = str(j.get(), "message");
    if (error_text.empty())
      error_text = "代理请求失败，旧内容已保留";
  }
  return status;
}
static bool body_failure(const char *stage, const char *message, int chunk = -1,
                         bool preserve_error = false) {
  if (!preserve_error || error_text.empty())
    error_text = message;
  ESP_LOGW("readocs-net", "body stage=%s chunk=%d", stage, chunk);
  return false;
}
static bool download(const Config &c, cJSON *ref, std::string &id) {
  if (!cJSON_IsObject(ref))
    return body_failure("reference", "正文引用格式无效，旧内容已保留");
  id = str(ref, "snapshot_id");
  auto bytes = num(ref, "bytes");
  if (!valid_hash(id) || bytes < 1 || bytes > 300 * 1024 ||
      str(ref, "sha256") != id)
    return body_failure("reference", "正文引用超出限制或校验无效，旧内容已保留");
  if (cache_body_verified(id) || sha256(read_file(id)) == id)
    return true;
  if (!cache_has_space(bytes)) {
    storage_full = true;
    return body_failure("storage_full", "SD 卡空间不足，已下载内容已保留");
  }
  auto *chunks = cJSON_GetObjectItemCaseSensitive(ref, "chunks");
  int count = cJSON_GetArraySize(chunks);
  if (!cJSON_IsArray(chunks) || !count || count > 10 ||
      count != (bytes + 32767) / 32768)
    return body_failure("chunk_list", "正文分块清单不完整，旧内容已保留");
  std::string body;
  body.reserve(bytes);
  for (int i = 0; i < count; ++i) {
    auto *ch = cJSON_GetArrayItem(chunks, i);
    Http out{{}, {}, 32768};
    if (!cJSON_IsObject(ch) || num(ch, "index", -1) != i ||
        num(ch, "bytes") != std::min(32768, bytes - i * 32768) ||
        !valid_hash(str(ch, "sha256")))
      return body_failure("chunk_metadata", "正文分块引用无效，旧内容已保留", i);
    if (request(c, "/v1/snapshots/" + id + "/chunks/" + std::to_string(i),
                out) != 200)
      return body_failure("chunk_request", "正文分块下载失败，旧内容已保留", i,
                          true);
    if (static_cast<int>(out.body.size()) != num(ch, "bytes"))
      return body_failure("chunk_size", "正文分块长度不符，旧内容已保留", i);
    if (sha256(out.body) != str(ch, "sha256"))
      return body_failure("chunk_hash", "正文分块校验失败，旧内容已保留", i);
    body += out.body;
  }
  if (static_cast<int>(body.size()) != bytes || sha256(body) != id)
    return body_failure("body_hash", "正文整体校验失败，旧内容已保留");
  Json root(cJSON_Parse(body.c_str()));
  if (!root || num(root.get(), "schema_version") != 1)
    return body_failure("schema", "正文格式不兼容，请更新固件");
  auto *blocks = cJSON_GetObjectItemCaseSensitive(root.get(), "blocks");
  if (!cJSON_IsArray(blocks) || cJSON_GetArraySize(blocks) > 500)
    return body_failure("structure", "正文结构超出限制，旧内容已保留");
  std::vector<std::string> ids;
  cJSON *b;
  cJSON_ArrayForEach(b, blocks) {
    auto bid = str(b, "block_id");
    if (bid.empty() || std::find(ids.begin(), ids.end(), bid) != ids.end() ||
        str(b, "text").size() > 65536)
      return body_failure("structure", "正文段落结构无效，旧内容已保留");
    ids.push_back(bid);
  }
  if (!write_file(id, body))
    return body_failure("body_write", "正文缓存写入失败，旧内容已保留");
  sync_made_progress = true;
  return true;
}
static constexpr size_t directory_limit = 512 * 1024;
static bool safe_id(const std::string &id) {
  return !id.empty() && id.size() <= 128 &&
         id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuv"
                              "wxyz0123456789_-") == std::string::npos;
}
static bool save_directory_progress(const std::vector<Entry> &entries,
                                    const std::string &etag,
                                    const std::string &directory_revision,
                                    int source, int cursor) {
  Json root(cJSON_CreateObject());
  cJSON_AddStringToObject(root.get(), "etag", etag.c_str());
  cJSON_AddStringToObject(root.get(), "directory_revision",
                          directory_revision.c_str());
  cJSON_AddNumberToObject(root.get(), "source", source);
  cJSON_AddNumberToObject(root.get(), "cursor", cursor);
  auto *array = cJSON_AddArrayToObject(root.get(), "entries");
  for (const auto &e : entries) {
    auto *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "source_id", e.source_id.c_str());
    cJSON_AddStringToObject(item, "document_id", e.document_id.c_str());
    cJSON_AddStringToObject(item, "title", e.title.c_str());
    cJSON_AddStringToObject(item, "snapshot_id", e.snapshot_id.c_str());
    cJSON_AddStringToObject(item, "target_snapshot_id", e.target_snapshot_id.c_str());
    cJSON_AddItemToArray(array, item);
  }
  auto hash = sha256(serialize(root.get()));
  cJSON_AddStringToObject(root.get(), "hash", hash.c_str());
  auto body = serialize(root.get());
  if (body.size() > directory_limit) {
    error_text = "目录超过设备容量，请在管理页阅读";
    return false;
  }
  if (!save_directory_progress_file(body)) {
    error_text = "目录进度保存失败，旧内容已保留";
    return false;
  }
  return true;
}
static void compact_directory_progress(const std::string &source,
                                       const std::string &document) {
  Json pending(cJSON_Parse(directory_progress().c_str()));
  auto *array = cJSON_GetObjectItemCaseSensitive(pending.get(), "entries");
  if (!cJSON_IsArray(array))
    return;
  std::vector<Entry> entries;
  cJSON *item;
  cJSON_ArrayForEach(item, array)
      entries.push_back({str(item, "source_id"), str(item, "document_id"),
                         str(item, "title"), str(item, "snapshot_id"), str(item, "target_snapshot_id")});
  auto compact = entries;
  trim_cached_articles(compact, source, document, "", "", external_cache_enabled());
  if (compact == entries)
    return;
  // This also completes a migration interrupted after the active-index
  // switch but before its old checkpoint was retired. Preserve pagination
  // when space permits; otherwise restart only the directory scan.
  if (!save_directory_progress(compact, str(pending.get(), "etag"),
                               str(pending.get(), "directory_revision"),
                               num(pending.get(), "source", -1),
                               num(pending.get(), "cursor", -1)))
    clear_directory_progress();
  collect_cache(true);
}
static bool fetch_entry(const Config &c, Entry &entry) {
  Http response{{}, {}, 4096};
  if (!safe_id(entry.source_id) || !safe_id(entry.document_id))
    return body_failure("identity", "文档标识无效，旧内容已保留");
  if (request(c, "/v1/sources/" + entry.source_id + "/documents/" +
                  entry.document_id,
              response) != 200)
    return body_failure("reference_request", "正文引用获取失败，旧内容已保留", -1,
                        true);
  Json item(cJSON_Parse(response.body.c_str()));
  if (!item || str(item.get(), "document_id") != entry.document_id)
    return body_failure("reference_json", "正文引用返回无效，旧内容已保留");
  auto *ref = cJSON_GetObjectItemCaseSensitive(item.get(), "snapshot");
  if (cJSON_IsNull(ref)) {
    entry.target_snapshot_id = "-"; // Known not ready; wait for a new manifest.
    return true;
  }
  if (!cJSON_IsObject(ref))
    return body_failure("reference_json", "正文引用返回无效，旧内容已保留");
  std::string id;
  if (!download(c, ref, id))
    return false;
  entry.snapshot_id = entry.target_snapshot_id = id;
  return true;
}
static bool repair_external_entries(std::vector<Entry> &entries, bool yield_to_check = false) {
  if (!external_cache_enabled()) return true;
  // Verify once in the network task, rather than reading every document on
  // each key wake. Missing/damaged card objects are fetched again, and must
  // not prevent an unrelated verified batch from committing.
  for (auto &entry : entries) {
    if (yield_to_check) {
      std::lock_guard<std::mutex> g(model.mutex);
      if (model.check_queued) return false;
    }
    if (!entry.snapshot_id.empty() && !cache_body_verified(entry.snapshot_id) &&
        sha256(read_file(entry.snapshot_id)) != entry.snapshot_id)
      entry.snapshot_id.clear();
  }
  return true;
}
static bool refresh_sources(const Config &c, bool manual) {
  std::string id;
  { std::lock_guard<std::mutex> g(model.mutex); id = model.refresh_id; }
  if (manual && id.empty()) {
    set_status("正在触发所有来源更新");
    if (!c.local_server.empty() && !local_proxy_failed) {
      // Select LAN/public with a retryable GET before the one-shot POST.
      // Away from the saved LAN, a failed POST cannot safely be replayed.
      Http route{{}, {}, 8192};
      if (request(c, "/v1/status", route) != 200) return false;
    }
    Http out{{}, {}, 4096};
    // POST is deliberately sent once. A lost response is not replayed.
    if (request(c, "/v1/refresh", out, "{}") != 202) return false;
    Json reply(cJSON_Parse(out.body.c_str()));
    id = str(reply.get(), "refresh_id");
    if (id.empty() || id.size() > 64 ||
        id.find_first_not_of("0123456789abcdef") != std::string::npos) {
      error_text = "代理未返回更新进度，请更新代理";
      return false;
    }
    if (!save_refresh_request(id)) {
      error_text = "更新已提交，但进度保存失败";
      return false;
    }
    std::lock_guard<std::mutex> g(model.mutex);
    model.refresh_id = id;
    model.refresh_waiting = model.sync_pending = model.check_notice = true;
  }
  if (id.empty()) return true;
  Http out{{}, {}, 8192};
  if (request(c, "/v1/status?refresh_id=" + id, out) != 200) return false;
  Json reply(cJSON_Parse(out.body.c_str()));
  auto *progress = cJSON_GetObjectItemCaseSensitive(reply.get(), "refresh");
  const auto state = str(progress, "state");
  if (num(reply.get(), "schema_version") != 1 || str(progress, "id") != id ||
      (state != "running" && state != "completed" && state != "partial" &&
       state != "expired" && state != "superseded")) {
    error_text = "代理更新进度无效，旧内容已保留";
    return false;
  }
  const bool waiting = state == "running";
  if (!waiting && !save_refresh_request("")) {
    error_text = "更新进度保存失败，请稍后再试";
    return false;
  }
  std::string message;
  if (waiting) {
    message = "来源更新 " + std::to_string(num(progress, "completed_sources")) +
        "/" + std::to_string(num(progress, "total_sources"));
    if (num(progress, "total_documents") > 0)
      message += "，正文 " + std::to_string(num(progress, "processed_documents")) +
          "/" + std::to_string(num(progress, "total_documents"));
  } else if (state == "partial")
    message = "部分来源更新失败，已保留可用内容";
  else if (state != "completed")
    message = "来源更新未完成，请重新检查";
  {
    std::lock_guard<std::mutex> g(model.mutex);
    model.refresh_waiting = waiting;
    model.refresh_failed = state != "running" && state != "completed";
    model.refresh_message = message;
    if (!waiting) model.refresh_id.clear();
    if (waiting) model.sync_pending = true;
  }
  set_status(message.empty() ? "正在同步内容到终端" : message);
  return true;
}
static void sync_status() {
  std::string message;
  {
    std::lock_guard<std::mutex> g(model.mutex);
    message = model.refresh_waiting || model.refresh_failed ? model.refresh_message :
        model.sync_pending ? "正文下载中，将自动接续" : "已同步";
  }
  set_status(message);
}
static bool sync(bool manual) {
  auto c = config_copy();
  if (c.server.empty() || c.token.empty()) {
    error_text = "在管理页生成配对码，通过 USB 绑定设备";
    return false;
  }
  if (!ensure_wifi() || !ensure_time())
    return false;
  {
    std::lock_guard<std::mutex> g(model.mutex);
    if (!manual && model.check_queued) return true;
  }
  // Start upstream discovery/fetches before any manifest or SD prefetch work.
  if (!refresh_sources(c, manual)) return false;
  std::string etag;
  std::vector<Entry> old;
  std::vector<Source> old_sources;
  {
    std::lock_guard<std::mutex> g(model.mutex);
    etag = model.etag;
    old = model.entries;
    old_sources = model.sources;
  }
  // An explicit check may arrive during a cold-boot SD integrity walk.
  // Yield between files so its POST is not stranded behind the whole card.
  if (!repair_external_entries(old, !manual)) return true;
  const auto selected = current_article();
  if (!compact_cache_for_download(
          old, selected.first, selected.second,
          [&](const std::vector<Entry> &compact) {
            if (!commit_cache(compact, etag, old_sources))
              return false;
            std::lock_guard<std::mutex> g(model.mutex);
            model.entries = compact;
            redraw = true;
            return true;
          },
          clear_directory_progress, [] { collect_cache(true); }, external_cache_enabled())) {
    error_text = "缓存空间不足，旧版本已保留";
    return false;
  }
  compact_directory_progress(selected.first, selected.second);
  // An added SD card expands the existing prefetch set, even if the
  // upstream manifest has not changed. Do not strand missing bodies on 304.
  const bool missing = external_cache_enabled() && std::any_of(old.begin(), old.end(),
      [](const Entry &e) { return e.snapshot_id.empty() && e.target_snapshot_id != "-"; });
  Http out{{}, {}, 8192};
  int status = request(c, "/v1/sync", out, "", manual || missing ? "" : etag);
  if (status != 200 && status != 304)
    return false;
  if (status == 200) {
    collect_cache();
    Json manifest(cJSON_Parse(out.body.c_str()));
    if (!manifest || num(manifest.get(), "schema_version") != 1) {
      error_text = "内容格式不兼容，请更新固件";
      return false;
    }
    std::vector<Entry> entries;
    bool complete = true;
    auto *sources = cJSON_GetObjectItemCaseSensitive(manifest.get(), "sources");
    std::vector<Source> declared_sources;
    if (!read_source_metadata(sources, declared_sources)) {
      error_text = "来源清单无效，旧内容已保留";
      return false;
    }
    // Publish names/counts before walking any directory pages. Keep the old
    // ETag until the complete directory and body prefetch have been committed,
    // otherwise a 304 could strand an unfinished scan after a restart.
    auto retained = old;
    retain_source_entries(retained, declared_sources);
    if (declared_sources != old_sources || retained != old) {
      if (!commit_cache(retained, etag, declared_sources)) {
        error_text = "来源清单保存失败，旧内容已保留";
        return false;
      }
      {
        std::lock_guard<std::mutex> g(model.mutex);
        model.entries = retained;
        model.sources = declared_sources;
        model.sync_pending = true;
      }
      old = std::move(retained);
      redraw = true;
    }
    Json directory(cJSON_Duplicate(sources, true));
    if (!directory)
      return false;
    cJSON *directory_source;
    cJSON_ArrayForEach(directory_source, directory.get())
        cJSON_DeleteItemFromObjectCaseSensitive(directory_source,
                                                "body_revision");
    const auto directory_revision = sha256(serialize(directory.get()));
    int source_index = 0, cursor = 0;
    Json pending(cJSON_Parse(directory_progress().c_str()));
    if (pending &&
        (str(pending.get(), "directory_revision") == directory_revision ||
         str(pending.get(), "etag") == out.etag)) {
      auto hash = str(pending.get(), "hash");
      cJSON_DeleteItemFromObjectCaseSensitive(pending.get(), "hash");
      if (sha256(serialize(pending.get())) == hash) {
        source_index = num(pending.get(), "source", -1);
        cursor = num(pending.get(), "cursor", -1);
        cJSON *item;
        cJSON_ArrayForEach(
            item, cJSON_GetObjectItemCaseSensitive(pending.get(), "entries")) {
          entries.push_back({str(item, "source_id"), str(item, "document_id"),
                             str(item, "title"), str(item, "snapshot_id"), str(item, "target_snapshot_id")});
          // A body-only manifest update preserves the directory revision.
          // Recheck staged targets once when its ETag changes, otherwise a
          // completed checkpoint could acknowledge unseen newer bodies.
          if (str(pending.get(), "etag") != out.etag)
            entries.back().target_snapshot_id.clear();
        }
      }
    }
    size_t expected = 0;
    for (const auto &source : declared_sources) {
      if (static_cast<size_t>(source.count) >
          std::numeric_limits<size_t>::max() - expected)
        return false;
      expected += source.count;
    }
    if (source_index < 0 || source_index > cJSON_GetArraySize(sources) ||
        cursor < 0 || entries.size() > expected ||
        (source_index == cJSON_GetArraySize(sources) &&
         entries.size() != expected)) {
      entries.clear();
      source_index = cursor = 0;
      clear_directory_progress();
    }
    merge_cached_entries(entries, old);
    while (source_index < cJSON_GetArraySize(sources)) {
      if (now_ms() + 3500 >= deadline) {
        if (!save_directory_progress(entries, out.etag, directory_revision,
                                     source_index, cursor))
          return false;
        {
          std::lock_guard<std::mutex> g(model.mutex);
          model.sync_pending = true;
        }
        set_status("目录同步中，将自动接续");
        return true;
      }
      auto *source = cJSON_GetArrayItem(sources, source_index);
      std::string sid = str(source, "id");
      if (!safe_id(sid))
        return false;
      std::string revision = str(source, "catalog_revision");
      Http cat{{}, {}, 65536};
      if (request(c,
                  "/v1/sources/" + sid + "/catalog?compact=true&revision=" +
                      revision + "&cursor=" + std::to_string(cursor),
                  cat) != 200)
        return false;
      Json catalog(cJSON_Parse(cat.body.c_str()));
      if (!catalog || str(catalog.get(), "catalog_revision") != revision)
        return false;
      auto *items = cJSON_GetObjectItemCaseSensitive(catalog.get(), "items");
      if (!cJSON_IsArray(items))
        return false;
      cJSON *item;
      cJSON_ArrayForEach(item, items) {
        Entry e{sid, str(item, "document_id"), str(item, "title"), "", str(item, "snapshot_id")};
        if (!safe_id(e.document_id) || e.title.size() > 1024 ||
            (!e.target_snapshot_id.empty() && !valid_hash(e.target_snapshot_id)))
          return false;
        if (e.target_snapshot_id.empty()) e.target_snapshot_id = "-";
        auto prior = std::find_if(old.begin(), old.end(), [&](const Entry &p) {
          return p.source_id == sid && p.document_id == e.document_id;
        });
        if (prior != old.end()) {
          e.snapshot_id = prior->snapshot_id;
        }
        entries.push_back(e);
      }
      auto *next =
          cJSON_GetObjectItemCaseSensitive(catalog.get(), "next_cursor");
      if (cJSON_IsNumber(next)) {
        if (next->valueint <= cursor)
          return false;
        cursor = next->valueint;
      } else {
        ++source_index;
        cursor = 0;
      }
      if (!save_directory_progress(entries, out.etag, directory_revision,
                                   source_index, cursor))
        return false;
    }
    // The complete directory is independent from the small offline body cache.
    if (entries.size() != expected) {
      clear_directory_progress();
      error_text = "目录数量不一致，请重新检查";
      return false;
    }
    const auto current = current_article();
    const auto keep = cached_article_mask(entries, current.first, current.second, "", "", external_cache_enabled());
    prepare_download_space();
    for (size_t i = 0; i < entries.size(); ++i) {
      auto &e = entries[i];
      if (!keep[i]) {
        e.snapshot_id.clear();
        continue;
      }
      if (external_cache_enabled() && e.target_snapshot_id == "-") continue;
      if (e.snapshot_id == e.target_snapshot_id && valid_hash(e.snapshot_id) &&
          (cache_body_verified(e.snapshot_id) || sha256(read_file(e.snapshot_id)) == e.snapshot_id))
        continue;
      if (now_ms() + 1000 >= deadline) { complete = false; break; }
      if (!fetch_entry(c, e) || (external_cache_enabled() && e.snapshot_id.empty() && e.target_snapshot_id != "-"))
        complete = false;
      // Persist each batch before starting more network work. A reset resumes
      // from the last verified body; the old reader index remains the fallback.
      if (external_cache_enabled() && (i + 1) % 12 == 0) {
        if (!commit_cache(entries, etag, declared_sources) ||
            !save_directory_progress(entries, out.etag, directory_revision, source_index, 0))
          return false;
        {
          std::lock_guard<std::mutex> g(model.mutex);
          model.entries = entries;
          model.sources = declared_sources;
        }
        redraw = true;
      }
      if (storage_full) { complete = false; break; }
    }
    const auto saved_etag = complete ? out.etag : etag;
    if (!commit_cache(entries, saved_etag, declared_sources)) {
      error_text = "缓存保存失败，旧版本已保留";
      return false;
    }
    {
      std::lock_guard<std::mutex> g(model.mutex);
      model.entries = entries;
      model.sources = declared_sources;
      model.etag = saved_etag;
    }
    if (complete)
      clear_directory_progress();
    else if (!save_directory_progress(entries, out.etag, directory_revision,
                                      source_index, 0))
      return false;
    {
      std::lock_guard<std::mutex> g(model.mutex);
      model.sync_pending = (!complete && !storage_full) || model.refresh_waiting;
    }
    collect_cache(); // Release versions older than the committed fallback.
    redraw = true;
  } else {
    // The selected article may have changed since the last manifest was
    // committed. Check it even on 304, including articles outside the prefetch
    // set; a nonempty local reference must not make an old body permanent.
    const auto current = current_article();
    auto entries = old;
    trim_cached_articles(entries, current.first, current.second, "", "", external_cache_enabled());
    auto selected =
        std::find_if(entries.begin(), entries.end(), [&](const Entry &e) {
          return e.source_id == current.first && e.document_id == current.second;
        });
    prepare_download_space();
    if (selected != entries.end() && !fetch_entry(c, *selected))
      return false;
    if (!commit_cache(entries, etag, old_sources)) {
      error_text = "缓存保存失败，旧版本已保留";
      return false;
    }
    if (entries != old) {
      std::lock_guard<std::mutex> g(model.mutex);
      model.entries = entries;
      redraw = true;
    }
    collect_cache();
  }
  if (storage_full) { set_status("SD 卡空间不足，已下载内容已保留"); return true; }
  {
    std::lock_guard<std::mutex> g(model.mutex);
    // A 304 acknowledges the complete local snapshot. Upstream work may
    // remain, but its previous pending flag must not survive completion.
    if (status == 304) model.sync_pending = false;
    model.sync_pending = model.sync_pending || model.refresh_waiting;
  }
  sync_status();
  return true;
}
static bool download_article(const cJSON *input) {
  auto c = config_copy();
  if (c.token.empty() || !ensure_wifi() || !ensure_time())
    return false;
  const auto source = str(input, "source_id"), doc = str(input, "document_id");
  if (!safe_id(source) || !safe_id(doc))
    return false;
  std::vector<Entry> entries;
  std::vector<Source> sources;
  std::string etag;
  {
    std::lock_guard<std::mutex> g(model.mutex);
    entries = model.entries;
    sources = model.sources;
    etag = model.etag;
  }
  repair_external_entries(entries);
  auto selected =
      std::find_if(entries.begin(), entries.end(), [&](const Entry &e) {
        return e.source_id == source && e.document_id == doc;
      });
  if (selected == entries.end())
    return false;
  prepare_download_space();
  if (!fetch_entry(c, *selected))
    return false;
  if (selected->snapshot_id.empty())
    return body_failure("not_ready", "正文尚未同步，请稍后重试");
  // Selection can change while the queued download is running. Keep both the
  // displayed selection and its just-completed request until the next sync.
  const auto current = current_article();
  trim_cached_articles(entries, current.first, current.second, source, doc, external_cache_enabled());
  if (!commit_cache(entries, etag, sources))
    return body_failure("cache_commit", "目录缓存保存失败，旧内容已保留");
  {
    std::lock_guard<std::mutex> g(model.mutex);
    model.entries = entries;
  }
  collect_cache();
  set_status("正文已下载");
  return true;
}

bool stop_setup() {
  if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) {
    error_text = "热点关闭失败，请重试或重启设备";
    set_status(error_text);
    return false;
  }
  {
    std::lock_guard<std::mutex> g(model.mutex);
    model.ap = false;
    model.ap_session.clear();
    std::fill(model.ap_password.begin(), model.ap_password.end(), 0);
    model.ap_password.clear();
    setup_operation.pending = false;
    setup_operation.close_at = 0;
  }
  if (!networking) {
    connected = false;
    if (esp_wifi_stop() != ESP_OK) {
      error_text = "热点已关闭，但 Wi-Fi 停止失败，请重启设备";
      set_status(error_text);
      return false;
    }
  }
  redraw = true;
  return true;
}
bool start_setup(const std::string &password) {
  if (!password.empty() && (password.size() < 12 || password.size() > 63)) {
    error_text = "USB 指定热点密码须为 12–63 字节";
    return false;
  }
  uint8_t mac[6];
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
    error_text = "热点设备标识读取失败";
    return false;
  }
  char ssid[32], pass[17], session[33];
  std::snprintf(ssid, sizeof(ssid), "Readocs-%02X%02X", mac[4], mac[5]);
  std::snprintf(pass, sizeof(pass), "%08lx%08lx",
                static_cast<unsigned long>(esp_random()),
                static_cast<unsigned long>(esp_random()));
  std::snprintf(session, sizeof(session), "%08lx%08lx%08lx%08lx",
                static_cast<unsigned long>(esp_random()),
                static_cast<unsigned long>(esp_random()),
                static_cast<unsigned long>(esp_random()),
                static_cast<unsigned long>(esp_random()));
  wifi_config_t ap{};
  std::strcpy(reinterpret_cast<char *>(ap.ap.ssid), ssid);
  std::strcpy(reinterpret_cast<char *>(ap.ap.password), pass);
  if (!password.empty()) {
    std::memset(ap.ap.password, 0, sizeof(ap.ap.password));
    std::memcpy(ap.ap.password, password.data(), password.size());
  }
  ap.ap.ssid_len = std::strlen(ssid);
  ap.ap.channel = 1;
  ap.ap.max_connection = 2;
  ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
  {
    std::lock_guard<std::mutex> g(model.mutex);
    model.ap = false;
    model.ap_session.clear();
    setup_operation = {};
  }
  auto err = esp_wifi_set_mode(WIFI_MODE_APSTA);
  if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &ap);
  if (err == ESP_OK) err = esp_wifi_start();
  esp_netif_ip_info_t ip{};
  bool ready = false;
  for (unsigned i = 0; err == ESP_OK && i < 40; ++i) {
    esp_netif_dhcp_status_t dhcp;
    if (esp_netif_is_netif_up(ap_netif) &&
        esp_netif_get_ip_info(ap_netif, &ip) == ESP_OK && ip.ip.addr &&
        esp_netif_dhcps_get_status(ap_netif, &dhcp) == ESP_OK && dhcp == ESP_NETIF_DHCP_STARTED) {
      ready = true;
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  if (!ready) {
    const auto restored = esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGW("readocs-setup", "AP start failed err=%s restore=%s", esp_err_to_name(err), esp_err_to_name(restored));
    error_text = "热点启动失败，请重试或通过 USB 配网";
    return false;
  }
  {
    std::lock_guard<std::mutex> g(model.mutex);
    model.ap_ssid = ssid;
    model.ap_password = password.empty() ? pass : password;
    model.ap_session = session;
    model.ap = true;
    setup_operation.message = "请填写目标 Wi-Fi，连接失败时原配置保留";
  }
  ap_activity = now_ms();
  redraw = true;
  set_status("请连接屏幕所示热点完成配网");
  return true;
}
static bool on_ap(httpd_req_t *req, const char **denial = nullptr) {
  sockaddr_storage local{};
  socklen_t len = sizeof(local);
  esp_netif_ip_info_t info{};
  const char *code = nullptr;
  {
    std::lock_guard<std::mutex> g(model.mutex);
    if (!model.ap) code = "setup_closed";
    else if (now_ms() - ap_activity >= 300000) code = "setup_expired";
  }
  if (!code && (esp_netif_get_ip_info(ap_netif, &info) != ESP_OK || !info.ip.addr))
    code = "ap_address_unavailable";
  if (!code && getsockname(httpd_req_to_sockfd(req),
                          reinterpret_cast<sockaddr *>(&local), &len) != 0)
    code = "socket_address_unavailable";
  uint32_t ipv4 = 0;
  if (!code) code = setup_ipv4(local, len, ipv4);
  if (!code && ipv4 != info.ip.addr) code = "setup_channel_mismatch";
  // Also reject a STA/routed peer reaching the AP address through weak-host routing.
  sockaddr_storage peer{};
  socklen_t peer_len = sizeof(peer);
  if (!code && getpeername(httpd_req_to_sockfd(req), reinterpret_cast<sockaddr *>(&peer), &peer_len) != 0)
    code = "peer_address_unavailable";
  uint32_t peer_ipv4 = 0;
  if (!code) code = setup_ipv4(peer, peer_len, peer_ipv4);
  if (!code && (!info.netmask.addr ||
      (peer_ipv4 & info.netmask.addr) != (info.ip.addr & info.netmask.addr)))
    code = "peer_channel_mismatch";
  if (denial) *denial = code;
  if (code)
    ESP_LOGW("readocs-setup", "denied reason=%s family=%d length=%u ipv6=%d",
             code, local.ss_family, static_cast<unsigned>(len), CONFIG_LWIP_IPV6);
  return !code;
}
static esp_err_t send_json(httpd_req_t *req, cJSON *j) {
  auto s = serialize(j);
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_send(req, s.data(), s.size());
}
static esp_err_t setup_error(httpd_req_t *req, const char *status,
                              const char *code, const char *message) {
  httpd_resp_set_status(req, status);
  Json j(cJSON_CreateObject());
  cJSON_AddStringToObject(j.get(), "code", code);
  cJSON_AddStringToObject(j.get(), "message", message);
  return send_json(req, j.get());
}
static void setup_result(const std::string &state, const std::string &code,
                         const std::string &message, int64_t close_delay = 0) {
  std::lock_guard<std::mutex> g(model.mutex);
  setup_operation.state = state;
  setup_operation.code = code;
  setup_operation.message = message;
  setup_operation.pending = false;
  setup_operation.close_at = model.ap && close_delay ? now_ms() + close_delay : 0;
}
static bool setup_failure(const char *code, const char *message) {
  error_text = message;
  setup_result("failed", code, message);
  return false;
}
static bool configure_wifi(const cJSON *input) {
  auto c = config_copy();
  Profile p{str(input, "ssid"), str(input, "password")};
  if (p.ssid.empty() || p.ssid.size() > 32 ||
      (!p.password.empty() && (p.password.size() < 8 || p.password.size() > 63)))
    return setup_failure("invalid_wifi", "Wi-Fi 名称或密码格式错误");
  auto it = std::find_if(c.networks.begin(), c.networks.end(),
                        [&](const Profile &old) { return old.ssid == p.ssid; });
  if (it == c.networks.end() && c.networks.size() >= 5) {
    auto replace = str(input, "replace");
    auto old = std::find_if(c.networks.begin(), c.networks.end(),
                          [&](const Profile &n) { return n.ssid == replace; });
    if (old == c.networks.end())
      return setup_failure("replacement_required", "请选择要替换的已保存网络");
    c.networks.erase(old);
  }
  {
    std::lock_guard<std::mutex> g(model.mutex);
    if (!setup_operation.pending) ++setup_operation.id;
    setup_operation.pending = true;
    setup_operation.state = "connecting";
    setup_operation.kind = "wifi";
    setup_operation.code.clear();
    setup_operation.message = "正在连接 Wi-Fi";
    setup_operation.close_at = 0;
  }
  set_status("正在连接 Wi-Fi");
  if (esp_wifi_start() != ESP_OK)
    return setup_failure("wifi_start_failed", "Wi-Fi 启动失败，请重试");
  if (!connect_profile(p)) {
    esp_wifi_disconnect();
    {
      std::lock_guard<std::mutex> g(model.mutex);
      model.ssid.clear(); model.ip.clear();
    }
    const char *message = std::strcmp(connection_error, "auth_failed") == 0
                              ? "密码错误或认证失败，请修改密码重试"
                         : std::strcmp(connection_error, "dhcp_timeout") == 0
                              ? "已关联网络但未取得 IP，请检查路由器 DHCP 后重试"
                         : std::strcmp(connection_error, "network_not_found") == 0
                              ? "没有找到该网络，请确认名称、2.4 GHz 信号或隐藏网络设置"
                              : "连接超时或启动失败，请重试；原网络配置已保留";
    return setup_failure(connection_error, message);
  }
  c.networks.erase(std::remove_if(c.networks.begin(), c.networks.end(),
                                 [&](const Profile &n) { return n.ssid == p.ssid; }),
                  c.networks.end());
  c.networks.insert(c.networks.begin(), p);
  if (!save_config(c)) {
    esp_wifi_disconnect(); connected = false;
    return setup_failure("save_failed", "网络已连接但配置保存失败，原配置保留，请重试");
  }
  {
    std::lock_guard<std::mutex> g(model.mutex);
    model.config = c;
  }
  setup_result("success", "connected", "Wi-Fi 已连接并保存，热点即将关闭", 20000);
  set_status("Wi-Fi 已连接并保存");
  return true;
}
static void service_setup() {
  std::unique_lock<std::mutex> radio(network_radio_mutex, std::try_to_lock);
  if (!radio.owns_lock()) return;
  bool close = false;
  {
    std::lock_guard<std::mutex> g(model.mutex);
    close = model.ap && ((setup_operation.close_at && now_ms() >= setup_operation.close_at) ||
                         now_ms() - ap_activity >= 300000);
  }
  if (close && !stop_setup()) {
    std::lock_guard<std::mutex> g(model.mutex);
    setup_operation.close_at = 0;
    ap_activity = now_ms(); // Avoid repeatedly refreshing the same close failure.
  }
}
static bool setup_action_is_current(const cJSON *input) {
  auto *marker = cJSON_GetObjectItemCaseSensitive(input, "_setup_session");
  if (!marker) return true; // Physical USB actions do not use browser sessions.
  auto session = str(input, "_setup_session");
  if (session.empty()) return false;
  std::lock_guard<std::mutex> g(model.mutex);
  return model.ap && model.ap_session == session &&
         num(input, "_setup_operation") == static_cast<int>(setup_operation.id);
}
static esp_err_t web(httpd_req_t *req) {
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
  const char *denial = nullptr;
  if (!on_ap(req, &denial))
    return setup_error(req, "403 Forbidden", denial,
        std::strcmp(denial, "setup_closed") == 0 || std::strcmp(denial, "setup_expired") == 0
            ? "配网会话已关闭，请在设备上重新开启配网"
            : "请重新连接设备屏幕所示热点，再打开 http://192.168.4.1/");
  std::string path(req->uri);
  if (path.size() > 512)
    return setup_error(req, "414 URI Too Long", "uri_too_long", "请求地址过长");
  path.resize(path.find('?') == std::string::npos ? path.size() : path.find('?'));
  if (path.size() > 96)
    return setup_error(req, "414 URI Too Long", "uri_too_long", "请求路径过长");
  const bool read = path == "/" || path == "/api/status" || path == "/api/scan";
  const bool write = path == "/api/wifi" || path == "/api/forget" || path == "/api/complete";
  if (!read && !write)
    return setup_error(req, "404 Not Found", "not_found", "没有这个配网接口");
  if ((read && req->method != HTTP_GET) || (write && req->method != HTTP_POST)) {
    httpd_resp_set_hdr(req, "Allow", read ? "GET" : "POST");
    return setup_error(req, "405 Method Not Allowed", "method_not_allowed", "请求方法不正确");
  }
  if (path == "/") {
    ap_activity = now_ms();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, PAGE);
  }
  if (req->method == HTTP_GET) {
    Json j(cJSON_CreateObject());
    if (path == "/api/status") {
      std::lock_guard<std::mutex> g(model.mutex);
      cJSON_AddStringToObject(j.get(), "session", model.ap_session.c_str());
      cJSON_AddStringToObject(j.get(), "status", model.status.c_str());
      cJSON_AddBoolToObject(j.get(), "sync_pending", model.sync_pending);
      cJSON_AddBoolToObject(j.get(), "local_proxy_configured", !model.config.local_server.empty());
      cJSON_AddBoolToObject(j.get(), "local_proxy_fallback", local_proxy_failed.load());
      auto *result = cJSON_AddObjectToObject(j.get(), "connection");
      cJSON_AddStringToObject(result, "state", setup_operation.state.c_str());
      cJSON_AddStringToObject(result, "kind", setup_operation.kind.c_str());
      cJSON_AddStringToObject(result, "code", setup_operation.code.c_str());
      cJSON_AddStringToObject(result, "message", setup_operation.message.c_str());
      cJSON_AddNumberToObject(result, "operation", setup_operation.id);
      cJSON_AddBoolToObject(result, "pending", setup_operation.pending);
      auto *a = cJSON_AddArrayToObject(j.get(), "networks");
      for (const auto &p : model.config.networks)
        cJSON_AddItemToArray(a, cJSON_CreateString(p.ssid.c_str()));
    } else {
      ap_activity = now_ms();
      std::unique_lock<std::mutex> radio(network_radio_mutex, std::try_to_lock);
      bool pending;
      { std::lock_guard<std::mutex> g(model.mutex); pending = setup_operation.pending; }
      wifi_scan_config_t scan{};
      scan.show_hidden = true;
      if (!radio.owns_lock() || networking || pending || esp_wifi_scan_start(&scan, true) != ESP_OK)
        return setup_error(req, "409 Conflict", "network_busy", "设备正在联网，请稍后重试扫描");
      wifi_ap_record_t found[20]{};
      uint16_t count = 20;
      if (esp_wifi_scan_get_ap_records(&count, found) != ESP_OK)
        return setup_error(req, "503 Service Unavailable", "scan_failed", "扫描失败，可手动填写网络名称重试");
      auto *a = cJSON_AddArrayToObject(j.get(), "networks");
      for (int i = 0; i < count; ++i) {
        if (!found[i].ssid[0])
          continue;
        auto *n = cJSON_CreateObject();
        cJSON_AddStringToObject(n, "ssid",
                                reinterpret_cast<char *>(found[i].ssid));
        cJSON_AddBoolToObject(n, "secure", found[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(a, n);
      }
    }
    return send_json(req, j.get());
  }
  char token[40]{};
  if (httpd_req_get_hdr_value_str(req, "X-Setup-Session", token,
                                  sizeof(token)) != ESP_OK)
    return setup_error(req, "403 Forbidden", "invalid_session", "缺少有效配网会话，请刷新页面");
  {
    std::lock_guard<std::mutex> g(model.mutex);
    if (model.ap_session != token)
      return setup_error(req, "403 Forbidden", "expired_session", "配网会话已更换，请刷新页面");
  }
  if (req->content_len <= 0 || req->content_len > 512)
    return setup_error(req, "400 Bad Request", "invalid_size", "提交内容大小不正确");
  std::string body(req->content_len, '\0');
  size_t used = 0;
  while (used < body.size()) {
    int n = httpd_req_recv(req, body.data() + used, body.size() - used);
    if (n <= 0)
      return setup_error(req, "408 Request Timeout", "incomplete_body", "提交未接收完整，请重试");
    used += n;
  }
  Json j(cJSON_ParseWithLengthOpts(body.c_str(), body.size() + 1, nullptr, true));
  if (!cJSON_IsObject(j.get()) || body.find('\0') != std::string::npos ||
      body.find("\\u0000") != std::string::npos)
    return setup_error(req, "400 Bad Request", "invalid_json", "提交格式不正确");
  auto ssid = str(j.get(), "ssid"), pass = str(j.get(), "password");
  if (path != "/api/complete" && (ssid.empty() || ssid.size() > 32 ||
      ssid.find('\0') != std::string::npos ||
      (path == "/api/wifi" && !pass.empty() && (pass.size() < 8 || pass.size() > 63))))
    return setup_error(req, "400 Bad Request", "invalid_wifi", "Wi-Fi 名称或密码格式错误");
  unsigned operation;
  {
    std::lock_guard<std::mutex> g(model.mutex);
    if (!model.ap || model.ap_session != token)
      return setup_error(req, "403 Forbidden", "expired_session", "配网会话已更换，请刷新页面");
    if (path == "/api/complete") {
      if (setup_operation.state != "success" || setup_operation.pending)
        return setup_error(req, "409 Conflict", "not_connected", "尚未确认连接并保存成功");
      setup_operation.close_at = now_ms() + 1000;
    } else {
      if (networking || setup_operation.pending)
        return setup_error(req, "409 Conflict", "operation_busy", "已有操作正在执行，请等待结果");
      while (cJSON_GetObjectItemCaseSensitive(j.get(), "_setup_session"))
        cJSON_DeleteItemFromObjectCaseSensitive(j.get(), "_setup_session");
      cJSON_AddStringToObject(j.get(), "_setup_session", model.ap_session.c_str());
      while (cJSON_GetObjectItemCaseSensitive(j.get(), "_setup_operation"))
        cJSON_DeleteItemFromObjectCaseSensitive(j.get(), "_setup_operation");
      cJSON_AddNumberToObject(j.get(), "_setup_operation", setup_operation.id + 1);
      if (!enqueue(path == "/api/forget" ? "forget" : "wifi", serialize(j.get())))
        return setup_error(req, "503 Service Unavailable", "queue_full", "操作队列已满，请稍后重试");
      ++setup_operation.id;
      setup_operation.pending = true;
      setup_operation.state = "queued";
      setup_operation.kind = path == "/api/forget" ? "forget" : "wifi";
      setup_operation.code.clear();
      setup_operation.message = "操作已接收，正在处理";
      setup_operation.close_at = 0;
    }
    operation = setup_operation.id;
    ap_activity = now_ms();
  }
  std::fill(body.begin(), body.end(), 0);
  Json ok(cJSON_CreateObject());
  cJSON_AddBoolToObject(ok.get(), "accepted", true);
  cJSON_AddNumberToObject(ok.get(), "operation", operation);
  return send_json(req, ok.get());
}
static esp_err_t setup_socket_open(httpd_handle_t, int fd) {
  // lwIP binds the accepted TCP PCB to this netif, before HTTP reads begin.
  // Local-address checks alone do not identify the incoming interface.
  struct ifreq interface{};
  if (esp_netif_get_netif_impl_name(ap_netif, interface.ifr_name) != ESP_OK ||
      setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, &interface, sizeof(interface)) != 0) {
    ESP_LOGW("readocs-setup", "setup socket interface binding failed");
    return ESP_FAIL;
  }
  return ESP_OK;
}
void init_network() {
  wifi_events = xEventGroupCreate();
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  sta_netif = esp_netif_create_default_wifi_sta();
  ap_netif = esp_netif_create_default_wifi_ap();
  esp_netif_set_hostname(sta_netif, "readocs");
  wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&wc));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_event_handler_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             wifi_event, nullptr));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
  httpd_handle_t server;
  ESP_LOGI("readocs-setup", "HTTP IPv6=%d", CONFIG_LWIP_IPV6);
  httpd_config_t sc = HTTPD_DEFAULT_CONFIG();
  sc.stack_size = 8192;
  sc.max_uri_handlers = 1;
  sc.uri_match_fn = httpd_uri_match_wildcard;
  sc.open_fn = setup_socket_open;
  ESP_ERROR_CHECK(httpd_start(&server, &sc));
  httpd_uri_t h{};
  h.uri = "/*";
  h.method = static_cast<httpd_method_t>(HTTP_ANY);
  h.handler = web;
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &h));
}
static void status_console() {
  Json j(cJSON_CreateObject());
  cJSON_AddStringToObject(j.get(), "firmware", "readocs-0.2.10");
  cJSON_AddStringToObject(j.get(), "font_family", "Noto Sans SC");
  cJSON_AddNumberToObject(j.get(), "font_weight", reader_font_weight);
  cJSON_AddBoolToObject(j.get(), "fonts_ready", reader_fonts_ready());
  cJSON_AddNumberToObject(j.get(), "font_revision", font_asset_revision);
  cJSON_AddNumberToObject(j.get(), "emoji_sequences", emoji_sequence_count);
  const auto storage = sd_storage_capacity();
  cJSON_AddBoolToObject(j.get(), "sd_available", storage.available);
  cJSON_AddBoolToObject(j.get(), "external_cache", external_cache_enabled());
  cJSON_AddNumberToObject(j.get(), "sd_total_bytes", storage.total);
  cJSON_AddNumberToObject(j.get(), "sd_free_bytes", storage.free);
  cJSON_AddNumberToObject(j.get(), "sd_reserved_bytes", sd_reserved_bytes);
  wifi_ap_record_t access_point{};
  if (esp_wifi_sta_get_ap_info(&access_point) == ESP_OK)
    cJSON_AddNumberToObject(j.get(), "wifi_rssi", access_point.rssi);
  wifi_ps_type_t power_save;
  if (esp_wifi_get_ps(&power_save) == ESP_OK)
    cJSON_AddNumberToObject(j.get(), "wifi_power_save", power_save);
  cJSON_AddBoolToObject(j.get(), "connected", connected.load());
  cJSON_AddBoolToObject(j.get(), "busy", networking.load());
  cJSON_AddNumberToObject(j.get(), "uptime_ms", now_ms());
  cJSON_AddNumberToObject(j.get(), "light_sleeps", light_sleep_count());
  cJSON_AddBoolToObject(j.get(), "light_sleep_available", light_sleep_available());
  cJSON_AddNumberToObject(j.get(), "interaction_window_ms", ui_interaction_ms);
  Json sleep_probe(cJSON_Parse(load_sleep_probe().c_str()));
  if (cJSON_IsObject(sleep_probe.get()))
    cJSON_AddItemToObject(j.get(), "light_sleep_probe", sleep_probe.release());
  {
    std::lock_guard<std::mutex> g(model.mutex);
    cJSON_AddStringToObject(j.get(), "status", model.status.c_str());
    cJSON_AddBoolToObject(j.get(), "setup_active", model.ap);
    cJSON_AddBoolToObject(j.get(), "setup_ipv6", CONFIG_LWIP_IPV6);
    cJSON_AddStringToObject(j.get(), "setup_state", setup_operation.state.c_str());
    cJSON_AddStringToObject(j.get(), "setup_code", setup_operation.code.c_str());
    cJSON_AddNumberToObject(j.get(), "setup_operation", setup_operation.id);
    cJSON_AddBoolToObject(j.get(), "setup_pending", setup_operation.pending);
    cJSON_AddBoolToObject(j.get(), "sync_pending", model.sync_pending);
    cJSON_AddBoolToObject(j.get(), "upstream_refresh_pending", model.refresh_waiting);
    cJSON_AddBoolToObject(j.get(), "check_notice", model.check_notice);
    cJSON_AddBoolToObject(j.get(), "local_proxy_configured", !model.config.local_server.empty());
    cJSON_AddBoolToObject(j.get(), "local_proxy_fallback", local_proxy_failed.load());
    cJSON_AddNumberToObject(j.get(), "font_size", model.config.font_size);
    cJSON_AddStringToObject(j.get(), "ip", model.ip.c_str());
    cJSON_AddStringToObject(j.get(), "ssid", model.ssid.c_str());
    cJSON_AddBoolToObject(j.get(), "paired", !model.config.token.empty());
    cJSON_AddNumberToObject(j.get(), "documents", model.entries.size());
    cJSON_AddNumberToObject(j.get(), "source_count", model.sources.size());
    cJSON_AddNumberToObject(j.get(), "source_index", model.source_index);
    auto *sources = cJSON_AddArrayToObject(j.get(), "sources");
    for (const auto &source : model.sources) {
      auto *item = cJSON_CreateObject();
      cJSON_AddStringToObject(item, "id", source.id.c_str());
      cJSON_AddStringToObject(item, "title", source.title.c_str());
      cJSON_AddNumberToObject(item, "count", source.count);
      cJSON_AddNumberToObject(
          item, "local_documents",
          std::count_if(model.entries.begin(), model.entries.end(),
                        [&](const Entry &e) { return e.source_id == source.id; }));
      cJSON_AddNumberToObject(
          item, "cached_documents",
          std::count_if(model.entries.begin(), model.entries.end(),
                        [&](const Entry &e) {
                          return e.source_id == source.id &&
                                 !e.snapshot_id.empty();
                        }));
      cJSON_AddItemToArray(sources, item);
    }
    cJSON_AddNumberToObject(
        j.get(), "cached_documents",
        std::count_if(model.entries.begin(), model.entries.end(),
                      [](const Entry &e) { return !e.snapshot_id.empty(); }));
    cJSON_AddStringToObject(j.get(), "displayed_snapshot_id",
                            model.displayed_snapshot.c_str());
    cJSON_AddNumberToObject(j.get(), "page", model.page + 1);
    cJSON_AddNumberToObject(j.get(), "pages", model.page_count);
    cJSON_AddStringToObject(j.get(), "view", view_name(model.view));
    cJSON_AddNumberToObject(j.get(), "list_index", model.list_index);
    cJSON_AddStringToObject(j.get(), "selected_source_id",
                            model.selected_source.c_str());
    cJSON_AddStringToObject(j.get(), "selected_document_id",
                            model.selected_document.c_str());
    cJSON_AddStringToObject(j.get(), "current_document_id",
                            model.current_document.c_str());
    cJSON_AddNumberToObject(j.get(), "render_count", model.render_count);
  }
  cJSON_AddNumberToObject(j.get(), "partial_updates",
                          display_partial_count());
  cJSON_AddNumberToObject(j.get(), "partial_limit",
                          display_partial_limit());
  cJSON_AddNumberToObject(j.get(), "pm1_btn_cfg1", pm1_register(0x49));
  cJSON_AddNumberToObject(j.get(), "pm1_btn_cfg2", pm1_register(0x4A));
  cJSON_AddNumberToObject(j.get(), "pm1_btn_irq", pm1_register(0x42));
  cJSON_AddNumberToObject(j.get(), "pm1_btn_mask", pm1_register(0x45));
  cJSON_AddNumberToObject(j.get(), "heap", esp_get_free_heap_size());
  cJSON_AddNumberToObject(
      j.get(), "internal_heap",
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  cJSON_AddNumberToObject(
      j.get(), "internal_heap_largest",
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  cJSON_AddNumberToObject(
      j.get(), "internal_heap_minimum",
      heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  cJSON_AddNumberToObject(j.get(), "psram_heap",
                          heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  std::printf("STATUS %s\n", serialize(j.get()).c_str());
}
void console_task(void *) {
  std::string line;
  bool overflow = false;
  while (true) {
    char ch;
    if (usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(100)) != 1)
      continue;
    if (ch != '\n' && ch != '\r') {
      if (line.size() < 8192)
        line += ch;
      else
        overflow = true;
      continue;
    }
    if (line.empty() && !overflow)
      continue;
    Json j(overflow ? nullptr : cJSON_Parse(line.c_str()));
    auto cmd = str(j.get(), "cmd");
    if (cmd == "status")
      status_console();
    else if (cmd == "light_sleep_probe") {
      request_light_sleep_probe();
      std::printf("ACCEPTED light_sleep_probe\n");
    } else if (cmd == "next" || cmd == "previous" || cmd == "prev_item" ||
             cmd == "menu" || cmd == "hold_a") {
      Key key = cmd == "next"       ? Key::Next
                : cmd == "previous" ? Key::Previous
                : cmd == "prev_item" ? Key::PreviousItem
                : cmd == "menu"     ? Key::Menu
                                    : Key::Check;
      bool ok = xQueueSend(buttons, &key, 0) == pdTRUE;
      std::printf("RESULT {\"command\":\"%s\",\"ok\":%s}\n", cmd.c_str(),
                  ok ? "true" : "false");
    } else if (cmd == "epd_probe") {
      const int mode = num(j.get(), "mode", 0);
      const std::string area = str(j.get(), "area");
      const int count = num(j.get(), "count", 2);
      const int area_code =
          area.empty() || area == "both" ? 0 : area == "partial" ? 1
          : area == "full"                   ? 2
                                              : -1;
      if (mode < 0 || mode > 4 || area_code < 0 || count < 1 || count > 5)
        std::printf("ERROR invalid epd_probe\n");
      else {
        request_display_probe(mode, area_code, count);
        std::printf("ACCEPTED epd_probe\n");
      }
    } else if (cmd == "epd_cleanup") {
      const int every = num(j.get(), "every", 20);
      if (every < 1 || every > 200)
        std::printf("ERROR invalid epd_cleanup\n");
      else {
        set_display_partial_limit(every);
        std::printf("RESULT {\"command\":\"epd_cleanup\",\"every\":%d}\n",
                    every);
      }
    } else if (cmd == "awake") {
      last_input = now_ms();
      std::printf("RESULT {\"command\":\"awake\",\"ok\":true}\n");
    } else if (cmd == "reboot")
      esp_restart();
    else if (cmd == "setup" || cmd == "wifi" || cmd == "bind" ||
             cmd == "check" || cmd == "forget" || cmd == "local_proxy" || cmd == "setup_cancel") {
      if (cmd == "check" ? request_check_update() : enqueue(cmd, line)) std::printf("ACCEPTED %s\n", cmd.c_str());
      else std::printf("RESULT {\"command\":\"%s\",\"ok\":false,\"code\":\"queue_full\"}\n", cmd.c_str());
    } else
      std::printf("ERROR invalid command\n");
    std::fill(line.begin(), line.end(), 0);
    line.clear();
    overflow = false;
  }
}
void network_task(void *) {
  while (true) {
    service_setup();
    Action *ptr = nullptr;
    if (xQueueReceive(actions, &ptr, pdMS_TO_TICKS(1000)) != pdTRUE) {
      continue;
    }
    std::unique_ptr<Action> action(ptr);
    networking = true;
    {
      // A manual UI light sleep pauses both CPUs. Wait for its return before
      // starting Wi-Fi, TLS, filesystem, or HTTP operations.
      std::lock_guard<std::mutex> awake(ui_sleep_mutex);
    }
    std::unique_lock<std::mutex> radio(network_radio_mutex);
    deadline = now_ms() + (external_cache_enabled() &&
        (action->type == "sync" || action->type == "check") ? 120000 :
        action->type == "download" ? 60000 : 20000);
    if (action->type == "check") {
      std::lock_guard<std::mutex> g(model.mutex);
      model.check_queued = false;
    }
    sync_made_progress = storage_full = false;
    if (external_cache_enabled()) {
      const auto capacity = sd_storage_capacity();
      ESP_LOGI("readocs-sd", "sync capacity available=%d total=%llu free=%llu reserve=%llu",
               capacity.available, (unsigned long long)capacity.total,
               (unsigned long long)capacity.free, (unsigned long long)sd_reserved_bytes);
    }
    http_retry_used = false;
    local_proxy_failed = false;
    error_text.clear();
    bool ok = true;
    Json input(cJSON_Parse(action->json.c_str()));
    auto c = config_copy();
    if (!setup_action_is_current(input.get())) {
      ok = false;
      error_text = "配网会话已更换，操作未执行";
    } else if (action->type == "setup")
      ok = start_setup(str(input.get(), "password"));
    else if (action->type == "setup_cancel") {
      ok = stop_setup();
      if (ok) set_status("配网热点已关闭");
    }
    else if (action->type == "wifi")
      ok = configure_wifi(input.get());
    else if (action->type == "forget") {
      auto ssid = str(input.get(), "ssid");
      c.networks.erase(
          std::remove_if(c.networks.begin(), c.networks.end(),
                         [&](const Profile &p) { return p.ssid == ssid; }),
          c.networks.end());
      ok = save_config(c);
      if (ok) {
        std::lock_guard<std::mutex> g(model.mutex);
        model.config = c;
        if (model.ssid == ssid) {
          esp_wifi_disconnect();
          model.ssid.clear();
        }
      }
      if (ok) {
        setup_result("idle", "network_forgotten", "网络已忘记");
        set_status("网络已忘记");
      } else
        ok = setup_failure("save_failed", "忘记网络时保存失败，原配置已保留");
    } else if (action->type == "bind") {
      auto server = str(input.get(), "server"), ca = str(input.get(), "ca"),
           code = str(input.get(), "code");
      if (server.rfind("https://", 0) != 0 || server.size() > 256 ||
          server.find_first_of("\r\n?#@") != std::string::npos ||
          ca.size() > 6144 || code.size() != 12) {
        ok = false;
        error_text = "代理地址或配对码格式错误";
      } else {
        while (server.back() == '/')
          server.pop_back();
        Config candidate = c;
        candidate.server = server;
        candidate.ca = ca;
        candidate.token.clear();
        candidate.local_server.clear();
        candidate.local_ca.clear();
        candidate.local_name.clear();
        ok = ensure_wifi() && ensure_time();
        if (ok) {
          Json body(cJSON_CreateObject());
          cJSON_AddStringToObject(body.get(), "code", code.c_str());
          cJSON_AddStringToObject(body.get(), "name", "PaperDIY");
          Http out{{}, {}, 4096};
          ok =
              request(candidate, "/v1/pair", out, serialize(body.get())) == 200;
          if (ok) {
            Json pair(cJSON_Parse(out.body.c_str()));
            candidate.token = str(pair.get(), "device_token");
            candidate.device_id = str(pair.get(), "device_id");
            ok = candidate.token.size() >= 32 && save_config(candidate);
            if (ok) {
              std::lock_guard<std::mutex> g(model.mutex);
              model.config = candidate;
            }
          }
        }
        if (ok)
          set_status("设备已绑定，按住 A 检查更新");
      }
    } else if (action->type == "local_proxy") {
      auto server = str(input.get(), "server"), ca = str(input.get(), "ca"), name = str(input.get(), "name");
      if (name.size() > 253 || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") != std::string::npos || (!server.empty() && (server.rfind("https://", 0) != 0 ||
           server.size() > 256 || server.find_first_of("\r\n?#@") != std::string::npos || ca.empty())) || ca.size() > 6144) {
        ok = false; error_text = "局域网代理地址或证书无效";
      } else {
        while (!server.empty() && server.back() == '/') server.pop_back();
        c.local_server = server; c.local_ca = server.empty() ? "" : ca;
        c.local_name = server.empty() ? "" : name;
        ok = save_config(c);
        if (ok) {
          std::lock_guard<std::mutex> g(model.mutex); model.config = c;
        }
      }
    } else if (action->type == "download")
      ok = download_article(input.get());
    else if (action->type == "sync" || action->type == "check")
      ok = sync(action->type == "check");
    else if (action->type == "status")
      status_console();
    if (!ok)
      set_status(error_text.empty() ? "操作未完成，旧内容已保留" : error_text);
    close_http();
    bool ap;
    {
      std::lock_guard<std::mutex> g(model.mutex);
      ap = model.ap;
    }
    if (!ap) {
      connected = false;
      esp_wifi_stop();
    }
    { std::lock_guard<std::mutex> g(model.mutex); model.last_network_ok = ok; }
    networking = false;
    bool pending;
    { std::lock_guard<std::mutex> g(model.mutex); pending = model.sync_pending; }
    // Keep progressing while actual downloads succeed. If the proxy has no
    // ready bodies, the normal 60-second/backoff schedule resumes instead.
    if (ok && external_cache_enabled() && pending && sync_made_progress && !storage_full)
      enqueue("sync");
    std::printf("RESULT {\"command\":\"%s\",\"ok\":%s}\n", action->type.c_str(),
                ok ? "true" : "false");
    std::fill(action->json.begin(), action->json.end(), 0);
  }
}
} // namespace rd
