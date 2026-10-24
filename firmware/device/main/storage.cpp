#include "readocs.hpp"
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_spiffs.h>
#include <mbedtls/sha256.h>
#include <nvs_flash.h>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace rd {
static constexpr char TAG[] = "readocs-store";
static nvs_handle_t handle;
Position position;

std::string sha256(const std::string &s) {
  unsigned char bytes[32];
  if (mbedtls_sha256(reinterpret_cast<const unsigned char *>(s.data()),
                     s.size(), bytes, 0) != 0)
    return "";
  char hex[65]{};
  for (int i = 0; i < 32; ++i)
    std::snprintf(hex + i * 2, 3, "%02x", bytes[i]);
  return hex;
}
bool valid_hash(const std::string &s) {
  return s.size() == 64 &&
         s.find_first_not_of("0123456789abcdef") == std::string::npos;
}
std::string read_file(const std::string &name, size_t limit) {
  if (name.find('/') != std::string::npos || name.size() > 70)
    return "";
  FILE *f = std::fopen(("/cache/" + name).c_str(), "rb");
  if (!f)
    return "";
  std::fseek(f, 0, SEEK_END);
  long size = std::ftell(f);
  std::rewind(f);
  if (size < 0 || static_cast<size_t>(size) > limit) {
    std::fclose(f);
    return "";
  }
  std::string out(size, '\0');
  size_t got = std::fread(out.data(), 1, size, f);
  std::fclose(f);
  return got == static_cast<size_t>(size) ? out : "";
}
bool write_file(const std::string &name, const std::string &body) {
  if (name.find('/') != std::string::npos || name.size() > 70)
    return false;
  if (valid_hash(name) && sha256(read_file(name, body.size())) == name)
    return true;
  const std::string path = "/cache/" + name, temp = path + ".part";
  FILE *f = std::fopen(temp.c_str(), "wb");
  if (!f)
    return false;
  bool ok = std::fwrite(body.data(), 1, body.size(), f) == body.size() &&
            std::fflush(f) == 0;
  if (ok)
    ok = fsync(fileno(f)) == 0;
  if (std::fclose(f) != 0)
    ok = false;
  if (ok)
    ok = std::rename(temp.c_str(), path.c_str()) == 0;
  if (!ok)
    std::remove(temp.c_str());
  return ok;
}
static std::string get_string(const char *key) {
  size_t size = 0;
  if (nvs_get_str(handle, key, nullptr, &size) != ESP_OK || size > 24576)
    return "";
  std::string out(size, '\0');
  if (nvs_get_str(handle, key, out.data(), &size) != ESP_OK)
    return "";
  if (!out.empty())
    out.pop_back();
  return out;
}
bool save_config(const Config &c) {
  Json root(cJSON_CreateObject());
  auto *a = cJSON_AddArrayToObject(root.get(), "networks");
  for (const auto &p : c.networks) {
    auto *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "ssid", p.ssid.c_str());
    cJSON_AddStringToObject(j, "password", p.password.c_str());
    cJSON_AddItemToArray(a, j);
  }
  cJSON_AddStringToObject(root.get(), "server", c.server.c_str());
  cJSON_AddStringToObject(root.get(), "ca", c.ca.c_str());
  cJSON_AddStringToObject(root.get(), "token", c.token.c_str());
  cJSON_AddStringToObject(root.get(), "device_id", c.device_id.c_str());
  cJSON_AddNumberToObject(root.get(), "interval", c.interval);
  cJSON_AddNumberToObject(root.get(), "font_size", c.font_size);
  auto body = serialize(root.get());
  return nvs_set_str(handle, "config", body.c_str()) == ESP_OK &&
         nvs_commit(handle) == ESP_OK;
}
std::string directory_progress() {
  auto id = get_string("catalog_stage");
  if (!valid_hash(id))
    return "";
  auto body = read_file(id, 512 * 1024);
  return sha256(body) == id ? body : "";
}
bool save_directory_progress_file(const std::string &body) {
  auto id = sha256(body);
  if (body.size() > 512 * 1024 || !write_file(id, body))
    return false;
  if (id == get_string("catalog_stage"))
    return true;
  auto old = get_string("catalog_stage");
  bool ok = nvs_set_str(handle, "catalog_stage", id.c_str()) == ESP_OK &&
            nvs_commit(handle) == ESP_OK;
  // Only obsolete progress files are removed; active/previous reader indexes
  // remain untouched.
  if (ok && valid_hash(old) && old != id)
    std::remove(("/cache/" + old).c_str());
  return ok;
}
void clear_directory_progress() {
  auto id = get_string("catalog_stage");
  if (nvs_erase_key(handle, "catalog_stage") == ESP_OK &&
      nvs_commit(handle) == ESP_OK && valid_hash(id))
    std::remove(("/cache/" + id).c_str());
}
static bool load_index(const std::string &id, std::vector<Entry> &out,
                       std::string &etag) {
  if (!valid_hash(id))
    return false;
  auto body = read_file(id, 512 * 1024);
  if (sha256(body) != id)
    return false;
  Json root(cJSON_Parse(body.c_str()));
  if (!root || num(root.get(), "schema_version") != 1)
    return false;
  auto *array = cJSON_GetObjectItemCaseSensitive(root.get(), "entries");
  if (!cJSON_IsArray(array))
    return false;
  std::vector<Entry> entries;
  cJSON *j;
  cJSON_ArrayForEach(j, array) {
    Entry e{str(j, "source_id"), str(j, "document_id"), str(j, "title"),
            str(j, "snapshot_id")};
    if (!e.snapshot_id.empty()) {
      auto doc = read_file(e.snapshot_id);
      if (!valid_hash(e.snapshot_id) || sha256(doc) != e.snapshot_id)
        return false;
    }
    entries.push_back(e);
  }
  out = std::move(entries);
  etag = str(root.get(), "etag");
  return true;
}
bool load_storage() {
  if (nvs_flash_init_partition("settings") != ESP_OK ||
      nvs_open_from_partition("settings", "readocs", NVS_READWRITE, &handle) !=
          ESP_OK)
    return false;
  Json root(cJSON_Parse(get_string("config").c_str()));
  if (root) {
    cJSON *j;
    cJSON_ArrayForEach(
        j, cJSON_GetObjectItemCaseSensitive(root.get(), "networks")) {
      if (model.config.networks.size() < 5)
        model.config.networks.push_back({str(j, "ssid"), str(j, "password")});
    }
    model.config.server = str(root.get(), "server");
    model.config.ca = str(root.get(), "ca");
    model.config.token = str(root.get(), "token");
    model.config.device_id = str(root.get(), "device_id");
    model.config.interval = num(root.get(), "interval", 3600);
    model.config.font_size = num(root.get(), "font_size", 24);
  } else {
    // Import a successful diagnostic network once; never erase factory or
    // unrelated NVS.
    nvs_handle_t old;
    if (nvs_open("readocs_net", NVS_READONLY, &old) == ESP_OK) {
      char ssid[33]{}, pass[64]{};
      size_t sn = sizeof(ssid), pn = sizeof(pass);
      if (nvs_get_str(old, "ssid", ssid, &sn) == ESP_OK &&
          nvs_get_str(old, "pass", pass, &pn) == ESP_OK) {
        model.config.networks.push_back({ssid, pass});
        save_config(model.config);
      }
      std::memset(pass, 0, sizeof(pass));
      nvs_close(old);
    }
  }
  esp_vfs_spiffs_conf_t cfg{};
  cfg.base_path = "/cache";
  cfg.partition_label = "cache";
  cfg.max_files = 8;
  cfg.format_if_mount_failed = false;
  auto err = esp_vfs_spiffs_register(&cfg);
  if (err == ESP_FAIL) {
    // Only format a new, wholly erased cache partition. Corrupt existing
    // content is retained for recovery.
    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "cache");
    uint8_t sample[4096];
    bool erased = p != nullptr;
    if (p)
      for (size_t offset = 0; offset < p->size && erased;
           offset += sizeof(sample)) {
        if (esp_partition_read(p, offset, sample, sizeof(sample)) != ESP_OK) {
          erased = false;
          break;
        }
        for (auto byte : sample)
          if (byte != 0xff) {
            erased = false;
            break;
          }
      }
    if (erased && esp_spiffs_format("cache") == ESP_OK)
      err = esp_vfs_spiffs_register(&cfg);
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Cache mount failed: %s", esp_err_to_name(err));
    return false;
  }
  if (!load_index(get_string("active"), model.entries, model.etag))
    load_index(get_string("previous"), model.entries, model.etag);
  model.displayed_snapshot = get_string("displayed");
  Json pos(cJSON_Parse(get_string("position").c_str()));
  if (pos) {
    position.document = str(pos.get(), "document");
    position.block = str(pos.get(), "block");
    position.snapshot = str(pos.get(), "snapshot");
    position.offset = num(pos.get(), "offset");
    position.page = num(pos.get(), "page");
  }
  return true;
}
bool commit_cache(const std::vector<Entry> &entries, const std::string &etag) {
  for (const auto &e : entries)
    if (!e.snapshot_id.empty() &&
        sha256(read_file(e.snapshot_id)) != e.snapshot_id)
      return false;
  Json root(cJSON_CreateObject());
  cJSON_AddNumberToObject(root.get(), "schema_version", 1);
  cJSON_AddStringToObject(root.get(), "etag", etag.c_str());
  auto *a = cJSON_AddArrayToObject(root.get(), "entries");
  for (const auto &e : entries) {
    auto *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "source_id", e.source_id.c_str());
    cJSON_AddStringToObject(j, "document_id", e.document_id.c_str());
    cJSON_AddStringToObject(j, "title", e.title.c_str());
    cJSON_AddStringToObject(j, "snapshot_id", e.snapshot_id.c_str());
    cJSON_AddItemToArray(a, j);
  }
  auto body = serialize(root.get()), id = sha256(body);
  if (body.size() > 512 * 1024)
    return false;
  if (id == get_string("active"))
    return true;
  if (!write_file(id, body))
    return false;
  auto old = get_string("active");
  if (!old.empty() && (nvs_set_str(handle, "previous", old.c_str()) != ESP_OK ||
                       nvs_commit(handle) != ESP_OK))
    return false;
  return nvs_set_str(handle, "active", id.c_str()) == ESP_OK &&
         nvs_commit(handle) == ESP_OK;
}
void save_position(const Position &p) {
  Json j(cJSON_CreateObject());
  cJSON_AddStringToObject(j.get(), "document", p.document.c_str());
  cJSON_AddStringToObject(j.get(), "block", p.block.c_str());
  cJSON_AddStringToObject(j.get(), "snapshot", p.snapshot.c_str());
  cJSON_AddNumberToObject(j.get(), "offset", p.offset);
  cJSON_AddNumberToObject(j.get(), "page", p.page);
  auto s = serialize(j.get());
  if (s == get_string("position"))
    return;
  Json all(cJSON_Parse(get_string("positions").c_str()));
  if (!all)
    all.reset(cJSON_CreateObject());
  cJSON_DeleteItemFromObjectCaseSensitive(all.get(), p.document.c_str());
  cJSON_AddItemToObject(all.get(), p.document.c_str(),
                        cJSON_Duplicate(j.get(), true));
  while (cJSON_GetArraySize(all.get()) > 101)
    cJSON_DeleteItemFromObjectCaseSensitive(all.get(), all->child->string);
  auto map = serialize(all.get());
  if (nvs_set_str(handle, "position", s.c_str()) != ESP_OK ||
      nvs_set_str(handle, "positions", map.c_str()) != ESP_OK ||
      nvs_commit(handle) != ESP_OK)
    ESP_LOGW(TAG, "Position commit failed");
}
Position load_position(const std::string &doc) {
  Json all(cJSON_Parse(get_string("positions").c_str()));
  auto *p = cJSON_GetObjectItemCaseSensitive(all.get(), doc.c_str());
  if (!cJSON_IsObject(p))
    return {};
  return {str(p, "document"), str(p, "block"), str(p, "snapshot"),
          static_cast<unsigned>(num(p, "offset")), num(p, "page")};
}
void collect_cache() {
  std::set<std::string> keep;
  for (const char *key : {"active", "previous"}) {
    auto id = get_string(key);
    if (!valid_hash(id))
      continue;
    keep.insert(id);
    Json root(cJSON_Parse(read_file(id, 512 * 1024).c_str()));
    cJSON *e;
    cJSON_ArrayForEach(
        e, cJSON_GetObjectItemCaseSensitive(root.get(), "entries")) {
      auto snapshot = str(e, "snapshot_id");
      if (valid_hash(snapshot))
        keep.insert(snapshot);
    }
  }
  auto staged = get_string("catalog_stage");
  if (valid_hash(staged))
    keep.insert(staged);
  Json pending(cJSON_Parse(directory_progress().c_str()));
  cJSON *item;
  cJSON_ArrayForEach(
      item, cJSON_GetObjectItemCaseSensitive(pending.get(), "entries")) {
    auto snapshot = str(item, "snapshot_id");
    if (valid_hash(snapshot))
      keep.insert(snapshot);
  }
  auto displayed = get_string("displayed");
  if (valid_hash(displayed))
    keep.insert(displayed);
  DIR *dir = opendir("/cache");
  if (!dir)
    return;
  while (auto *e = readdir(dir)) {
    std::string name = e->d_name;
    if (name[0] != '.' && !keep.count(name))
      std::remove(("/cache/" + name).c_str());
  }
  closedir(dir);
}
void display_marker(bool pending, const std::string &snapshot) {
  if (!pending && !snapshot.empty()) {
    std::lock_guard<std::mutex> g(model.mutex);
    model.displayed_snapshot = snapshot;
  }
  if (nvs_set_u8(handle, "refresh_pending", pending ? 1 : 0) != ESP_OK)
    return;
  if (!pending && !snapshot.empty())
    nvs_set_str(handle, "displayed", snapshot.c_str());
  if (nvs_commit(handle) != ESP_OK)
    ESP_LOGW(TAG, "Display checkpoint failed");
}
bool display_pending() {
  uint8_t p = 0;
  nvs_get_u8(handle, "refresh_pending", &p);
  return p != 0;
}
} // namespace rd
