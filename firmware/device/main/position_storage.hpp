#pragma once
#include <nvs.h>
#include <string>

namespace rd {
inline constexpr size_t positions_limit = 128 * 1024;

inline std::string read_position_map(nvs_handle_t handle) {
  size_t size = 0;
  const auto err = nvs_get_blob(handle, "positions_v2", nullptr, &size);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    // Keep the old string readable until the first successful blob save.
    if (nvs_get_str(handle, "positions", nullptr, &size) != ESP_OK ||
        size == 0 || size > positions_limit)
      return "";
    std::string out(size, '\0');
    if (nvs_get_str(handle, "positions", out.data(), &size) != ESP_OK)
      return "";
    out.pop_back();
    return out;
  }
  if (err != ESP_OK || size > positions_limit)
    return "";
  std::string out(size, '\0');
  return nvs_get_blob(handle, "positions_v2", out.data(), &size) == ESP_OK
             ? out
             : "";
}

inline esp_err_t write_position_map(nvs_handle_t handle,
                                    const std::string &body) {
  if (body.size() > positions_limit)
    return ESP_ERR_NVS_VALUE_TOO_LONG;
  // NVS strings stop at 4000 bytes; blobs span pages within the partition.
  return nvs_set_blob(handle, "positions_v2", body.data(), body.size());
}
} // namespace rd
