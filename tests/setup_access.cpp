#include "setup_access.hpp"
#include <arpa/inet.h>
#include <cassert>
#include <iostream>
#include <mutex>
#include <unistd.h>
namespace rd {
struct { std::mutex mutex; bool ap = true; } model;
struct httpd_req_t { int fd; };
struct esp_netif_ip_info_t { struct { uint32_t addr; } ip; };
static int *ap_netif = nullptr;
static bool ip_failure = false;
static uint32_t ap_ip = htonl(INADDR_LOOPBACK);
constexpr int ESP_OK = 0;
#define CONFIG_LWIP_IPV6 1
template<typename... Args> static void log(Args...) {}
#define ESP_LOGW(...) log(__VA_ARGS__)
static int esp_netif_get_ip_info(int *, esp_netif_ip_info_t *info) {
  info->ip.addr = ap_ip;
  return ip_failure ? -1 : ESP_OK;
}
static int httpd_req_to_sockfd(httpd_req_t *req) { return req->fd; }
#include "setup_access_under_test.hpp"
static bool socket_case(int family) {
  int listener = socket(family, SOCK_STREAM, 0);
  assert(listener >= 0);
  if (family == AF_INET6) {
    int only = 0;
    assert(setsockopt(listener, IPPROTO_IPV6, IPV6_V6ONLY, &only, sizeof(only)) == 0);
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    assert(bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
  } else {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ap_ip;
    assert(bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
  }
  assert(listen(listener, 1) == 0);
  sockaddr_storage actual{};
  socklen_t length = sizeof(actual);
  assert(getsockname(listener, reinterpret_cast<sockaddr *>(&actual), &length) == 0);
  sockaddr_in target{};
  target.sin_family = AF_INET;
  target.sin_addr.s_addr = ap_ip;
  target.sin_port = family == AF_INET6 ? reinterpret_cast<sockaddr_in6 *>(&actual)->sin6_port
                                    : reinterpret_cast<sockaddr_in *>(&actual)->sin_port;
  int client = socket(AF_INET, SOCK_STREAM, 0);
  assert(connect(client, reinterpret_cast<sockaddr *>(&target), sizeof(target)) == 0);
  int accepted = accept(listener, nullptr, nullptr);
  assert(accepted >= 0);
  httpd_req_t req{accepted};
  bool allowed = on_ap(&req);
  if (allowed) {
    model.ap = false; assert(!on_ap(&req)); model.ap = true;
    ap_ip = htonl(0x7f000002); assert(!on_ap(&req)); ap_ip = htonl(INADDR_LOOPBACK);
    ip_failure = true; assert(!on_ap(&req)); ip_failure = false;
  }
  close(accepted); close(client); close(listener);
  return allowed;
}
}
int main() {
  using namespace rd;
  // Exercise the mapped-address failure first, without replacing getsockname.
  if (!socket_case(AF_INET6)) { std::cerr << "legal mapped IPv4 request denied\n"; return 1; }
  assert(socket_case(AF_INET));
  httpd_req_t invalid{-1}; assert(!on_ap(&invalid));
  sockaddr_storage address{};
  uint32_t value = 0;
  address.ss_family = AF_INET;
  assert(setup_ipv4(address, sizeof(sockaddr_in) - 1, value));
  assert(setup_ipv4(address, 0, value));
  address.ss_family = AF_INET6;
  assert(setup_ipv4(address, sizeof(sockaddr_in6) - 1, value));
  assert(setup_ipv4(address, sizeof(sockaddr_in6), value)); // Native IPv6 is not the AP.
  address.ss_family = AF_UNSPEC;
  assert(setup_ipv4(address, sizeof(address), value));
  std::cout << "IPv4 and real dual-stack AP access passed; invalid channels denied\n";
}
