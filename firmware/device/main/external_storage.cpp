#include "external_storage.hpp"
#include <M5Unified.h>
#include <driver/sdspi_host.h>
#include <driver/spi_master.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <cerrno>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>
namespace rd {
static sdmmc_card_t *card = nullptr;
static std::string directory;
const std::string &sd_cache_directory() { return directory; }
StorageCapacity sd_storage_capacity() {
  StorageCapacity out;
  out.available = card && esp_vfs_fat_info("/sd", &out.total, &out.free) == ESP_OK;
  return out;
}
static bool make_directory(const std::string &path) {
  if (mkdir(path.c_str(), 0755) == 0) return true;
  struct stat st{};
  return errno == EEXIST && stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
bool mount_sd_storage() {
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.max_freq_khz = 10000;
  spi_bus_config_t bus{};
  bus.mosi_io_num = M5.getPin(m5::pin_name_t::sd_spi_mosi);
  bus.miso_io_num = M5.getPin(m5::pin_name_t::sd_spi_miso);
  bus.sclk_io_num = M5.getPin(m5::pin_name_t::sd_spi_sclk);
  bus.quadwp_io_num = bus.quadhd_io_num = -1;
  bus.max_transfer_sz = 4096;
  if (bus.mosi_io_num < 0 || bus.miso_io_num < 0 || bus.sclk_io_num < 0)
    return false;
  auto err = spi_bus_initialize(static_cast<spi_host_device_t>(host.slot), &bus, SDSPI_DEFAULT_DMA);
  if (err != ESP_OK) return false;
  sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot.gpio_cs = static_cast<gpio_num_t>(M5.getPin(m5::pin_name_t::sd_spi_cs));
  slot.host_id = static_cast<spi_host_device_t>(host.slot);
  esp_vfs_fat_sdmmc_mount_config_t cfg{};
  cfg.format_if_mount_failed = false; // Never format a user's expansion card.
  cfg.max_files = 8;
  err = esp_vfs_fat_sdspi_mount("/sd", &host, &slot, &cfg, &card);
  if (err != ESP_OK) {
    spi_bus_free(static_cast<spi_host_device_t>(host.slot));
    ESP_LOGW("readocs-sd", "SD unavailable: %s; keeping internal cache", esp_err_to_name(err));
    return false;
  }
  uint8_t mac[6]{};
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) return false;
  char identity[13];
  std::snprintf(identity, sizeof(identity), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  directory = std::string("/sd/readocs/") + identity;
  if (!make_directory("/sd/readocs") || !make_directory(directory)) return false;
  const auto space = sd_storage_capacity();
  // Low space stops new downloads, not access to an existing offline cache.
  // Publishing a new index/migration checks its own transaction headroom.
  if (!space.available) return false;
  const auto probe = directory + "/.write-test";
  FILE *file = std::fopen(probe.c_str(), "wb");
  if (!file) return false;
  bool ok = std::fwrite("readocs", 1, 7, file) == 7 && std::fflush(file) == 0 && fsync(fileno(file)) == 0;
  if (std::fclose(file) != 0) ok = false;
  std::remove(probe.c_str());
  ESP_LOGI("readocs-sd", "SD total=%llu free=%llu writable=%d", (unsigned long long)space.total, (unsigned long long)space.free, ok);
  return ok;
}
} // namespace rd
