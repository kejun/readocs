#include <algorithm>
#include <atomic>
#include <cassert>
#include <cJSON.h>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include "setup_page.hpp"
namespace rd {
#include "setup_types.hpp"
struct {
  std::mutex mutex;
  Config config;
  bool ap = false, sync_pending = false;
  std::string ap_session, ap_ssid, ap_password, status, ssid, ip;
} model;
static SetupOperation setup_operation;
static std::mutex network_radio_mutex;
static std::atomic<bool> networking{false}, connected{false}, associated{false}, redraw{false}, local_proxy_failed{false};
static int64_t clock_ms = 1000, deadline = 21000, ap_activity = 1000;
static int64_t now_ms() { return clock_ms; }
static std::string error_text;
static const char *connection_error = "connection_failed";
static Config config_copy() { return model.config; }
static void set_status(const std::string &s) { model.status = s; }
static bool save_ok = true;
static int saves = 0;
static bool save_config(const Config &) { ++saves; return save_ok; }
using esp_err_t = int;
using EventBits_t = unsigned;
constexpr int ESP_OK = 0, ESP_FAIL = -1, pdTRUE = 1, pdFALSE = 0;
constexpr unsigned GOT_IP = 1, LOST_IP = 2;
static int actions = 0, wifi_events = 0, ap_netif = 0;
static std::unique_ptr<Action> queued;
static bool queue_ok = true;
static int xQueueSend(int, Action **a, int) { if (!queue_ok) return 0; queued.reset(*a); return pdTRUE; }
#define pdMS_TO_TICKS(x) (x)
static void vTaskDelay(int ticks) { clock_ms += ticks; }
static void xEventGroupClearBits(int, unsigned) {}
static unsigned result_bits = GOT_IP;
static int result_reason = 0, reason = 0;
static bool result_associated = true;
static unsigned xEventGroupWaitBits(int, unsigned, int, int, int) {
  reason = result_reason; associated = result_associated;
  connected = (result_bits & GOT_IP) != 0;
  return result_bits;
}
constexpr int WIFI_ALL_CHANNEL_SCAN = 1, WIFI_CONNECT_AP_BY_SIGNAL = 1,
              WIFI_AUTH_OPEN = 0, WIFI_AUTH_WPA2_PSK = 1, WPA3_SAE_PWE_BOTH = 1,
              WIFI_IF_STA = 0, WIFI_IF_AP = 1, WIFI_MODE_STA = 1, WIFI_MODE_APSTA = 3,
              WIFI_REASON_AUTH_FAIL = 10, WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT = 11,
              WIFI_REASON_HANDSHAKE_TIMEOUT = 12, WIFI_REASON_NO_AP_FOUND = 13,
              ESP_MAC_WIFI_STA = 0, ESP_NETIF_DHCP_STARTED = 1;
using esp_netif_dhcp_status_t = int;
struct wifi_config_t {
  struct { uint8_t ssid[32]{}, password[64]{}; int scan_method, sort_method;
    struct { int authmode; } threshold; struct { bool capable; } pmf_cfg;
    int sae_pwe_h2e; } sta;
  struct { uint8_t ssid[32]{}, password[64]{}; int ssid_len, channel, max_connection, authmode; } ap;
};
static int start_error = 0, config_error = 0, mode_error = 0, stop_error = 0;
static int starts = 0, stops = 0;
static bool ap_up = true;
static wifi_config_t last_ap;
static int esp_wifi_start() { ++starts; return start_error; }
static int esp_wifi_stop() { ++stops; return stop_error; }
static int esp_wifi_disconnect() { connected = associated = false; return ESP_OK; }
static int esp_wifi_connect() { return ESP_OK; }
static int esp_wifi_set_mode(int) { return mode_error; }
static int esp_wifi_set_config(int iface, const wifi_config_t *c) { if (iface == WIFI_IF_AP) last_ap = *c; return config_error; }
static int esp_read_mac(uint8_t *mac, int) { std::memset(mac, 1, 6); return ESP_OK; }
static unsigned esp_random() { return 123456; }
static const char *esp_err_to_name(int) { return "test"; }
static bool esp_netif_is_netif_up(int) { return ap_up; }
struct esp_netif_ip_info_t { struct { uint32_t addr; } ip; };
static int esp_netif_get_ip_info(int, esp_netif_ip_info_t *info) { info->ip.addr = 123; return ESP_OK; }
static int esp_netif_dhcps_get_status(int, int *dhcp) { *dhcp = ESP_NETIF_DHCP_STARTED; return ESP_OK; }
using httpd_handle_t = void *;
struct ifreq { char ifr_name[16]; };
constexpr int SOL_SOCKET = 1, SO_BINDTODEVICE = 2;
static int bind_error = 0;
static int esp_netif_get_netif_impl_name(int, char *name) { std::strcpy(name, "ap1"); return ESP_OK; }
static int setsockopt(int fd, int level, int option, const void *value, size_t size) {
  assert(fd == 42 && level == SOL_SOCKET && option == SO_BINDTODEVICE && size == sizeof(ifreq));
  assert(std::string(static_cast<const ifreq *>(value)->ifr_name) == "ap1");
  return bind_error;
}
template<typename... Args> static void log(Args...) {}
#define ESP_LOGW(...) log(__VA_ARGS__)
constexpr int HTTP_GET = 0, HTTP_POST = 1;
struct httpd_req_t {
  const char *uri;
  int method, content_len;
  std::string input, session;
  bool channel = true;
  size_t cursor = 0;
  std::string response, status = "200 OK", type;
  std::map<std::string, std::string> headers;
};
static bool on_ap(httpd_req_t *req, const char **code) {
  *code = !model.ap ? "setup_closed" : !req->channel ? "setup_channel_mismatch" : nullptr;
  return !*code;
}
static int httpd_resp_set_type(httpd_req_t *r, const char *s) { r->type = s; return ESP_OK; }
static int httpd_resp_set_status(httpd_req_t *r, const char *s) { r->status = s; return ESP_OK; }
static int httpd_resp_set_hdr(httpd_req_t *r, const char *k, const char *v) { r->headers[k] = v; return ESP_OK; }
static int httpd_resp_send(httpd_req_t *r, const char *s, size_t n) { r->response.assign(s, n); return ESP_OK; }
static int httpd_resp_sendstr(httpd_req_t *r, const char *s) { r->response = s; return ESP_OK; }
static int httpd_req_get_hdr_value_str(httpd_req_t *r, const char *, char *dest, size_t n) {
  if (r->session.empty() || r->session.size() >= n) return ESP_FAIL;
  std::strcpy(dest, r->session.c_str()); return ESP_OK;
}
static int httpd_req_recv(httpd_req_t *r, char *dest, size_t n) {
  n = std::min({n, r->input.size() - r->cursor, size_t(3)});
  std::memcpy(dest, r->input.data() + r->cursor, n); r->cursor += n; return n;
}
struct wifi_scan_config_t { bool show_hidden; };
struct wifi_ap_record_t { uint8_t ssid[33]; int authmode; };
static int scans = 0;
static int esp_wifi_scan_start(wifi_scan_config_t *, bool) { ++scans; return ESP_OK; }
static int esp_wifi_scan_get_ap_records(uint16_t *n, wifi_ap_record_t *) { *n = 0; return ESP_OK; }
#include "setup_under_test.hpp"
static httpd_req_t request(const std::string &path, int method = HTTP_GET,
                          const std::string &body = "", const std::string &session = "") {
  httpd_req_t r{path.c_str(), method, static_cast<int>(body.size()), body, session, true, 0, "", "200 OK", "", {}};
  assert(web(&r) == ESP_OK); return r;
}
static std::string code(const httpd_req_t &r) { Json j(cJSON_Parse(r.response.c_str())); return str(j.get(), "code"); }
static void reset() {
  model.config = {}; model.config.networks = {{"old", "old-password"}};
  model.config.token = "binding-preserved"; model.ap = true; model.ap_session = std::string(32, 'a');
  setup_operation = {}; queued.reset(); queue_ok = save_ok = true;
  networking = connected = associated = false;
  clock_ms = ap_activity = 1000; deadline = 21000;
  scans = starts = stops = saves = start_error = config_error = mode_error = stop_error = 0;
  result_bits = GOT_IP; result_reason = 0; result_associated = ap_up = true;
}
static bool run_wifi() {
  assert(queued && queued->type == "wifi"); Json input(cJSON_Parse(queued->json.c_str()));
  assert(setup_action_is_current(input.get())); networking = true;
  bool ok = configure_wifi(input.get()); networking = false; queued.reset(); return ok;
}
}
int main() {
  using namespace rd;
  assert(setup_socket_open(nullptr, 42) == ESP_OK);
  bind_error = -1; assert(setup_socket_open(nullptr, 42) == ESP_FAIL); bind_error = 0;
  reset(); assert(request("/?source=setup").response.find("<title>Readocs") != std::string::npos && scans == 0);
  clock_ms = 2000; auto activity = ap_activity;
  auto status = request("/api/status?poll=1"); assert(status.status == "200 OK" && status.type.find("application/json") == 0);
  assert(ap_activity == activity && scans == 0);
  assert(request("/api/typo").status == "404 Not Found" && scans == 0);
  assert(request("/api/wifi").status == "405 Method Not Allowed");
  assert(request("/api/status", HTTP_POST, "{}").status == "405 Method Not Allowed");
  const std::string data = "{\"ssid\":\"hidden\",\"password\":\"new-password\"}";
  auto token = model.ap_session;
  assert(code(request("/api/wifi", HTTP_POST, data)) == "invalid_session");
  assert(code(request("/api/wifi", HTTP_POST, data, "old-session")) == "expired_session");
  httpd_req_t spoof{"/api/wifi", HTTP_POST, static_cast<int>(data.size()), data, token, false, 0, "", "200 OK", "", {}};
  spoof.headers["Host"] = "192.168.4.1"; spoof.headers["X-Forwarded-For"] = "192.168.4.2";
  assert(web(&spoof) == ESP_OK && code(spoof) == "setup_channel_mismatch" && !queued);
  queue_ok = false; assert(code(request("/api/wifi", HTTP_POST, data, token)) == "queue_full" && !setup_operation.pending);
  queue_ok = true; assert(request("/api/wifi?submit=1", HTTP_POST, data, token).status == "200 OK" && setup_operation.pending);
  assert(code(request("/api/wifi", HTTP_POST, data, token)) == "operation_busy");
  assert(code(request("/api/scan")) == "network_busy" && scans == 0);
  result_bits = LOST_IP; result_reason = WIFI_REASON_AUTH_FAIL;
  assert(!run_wifi() && setup_operation.code == "auth_failed" && model.config.networks[0].ssid == "old" && saves == 0);
  assert(model.ap && !setup_operation.pending);
  request("/api/wifi", HTTP_POST, data, token); result_bits = LOST_IP; result_reason = WIFI_REASON_NO_AP_FOUND;
  assert(!run_wifi() && setup_operation.code == "network_not_found" && saves == 0);
  request("/api/wifi", HTTP_POST, data, token); result_bits = 0; result_reason = 0; result_associated = true;
  assert(!run_wifi() && setup_operation.code == "dhcp_timeout" && saves == 0);
  request("/api/wifi", HTTP_POST, data, token); result_bits = GOT_IP; save_ok = false;
  assert(!run_wifi() && setup_operation.code == "save_failed" && model.config.networks.size() == 1 && model.ap);
  request("/api/wifi", HTTP_POST, data, token); save_ok = true;
  assert(run_wifi() && setup_operation.state == "success" && model.ap && setup_operation.close_at > clock_ms);
  assert(model.config.networks.size() == 2 && model.config.networks[0].ssid == "hidden" && model.config.token == "binding-preserved");
  assert(request("/api/complete", HTTP_POST, "{}", token).status == "200 OK");
  clock_ms += 1001; service_setup(); assert(!model.ap && !connected && stops == 1 && model.ap_session.empty());
  reset(); token = model.ap_session; activity = ap_activity;
  for (int i = 0; i < 301; ++i) { clock_ms += 1000; request("/api/status"); }
  assert(ap_activity == activity); service_setup(); assert(!model.ap && stops == 1);
  reset(); token = model.ap_session; request("/api/wifi", HTTP_POST, data, token);
  Json stale(cJSON_Parse(queued->json.c_str())); model.ap_session = std::string(32, 'b');
  assert(!setup_action_is_current(stale.get()));
  reset(); token = model.ap_session;
  request("/api/wifi", HTTP_POST,
          "{\"ssid\":\"hidden\",\"password\":\"password\",\"_setup_session\":null,\"_setup_session\":\"forged\",\"_setup_operation\":99}", token);
  Json normalized(cJSON_Parse(queued->json.c_str()));
  assert(setup_action_is_current(normalized.get()) && str(normalized.get(), "_setup_session") == token);
  Json invalid_marker(cJSON_Parse("{\"_setup_session\":null}"));
  assert(!setup_action_is_current(invalid_marker.get()));
  reset(); model.config.networks.clear();
  for (int i = 0; i < 5; ++i) model.config.networks.push_back({"saved" + std::to_string(i), "password"});
  token = model.ap_session; request("/api/wifi", HTTP_POST, data, token);
  assert(!run_wifi() && setup_operation.code == "replacement_required" && starts == 0 && model.config.networks.size() == 5);
  request("/api/wifi", HTTP_POST, "{\"ssid\":\"new\",\"password\":\"password\",\"replace\":\"saved2\"}", token);
  assert(run_wifi() && model.config.networks.size() == 5 && model.config.networks[0].ssid == "new");
  assert(std::none_of(model.config.networks.begin(), model.config.networks.end(), [](const auto &p) { return p.ssid == "saved2"; }));
  reset(); mode_error = -1; assert(!start_setup("") && !model.ap);
  reset(); config_error = -1; assert(!start_setup("") && !model.ap);
  reset(); start_error = -1; assert(!start_setup("") && !model.ap);
  reset(); ap_up = false; assert(!start_setup("") && !model.ap);
  reset(); assert(start_setup("test-12-byte") && model.ap && std::strlen(reinterpret_cast<char *>(last_ap.ap.password)) == 12);
  assert(model.ap_password == "test-12-byte");
  assert(!start_setup("short"));
}
