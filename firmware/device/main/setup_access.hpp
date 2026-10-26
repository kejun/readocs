#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>

namespace rd {
// getsockname may return an IPv4-mapped IPv6 address on IDF's dual-stack server.
inline const char *setup_ipv4(const sockaddr_storage &address, socklen_t length,
                              uint32_t &ipv4) {
  if (length < offsetof(sockaddr, sa_family) + sizeof(sa_family_t) || length > sizeof(address))
    return "address_length";
  if (address.ss_family == AF_INET) {
    if (length < sizeof(sockaddr_in)) return "address_length";
    ipv4 = reinterpret_cast<const sockaddr_in *>(&address)->sin_addr.s_addr;
    return nullptr;
  }
  if (address.ss_family == AF_INET6) {
    if (length < sizeof(sockaddr_in6)) return "address_length";
    const auto *bytes = reinterpret_cast<const sockaddr_in6 *>(&address)->sin6_addr.s6_addr;
    const uint8_t prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 255};
    if (std::memcmp(bytes, prefix, sizeof(prefix))) return "native_ipv6";
    std::memcpy(&ipv4, bytes + 12, sizeof(ipv4));
    return nullptr;
  }
  return "address_family";
}
} // namespace rd
