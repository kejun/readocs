#include "cache_merge.hpp"
#include "cache_policy.hpp"
#include "readocs.hpp"
#include "source_metadata.hpp"
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
#include <utility>

namespace rd {
static esp_netif_t *sta_netif, *ap_netif;
static EventGroupHandle_t wifi_events;
static std::atomic<int> reason{0};
static constexpr EventBits_t GOT_IP = BIT0, LOST_IP = BIT1;
static int64_t deadline = 0;
static std::string error_text = "";
int64_t now_ms() { return esp_timer_get_time() / 1000; }
void set_status(const std::string &s, bool visible) {
  {
    std::lock_guard<std::mutex> g(model.mutex);
    model.status = s;
  }
  if (visible)
    redraw = true;
}
void enqueue(const std::string &type, const std::string &json) {
  auto *a = new Action{type, json};
  if (xQueueSend(actions, &a, 0) != pdTRUE)
    delete a;
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
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    connected = false;
    reason = static_cast<wifi_event_sta_disconnected_t *>(data)->reason;
    xEventGroupSetBits(wifi_events, LOST_IP);
  }
}
static bool connect_profile(const Profile &p) {
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
  if (err != ESP_OK || esp_wifi_connect() != ESP_OK)
    return false;
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
  return false;
}
static bool ensure_wifi() {
  auto c = config_copy();
  wifi_mode_t mode;
  esp_wifi_get_mode(&mode);
  if (mode == WIFI_MODE_NULL)
    esp_wifi_set_mode(WIFI_MODE_STA);
  esp_wifi_start();
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
static std::string http_server, http_ca;
static bool http_retry_used = false;
static void close_http() {
  if (http_client) {
    esp_http_client_set_user_data(http_client, nullptr);
    esp_http_client_cleanup(http_client);
    http_client = nullptr;
  }
  http_server.clear();
  http_ca.clear();
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
                   const std::string &body = "", const std::string &etag = "") {
  const auto started = now_ms();
  if (now_ms() >= deadline) {
    error_text = "联网超时，已保留离线内容";
    log_http_result(-1, ESP_ERR_TIMEOUT, 0, started);
    return -1;
  }
  const std::string url = c.server + path;
  if (http_client && (http_server != c.server || http_ca != c.ca))
    close_http();
  if (!http_client) {
    http_server = c.server;
    http_ca = c.ca;
    esp_http_client_config_t h{};
    h.url = url.c_str();
    h.timeout_ms = std::clamp<int64_t>(deadline - now_ms(), 1, 5000);
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
          client, std::clamp<int64_t>(deadline - now_ms(), 1, 5000)) ==
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
  if (sha256(read_file(id)) == id)
    return true;
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
                         str(item, "title"), str(item, "snapshot_id")});
  auto compact = entries;
  trim_cached_articles(compact, source, document);
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
  if (cJSON_IsNull(ref))
    return true; // Discovery precedes the independent upstream body job.
  if (!cJSON_IsObject(ref))
    return body_failure("reference_json", "正文引用返回无效，旧内容已保留");
  std::string id;
  if (!download(c, ref, id))
    return false;
  entry.snapshot_id = id;
  return true;
}
static bool sync(bool manual) {
  auto c = config_copy();
  if (c.server.empty() || c.token.empty()) {
    error_text = "在管理页生成配对码，通过 USB 绑定设备";
    return false;
  }
  if (!ensure_wifi() || !ensure_time())
    return false;
  std::string etag;
  std::vector<Entry> old;
  std::vector<Source> old_sources;
  {
    std::lock_guard<std::mutex> g(model.mutex);
    etag = model.etag;
    old = model.entries;
    old_sources = model.sources;
  }
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
          clear_directory_progress, [] { collect_cache(true); })) {
    error_text = "缓存空间不足，旧版本已保留";
    return false;
  }
  compact_directory_progress(selected.first, selected.second);
  Http out{{}, {}, 8192};
  int status = request(c, "/v1/sync", out, "", manual ? "" : etag);
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
    std::vector<std::string> unchanged_snapshots;
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
                             str(item, "title"), str(item, "snapshot_id")});
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
        set_status("目录同步中，下次检查接续");
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
        Entry e{sid, str(item, "document_id"), str(item, "title"), ""};
        if (!safe_id(e.document_id) || e.title.size() > 1024)
          return false;
        auto prior = std::find_if(old.begin(), old.end(), [&](const Entry &p) {
          return p.source_id == sid && p.document_id == e.document_id;
        });
        if (prior != old.end()) {
          e.snapshot_id = prior->snapshot_id;
          if (!e.snapshot_id.empty() &&
              e.snapshot_id == str(item, "snapshot_id"))
            unchanged_snapshots.push_back(e.snapshot_id);
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
    const auto keep = cached_article_mask(entries, current.first, current.second);
    prepare_download_space();
    for (size_t i = 0; i < entries.size(); ++i) {
      auto &e = entries[i];
      if (!keep[i]) {
        e.snapshot_id.clear();
        continue;
      }
      if (std::find(unchanged_snapshots.begin(), unchanged_snapshots.end(),
                    e.snapshot_id) != unchanged_snapshots.end() &&
          valid_hash(e.snapshot_id) &&
          sha256(read_file(e.snapshot_id)) == e.snapshot_id)
        continue;
      if (now_ms() + 1000 >= deadline || !fetch_entry(c, e))
        complete = false;
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
      model.sync_pending = !complete;
    }
    collect_cache(); // Release versions older than the committed fallback.
    redraw = true;
  } else {
    // The selected article may have changed since the last manifest was
    // committed. Check it even on 304, including articles outside the prefetch
    // set; a nonempty local reference must not make an old body permanent.
    const auto current = current_article();
    auto entries = old;
    trim_cached_articles(entries, current.first, current.second);
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
  if (manual) {
    Http refresh{{}, {}, 2048};
    int s = request(c, "/v1/refresh", refresh, "{}");
    if (s == 202)
      set_status("更新处理中，下次检查时接续");
    else if (s == 429) {
      bool pending;
      {
        std::lock_guard<std::mutex> g(model.mutex);
        pending = model.sync_pending;
      }
      set_status(pending ? "正文下载中，下次检查接续" : "已同步", pending);
    } else
      return false;
  } else {
    bool pending;
    {
      std::lock_guard<std::mutex> g(model.mutex);
      pending = model.sync_pending;
    }
    set_status(pending ? "正文下载中，下次检查接续" : "已同步", pending);
  }
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
  trim_cached_articles(entries, current.first, current.second, source, doc);
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

void stop_setup() {
  {
    std::lock_guard<std::mutex> g(model.mutex);
    model.ap = false;
    model.ap_session.clear();
  }
  esp_wifi_set_mode(WIFI_MODE_STA);
  redraw = true;
}
void start_setup() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
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
  ap.ap.ssid_len = std::strlen(ssid);
  ap.ap.channel = 1;
  ap.ap.max_connection = 2;
  ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
  esp_wifi_set_mode(WIFI_MODE_APSTA);
  esp_wifi_set_config(WIFI_IF_AP, &ap);
  esp_wifi_start();
  {
    std::lock_guard<std::mutex> g(model.mutex);
    model.ap_ssid = ssid;
    model.ap_password = pass;
    model.ap_session = session;
    model.ap = true;
  }
  ap_activity = now_ms();
  redraw = true;
}
static const char PAGE[] =
    R"HTML(<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Readocs · Wi-Fi</title><style>*{box-sizing:border-box}body{font:16px/1.6 -apple-system,BlinkMacSystemFont,"PingFang SC",sans-serif;color:#1c252c;background:#f7f8fa;max-width:440px;padding:32px 24px;margin:20px auto}h1{font-size:28px;margin:40px 0 30px}label{display:block;font-size:14px;margin-top:22px}input,button{font:inherit;width:100%;padding:12px;border:1px solid #d3dde1;border-radius:7px;background:white;margin-top:8px}button{cursor:pointer}button[type=submit]{background:#17766b;color:white;border:0;margin-top:30px}#networks button{text-align:left;background:none;border:0;border-bottom:1px solid #e0e6e9;border-radius:0}details{font-size:13px;margin-top:28px;color:#69777e}#result{font-size:14px}header{font-weight:650;font-size:22px}</style><header>readocs</header><h1>连接 Wi-Fi</h1><div id="networks"></div><button id="scan">扫描网络</button><form id="form"><label for="ssid">Wi-Fi 名称</label><input id="ssid" maxlength="32" required><label for="password">密码</label><input id="password" type="password" maxlength="63" autocomplete="new-password"><div id="replace"></div><button type="submit">连接</button><p id="result" role="status"></p></form><details><summary>已保存的网络</summary><div id="saved"></div></details><details><summary>网络要求</summary><p>使用 2.4 GHz 个人 Wi-Fi 或手机热点。换网络后，阅读内容和飞书配置会保留。</p></details><script>let session='',profiles=[];const $=s=>document.querySelector(s);async function call(path,data){const r=await fetch(path,{method:data?'POST':'GET',headers:data?{'Content-Type':'application/json','X-Setup-Session':session}:{},body:data?JSON.stringify(data):undefined});const j=await r.json();if(!r.ok)throw Error(j.message||'操作失败');return j}async function load(){let s=await call('/api/status');session=s.session;profiles=s.networks;$('#saved').replaceChildren();profiles.forEach(n=>{let b=document.createElement('button');b.type='button';b.textContent=n; b.onclick=async()=>{if(confirm('忘记 '+n+'？')){try{await call('/api/forget',{ssid:n});await load()}catch(e){$('#result').textContent=e.message}}};$('#saved').append(b)})}$('#scan').onclick=async()=>{try{$('#scan').disabled=true;const list=await call('/api/scan');$('#networks').replaceChildren();list.networks.forEach(n=>{let b=document.createElement('button');b.type='button';b.textContent=n.ssid+(n.secure?' · 加密':'');b.onclick=()=>{$('#ssid').value=n.ssid;$('#password').focus()};$('#networks').append(b)})}catch(e){$('#result').textContent=e.message}finally{$('#scan').disabled=false}};$('#form').onsubmit=async e=>{e.preventDefault();let data={ssid:$('#ssid').value,password:$('#password').value};if(profiles.length>=5&&!profiles.includes(data.ssid)){let choice=prompt('已保存 5 个网络，请输入要替换的网络名称：\n'+profiles.join('\n'));if(!profiles.includes(choice))return;data.replace=choice}try{await call('/api/wifi',data);$('#password').value='';$('#result').textContent='正在连接，请查看设备屏幕。连接成功后热点会关闭。'}catch(err){$('#result').textContent=err.message}};load().catch(e=>$('#result').textContent=e.message);</script></html>)HTML";
