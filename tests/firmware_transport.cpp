#include <algorithm>
#include <atomic>
#include <cassert>
#include <cJSON.h>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>
namespace rd {
#include "transport_types.hpp"
struct Config {
  std::string server = "https://public", ca = "public-ca", token = "device-token";
  std::string local_server = "https://local", local_ca = "local-ca", local_name = "localhost";
};
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NOT_FOUND = 1,
              ESP_ERR_TIMEOUT = 2, ESP_ERR_NO_MEM = 3;
constexpr int HTTP_METHOD_GET = 0, HTTP_METHOD_POST = 1;
struct Client {
  std::string url, post;
  std::map<std::string, std::string> headers;
  void *data = nullptr;
  int method = HTTP_METHOD_GET;
};
using esp_http_client_handle_t = Client *;
static void http_event() {}
static void esp_crt_bundle_attach() {}
struct esp_http_client_config_t {
  const char *url = nullptr, *user_agent = nullptr, *common_name = nullptr, *cert_pem = nullptr;
  int timeout_ms = 0;
  void (*event_handler)() = nullptr;
  void (*crt_bundle_attach)() = nullptr;
  bool disable_auto_redirect = false, keep_alive_enable = false;
};
static int64_t deadline = 120000, clock_ms = 0;
static std::string error_text, http_server, http_ca, http_name;
static bool http_retry_used = false, overflow = false;
static std::atomic<bool> local_proxy_failed{false};
static esp_http_client_handle_t http_client = nullptr;
static std::vector<int> outcomes;
static std::vector<std::string> urls;
static int attempts = 0, opened = 0;
static int64_t now_ms() { return clock_ms; }
static bool external_cache_enabled() { return true; }
static void log_http_result(int, int, size_t, int64_t) {}
static Client *esp_http_client_init(const esp_http_client_config_t *c) {
  assert(c->disable_auto_redirect && c->keep_alive_enable && c->event_handler);
  if (std::string(c->url).rfind("https://local", 0) == 0) {
    assert(std::string(c->common_name) == "localhost" && std::string(c->cert_pem) == "local-ca");
  } else {
    assert(!c->common_name && std::string(c->cert_pem) == "public-ca");
  }
  ++opened;
  auto *client = new Client;
  client->url = c->url;
  return client;
}
static int esp_http_client_set_user_data(Client *c, void *data) { c->data = data; return ESP_OK; }
static int esp_http_client_cleanup(Client *c) { delete c; return ESP_OK; }
static int esp_http_client_set_header(Client *c, const char *key, const char *value) {
  if (value) c->headers[key] = value;
  else c->headers.erase(key);
  return ESP_OK;
}
static int esp_http_client_set_post_field(Client *c, const char *body, int size) {
  c->post = body ? std::string(body, size) : "";
  return ESP_OK;
}
static int esp_http_client_set_url(Client *c, const char *url) { c->url = url; return ESP_OK; }
static int esp_http_client_set_timeout_ms(Client *, int timeout) { assert(timeout > 0); return ESP_OK; }
static int esp_http_client_set_method(Client *c, int method) { c->method = method; return ESP_OK; }
static int esp_http_client_get_status_code(Client *c) {
  return c->method == HTTP_METHOD_POST ? 202 : 200;
}
static int esp_http_client_perform(Client *c) {
  assert(c->headers.at("Authorization") == "Bearer device-token");
  assert(c->headers.at("X-Protocol-Version") == "1");
  urls.push_back(c->url);
  const int result = outcomes.at(attempts++);
  auto *out = static_cast<Http *>(c->data);
  out->body = result == ESP_OK ? "complete" : "partial";
  out->etag = result == ESP_OK ? "new-etag" : "partial-etag";
  out->overflow = overflow;
  ++clock_ms;
  return result;
}
#include "transport_under_test.hpp"
static void reset(std::vector<int> results) {
  close_http();
  outcomes = std::move(results);
  urls.clear();
  attempts = opened = 0;
  clock_ms = 0;
  deadline = 120000;
  http_retry_used = overflow = false;
  local_proxy_failed = false;
}
}
int main() {
  using namespace rd;
  Config c;
  reset({ESP_FAIL, ESP_OK});
  Http first{{}, {}, 1024};
  assert(request(c, "/v1/sync", first) == 200);
  assert(attempts == 2 && opened == 2 && !local_proxy_failed);
  assert(urls == std::vector<std::string>({"https://local/v1/sync", "https://local/v1/sync"}));
  assert(first.body == "complete" && first.etag == "new-etag");

  reset({ESP_FAIL, ESP_FAIL, ESP_OK});
  Http second{{}, {}, 1024};
  assert(request(c, "/v1/sync", second) == 200 && local_proxy_failed);
  assert(urls == std::vector<std::string>({"https://local/v1/sync", "https://local/v1/sync", "https://public/v1/sync"}));
  assert(opened == 3 && second.body == "complete");

  reset({ESP_FAIL});
  Http post{{}, {}, 1024};
  assert(request(c, "/v1/refresh", post, "{}") == -1);
  assert(attempts == 1 && !local_proxy_failed); // A POST is never replayed.

  reset({ESP_FAIL, ESP_FAIL, ESP_OK, ESP_OK});
  Http probe{{}, {}, 1024}, refresh{{}, {}, 1024};
  assert(request(c, "/v1/status", probe) == 200 && local_proxy_failed);
  assert(request(c, "/v1/refresh", refresh, "{}") == 202);
  assert(urls == std::vector<std::string>({"https://local/v1/status", "https://local/v1/status", "https://public/v1/status", "https://public/v1/refresh"}));

  reset({ESP_OK, ESP_FAIL});
  Http ready{{}, {}, 1024}, lost{{}, {}, 1024};
  assert(request(c, "/v1/status", ready) == 200);
  assert(request(c, "/v1/refresh", lost, "{}") == -1);
  assert(attempts == 2 && !local_proxy_failed);
  assert(urls == std::vector<std::string>({"https://local/v1/status", "https://local/v1/refresh"}));

  reset({ESP_FAIL});
  overflow = true;
  Http too_large{{}, {}, 1};
  assert(request(c, "/v1/sync", too_large) == -1 && attempts == 1);
  close_http();
}
