#pragma once
#include <cstddef>
#include <cstdint>

using esp_err_t = int;
using nvs_handle_t = uint32_t;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_NVS_NOT_FOUND = 1;
constexpr esp_err_t ESP_ERR_NVS_VALUE_TOO_LONG = 2;
constexpr esp_err_t ESP_ERR_NVS_TYPE_MISMATCH = 3;
esp_err_t nvs_get_str(nvs_handle_t, const char *, char *, size_t *);
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