static bool on_ap(httpd_req_t *req) {
  sockaddr_in local{};
  socklen_t len = sizeof(local);
  esp_netif_ip_info_t info{};
  esp_netif_get_ip_info(ap_netif, &info);
  std::lock_guard<std::mutex> g(model.mutex);
  return model.ap &&
         getsockname(httpd_req_to_sockfd(req),
                     reinterpret_cast<sockaddr *>(&local), &len) == 0 &&
         local.sin_addr.s_addr == info.ip.addr;
}
static esp_err_t send_json(httpd_req_t *req, cJSON *j) {
  auto s = serialize(j);
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_send(req, s.data(), s.size());
}
static esp_err_t web(httpd_req_t *req) {
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
  if (!on_ap(req))
    return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
                               "Use the device setup hotspot");
  ap_activity = now_ms();
  if (std::strcmp(req->uri, "/") == 0) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, PAGE);
  }
  if (req->method == HTTP_GET) {
    Json j(cJSON_CreateObject());
    if (std::strcmp(req->uri, "/api/status") == 0) {
      std::lock_guard<std::mutex> g(model.mutex);
      cJSON_AddStringToObject(j.get(), "session", model.ap_session.c_str());
      cJSON_AddStringToObject(j.get(), "status", model.status.c_str());
      auto *a = cJSON_AddArrayToObject(j.get(), "networks");
      for (const auto &p : model.config.networks)
        cJSON_AddItemToArray(a, cJSON_CreateString(p.ssid.c_str()));
    } else {
      wifi_scan_config_t scan{};
      scan.show_hidden = true;
      if (networking || esp_wifi_scan_start(&scan, true) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Network busy");
      wifi_ap_record_t found[20]{};
      uint16_t count = 20;
      esp_wifi_scan_get_ap_records(&count, found);
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
    return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Invalid session");
  {
    std::lock_guard<std::mutex> g(model.mutex);
    if (model.ap_session != token)
      return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Expired session");
  }
  if (req->content_len <= 0 || req->content_len > 512)
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid size");
  std::string body(req->content_len, '\0');
  size_t used = 0;
  while (used < body.size()) {
    int n = httpd_req_recv(req, body.data() + used, body.size() - used);
    if (n <= 0)
      return ESP_FAIL;
    used += n;
  }
  Json j(cJSON_Parse(body.c_str()));
  auto ssid = str(j.get(), "ssid"), pass = str(j.get(), "password");
  if (ssid.empty() || ssid.size() > 32 ||
      ssid.find('\0') != std::string::npos ||
      (!pass.empty() && (pass.size() < 8 || pass.size() > 63)))
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "Invalid Wi-Fi data");
  enqueue(std::strcmp(req->uri, "/api/forget") == 0 ? "forget" : "wifi", body);
  std::fill(body.begin(), body.end(), 0);
  Json ok(cJSON_CreateObject());
  cJSON_AddBoolToObject(ok.get(), "accepted", true);
  return send_json(req, ok.get());
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
      WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             wifi_event, nullptr));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
  httpd_handle_t server;
  httpd_config_t sc = HTTPD_DEFAULT_CONFIG();
  sc.stack_size = 8192;
  sc.max_uri_handlers = 6;
  ESP_ERROR_CHECK(httpd_start(&server, &sc));
  for (const char *uri :
       {"/", "/api/status", "/api/scan", "/api/wifi", "/api/forget"}) {
    httpd_uri_t h{};
    h.uri = uri;
    h.method = std::strcmp(uri, "/api/wifi") == 0 ||
                       std::strcmp(uri, "/api/forget") == 0
                   ? HTTP_POST
                   : HTTP_GET;
    h.handler = web;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &h));
  }
}
static void status_console() {
  Json j(cJSON_CreateObject());
  cJSON_AddStringToObject(j.get(), "firmware", "readocs-0.2.3");
  cJSON_AddBoolToObject(j.get(), "connected", connected.load());
  cJSON_AddBoolToObject(j.get(), "busy", networking.load());
  {
    std::lock_guard<std::mutex> g(model.mutex);
    cJSON_AddStringToObject(j.get(), "status", model.status.c_str());
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
    else if (cmd == "next" || cmd == "previous" || cmd == "prev_item" ||
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
             cmd == "check" || cmd == "forget") {
      enqueue(cmd, line);
      std::printf("ACCEPTED %s\n", cmd.c_str());
    } else
      std::printf("ERROR invalid command\n");
    std::fill(line.begin(), line.end(), 0);
    line.clear();
    overflow = false;
  }
}
void network_task(void *) {
  while (true) {
    Action *ptr = nullptr;
    if (xQueueReceive(actions, &ptr, pdMS_TO_TICKS(1000)) != pdTRUE) {
      bool ap;
      {
        std::lock_guard<std::mutex> g(model.mutex);
        ap = model.ap;
      }
      if (ap && now_ms() - ap_activity > 300000)
        stop_setup();
      continue;
    }
    std::unique_ptr<Action> action(ptr);
    networking = true;
    deadline = now_ms() + (action->type == "download" ? 60000 : 20000);
    http_retry_used = false;
    error_text.clear();
    bool ok = true;
    Json input(cJSON_Parse(action->json.c_str()));
    auto c = config_copy();
    if (action->type == "setup")
      start_setup();
    else if (action->type == "wifi") {
      Profile p{str(input.get(), "ssid"), str(input.get(), "password")};
      auto it =
          std::find_if(c.networks.begin(), c.networks.end(),
                       [&](const Profile &old) { return old.ssid == p.ssid; });
      if (p.ssid.empty() || p.ssid.size() > 32 ||
          (!p.password.empty() &&
           (p.password.size() < 8 || p.password.size() > 63))) {
        ok = false;
        error_text = "Wi-Fi 名称或密码格式错误";
      } else if (it == c.networks.end() && c.networks.size() >= 5) {
        auto replace = str(input.get(), "replace");
        auto old =
            std::find_if(c.networks.begin(), c.networks.end(),
                         [&](const Profile &n) { return n.ssid == replace; });
        if (old == c.networks.end()) {
          ok = false;
          error_text = "请选择要替换的已保存网络";
        } else
          c.networks.erase(old);
      }
      if (ok) {
        set_status("正在连接 Wi-Fi");
        esp_wifi_start();
        ok = connect_profile(p);
        if (ok) {
          c.networks.erase(std::remove_if(c.networks.begin(), c.networks.end(),
                                          [&](const Profile &n) {
                                            return n.ssid == p.ssid;
                                          }),
                           c.networks.end());
          c.networks.insert(c.networks.begin(), p);
          ok = save_config(c);
          if (ok) {
            {
              std::lock_guard<std::mutex> g(model.mutex);
              model.config = c;
            }
            stop_setup();
            set_status("Wi-Fi 已连接");
          }
        } else
          error_text = reason == WIFI_REASON_AUTH_FAIL ||
                               reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
                           ? "密码错误，请重试"
                           : "连接失败，原网络配置已保留";
      }
    } else if (action->type == "forget") {
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
      set_status("网络已忘记");
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
    networking = false;
    std::printf("RESULT {\"command\":\"%s\",\"ok\":%s}\n", action->type.c_str(),
                ok ? "true" : "false");
    std::fill(action->json.begin(), action->json.end(), 0);
  }
}
} // namespace rd
