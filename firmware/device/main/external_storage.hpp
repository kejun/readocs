#pragma once
#include <cstdint>
#include <string>
namespace rd {
struct StorageCapacity {
  bool available = false;
  uint64_t total = 0, free = 0;
};
inline constexpr uint64_t sd_reserved_bytes = 8ULL * 1024 * 1024;
inline bool sd_can_fit(const StorageCapacity &space, uint64_t bytes) {
  return space.available && space.free >= sd_reserved_bytes &&
         bytes <= (space.free - sd_reserved_bytes) / 2;
}
inline bool sd_can_publish(const StorageCapacity &space, uint64_t bytes) {
  return space.available && bytes <= space.free / 2;
}
bool mount_sd_storage();
const std::string &sd_cache_directory();
StorageCapacity sd_storage_capacity();
bool external_cache_enabled();
const std::string &cache_directory();
bool cache_body_verified(const std::string &id);
bool cache_has_space(uint64_t bytes);
} // namespace rd
