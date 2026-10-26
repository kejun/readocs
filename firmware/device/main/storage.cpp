#include "readocs.hpp"
#include "cache_file.hpp"
#include "external_storage.hpp"
#include "position_storage.hpp"
#include "source_metadata.hpp"
#include <cerrno>
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
static nvs_handle_t handle, index_handle;
static std::string root_directory = "/cache";
static std::mutex verified_mutex;
static std::set<std::string> verified_files;
const std::string &cache_directory() { return root_directory; }
bool external_cache_enabled() { return root_directory != "/cache"; }
bool cache_has_space(uint64_t bytes) {
  if (!external_cache_enabled()) return true;
  const auto space = sd_storage_capacity();
  return sd_can_fit(space, bytes);
}
bool cache_body_verified(const std::string &id) {
  if (!external_cache_enabled()) return false;
  std::lock_guard<std::mutex> lock(verified_mutex);
  return verified_files.count(id) != 0;
}
static void remember_verified(const std::string &id) {
  if (!external_cache_enabled()) return;
  std::lock_guard<std::mutex> lock(verified_mutex);
  if (verified_files.size() >= 4096) verified_files.clear();
  verified_files.insert(id);
}
static nvs_handle_t key_handle(const char *key) {
  return !std::strcmp(key, "active") || !std::strcmp(key, "previous") ||
         !std::strcmp(key, "catalog_stage") ? index_handle : handle;
}
static void forget_verified(const std::string &id) {
  std::lock_guard<std::mutex> lock(verified_mutex);
  verified_files.erase(id);
}
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
  if (!valid_hash(name)) return "";
  auto body = read_cache_file(cache_directory() + "/" + name, limit);
  if (external_cache_enabled() && sha256(body) != name) {
    forget_verified(name);
    return "";
  }
  if (!body.empty()) remember_verified(name);
  return body;
}
bool write_file(const std::string &name, const std::string &body) {
  if (name.find('/') != std::string::npos || name.size() > 70)
    return false;
  if (!valid_hash(name))
    return false;
  // The reserved headroom must remain available to publish the final index
  // and checkpoint after body prefetch reaches its low-space boundary.
  if (external_cache_enabled() && !sd_can_publish(sd_storage_capacity(), body.size())) {
    errno = ENOSPC;
    return false;
  }
  const bool ok = publish_cache_file(cache_directory() + "/" + name, body, name, sha256);
  if (ok) remember_verified(name);
  return ok;
}
static std::string get_string(const char *key) {
  size_t size = 0;
  if (nvs_get_str(key_handle(key), key, nullptr, &size) != ESP_OK || size > 24576)
    return "";
  std::string out(size, '\0');
  if (nvs_get_str(key_handle(key), key, out.data(), &size) != ESP_OK)
    return "";
  if (!out.empty())
    out.pop_back();
  return out;
}
static bool cache_pointer(const char *key, std::string &out) {
  size_t size = 0;
  auto err = nvs_get_str(key_handle(key), key, nullptr, &size);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    out.clear();
    return true;
  }
  if (err == ESP_OK && size != 65)
    err = ESP_ERR_INVALID_SIZE;
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "cache gc aborted stage=pointer key=%s err=%s", key,
             esp_err_to_name(err));
    return false;
  }
  std::string value(size, '\0');
  err = nvs_get_str(key_handle(key), key, value.data(), &size);
  if (err != ESP_OK || size != 65 || value.back() != '\0') {
    ESP_LOGW(TAG, "cache gc aborted stage=pointer_read key=%s err=%s", key,
             esp_err_to_name(err));
    return false;
  }
  value.pop_back();
  if (!valid_hash(value)) {
    ESP_LOGW(TAG, "cache gc aborted stage=pointer_hash key=%s", key);
    return false;
  }
  out = std::move(value);
  return true;
}
static std::string get_positions() {
  return read_position_map(handle);
}
// Explicit diagnostics only; ordinary light-sleep cycles never write NVS.
bool save_sleep_probe(const std::string &json) {
  return json.size() <= 1024 &&
         nvs_set_str(handle, "sleep_probe", json.c_str()) == ESP_OK &&
         nvs_commit(handle) == ESP_OK;
}
std::string load_sleep_probe() { return get_string("sleep_probe"); }
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
  cJSON_AddStringToObject(root.get(), "local_server", c.local_server.c_str());
  cJSON_AddStringToObject(root.get(), "local_ca", c.local_ca.c_str());
  cJSON_AddStringToObject(root.get(), "local_name", c.local_name.c_str());
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
  bool ok = nvs_set_str(index_handle, "catalog_stage", id.c_str()) == ESP_OK &&
            nvs_commit(index_handle) == ESP_OK;
  // Only obsolete progress files are removed; active/previous reader indexes
  // remain untouched.
  if (ok && valid_hash(old) && old != id)
    std::remove((cache_directory() + "/" + old).c_str());
  return ok;
}
void clear_directory_progress() {
  auto id = get_string("catalog_stage");
  if (nvs_erase_key(index_handle, "catalog_stage") == ESP_OK &&
      nvs_commit(index_handle) == ESP_OK && valid_hash(id))
    std::remove((cache_directory() + "/" + id).c_str());
}
static bool load_index(const std::string &id, std::vector<Entry> &out,
                       std::string &etag, std::vector<Source> &sources,
                       bool verify_bodies = true) {
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
            str(j, "snapshot_id"), str(j, "target_snapshot_id")};
    if (!e.snapshot_id.empty()) {
      auto doc = verify_bodies ? read_file(e.snapshot_id) : "";
      if (!valid_hash(e.snapshot_id) || (verify_bodies && sha256(doc) != e.snapshot_id))
        return false;
    }
    entries.push_back(e);
  }
  std::vector<Source> declared;
  auto *metadata = cJSON_GetObjectItemCaseSensitive(root.get(), "sources");
  if (metadata ? !read_source_metadata(metadata, declared)
               : !infer_source_metadata(entries, declared))
    return false;
  if (!source_entries_valid(entries, declared))
    return false;
  out = std::move(entries);
  sources = std::move(declared);
  // A legacy index cannot reveal empty sources or their real display names.
  // Force one manifest response instead of letting its old ETag return 304.
  etag = metadata ? str(root.get(), "etag") : "";
  return true;
}
bool load_storage() {
  if (nvs_flash_init_partition("settings") != ESP_OK ||
      nvs_open_from_partition("settings", "readocs", NVS_READWRITE, &handle) !=
          ESP_OK)
    return false;
  index_handle = handle;
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
    model.config.local_server = str(root.get(), "local_server");
    model.config.local_ca = str(root.get(), "local_ca");
    model.config.local_name = str(root.get(), "local_name");
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
  if (!load_index(get_string("active"), model.entries, model.etag, model.sources))
    load_index(get_string("previous"), model.entries, model.etag, model.sources);
  // SD metadata has independent pointers. Removing a card restores the
  // original internal cache; migration never erases those files or pointers.
  if (mount_sd_storage()) {
    nvs_handle_t sd_handle;
    if (nvs_open_from_partition("settings", "readocs_sd", NVS_READWRITE, &sd_handle) == ESP_OK) {
      auto original_entries = model.entries;
      auto original_sources = model.sources;
      auto original_etag = model.etag;
      root_directory = sd_cache_directory();
      index_handle = sd_handle;
      bool loaded = load_index(get_string("active"), model.entries, model.etag, model.sources, false) ||
                    load_index(get_string("previous"), model.entries, model.etag, model.sources, false);
      if (!loaded) {
        bool migrated = true;
        for (const auto &entry : original_entries) {
          if (entry.snapshot_id.empty()) continue;
          auto body = read_cache_file("/cache/" + entry.snapshot_id, 310 * 1024);
          if (sha256(body) != entry.snapshot_id || !write_file(entry.snapshot_id, body)) { migrated = false; break; }
        }
        migrated = migrated && commit_cache(original_entries, original_etag, original_sources);
        if (migrated) {
          const auto active = get_string("active");
          nvs_set_str(index_handle, "previous", active.c_str());
          nvs_erase_key(index_handle, "catalog_stage");
          nvs_commit(index_handle);
        } else {
          root_directory = "/cache";
          index_handle = handle;
          nvs_close(sd_handle);
        }
        model.entries = std::move(original_entries);
        model.sources = std::move(original_sources);
        model.etag = original_etag;
      }
    }
  }
  model.displayed_snapshot = get_string("displayed");
  Json pos(cJSON_Parse(get_string("position").c_str()));
  if (pos) {
    position.document = str(pos.get(), "document");
    position.block = str(pos.get(), "block");
    position.snapshot = str(pos.get(), "snapshot");
    position.offset = num(pos.get(), "offset");
    position.page = num(pos.get(), "page");
    position.font_revision = num(pos.get(), "font_revision");
  }
  return true;
}
static bool cache_commit_failure(const char *stage, int saved_errno = 0,
                                  esp_err_t nvs_error = ESP_OK, size_t bytes = 0) {
  size_t total = 0, used = 0;
  const auto info = esp_spiffs_info("cache", &total, &used);
  ESP_LOGE(TAG,
           "cache commit stage=%s errno=%d nvs=%s bytes=%u used=%u total=%u info=%s",
           stage, saved_errno, esp_err_to_name(nvs_error),
           static_cast<unsigned>(bytes), static_cast<unsigned>(used),
           static_cast<unsigned>(total), esp_err_to_name(info));
  return false;
}
bool commit_cache(const std::vector<Entry> &entries, const std::string &etag,
                  const std::vector<Source> &sources) {
  if (!source_entries_valid(entries, sources))
    return cache_commit_failure("source_map");
  for (const auto &e : entries) {
    if (e.snapshot_id.empty())
      continue;
    errno = 0;
    if (cache_body_verified(e.snapshot_id)) continue;
    auto body = read_file(e.snapshot_id);
    if (sha256(body) != e.snapshot_id)
      return cache_commit_failure("body_verify", errno, ESP_OK, body.size());
  }
  Json root(cJSON_CreateObject());
  cJSON_AddNumberToObject(root.get(), "schema_version", 1);
  cJSON_AddStringToObject(root.get(), "etag", etag.c_str());
  write_source_metadata(root.get(), sources);
  auto *a = cJSON_AddArrayToObject(root.get(), "entries");
  for (const auto &e : entries) {
    auto *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "source_id", e.source_id.c_str());
    cJSON_AddStringToObject(j, "document_id", e.document_id.c_str());
    cJSON_AddStringToObject(j, "title", e.title.c_str());
    cJSON_AddStringToObject(j, "snapshot_id", e.snapshot_id.c_str());
    cJSON_AddStringToObject(j, "target_snapshot_id", e.target_snapshot_id.c_str());
    cJSON_AddItemToArray(a, j);
  }
  auto body = serialize(root.get()), id = sha256(body);
  if (body.size() > 512 * 1024)
    return cache_commit_failure("index_size", 0, ESP_OK, body.size());
  if (id == get_string("active"))
    return true;
  errno = 0;
  if (!write_file(id, body))
    return cache_commit_failure("index_write", errno, ESP_OK, body.size());
  auto old = get_string("active");
  if (!old.empty()) {
    auto err = nvs_set_str(index_handle, "previous", old.c_str());
    if (err != ESP_OK)
      return cache_commit_failure("previous_set", 0, err);
    err = nvs_commit(index_handle);
    if (err != ESP_OK)
      return cache_commit_failure("previous_commit", 0, err);
  }
  auto err = nvs_set_str(index_handle, "active", id.c_str());
  if (err != ESP_OK)
    return cache_commit_failure("active_set", 0, err);
  err = nvs_commit(index_handle);
  if (err != ESP_OK)
    return cache_commit_failure("active_commit", 0, err);
  return true;
}
void save_position(const Position &p) {
  Json j(cJSON_CreateObject());
  cJSON_AddStringToObject(j.get(), "document", p.document.c_str());
  cJSON_AddStringToObject(j.get(), "block", p.block.c_str());
  cJSON_AddStringToObject(j.get(), "snapshot", p.snapshot.c_str());
  cJSON_AddNumberToObject(j.get(), "offset", p.offset);
  cJSON_AddNumberToObject(j.get(), "page", p.page);
  cJSON_AddNumberToObject(j.get(), "font_revision", p.font_revision);
  auto s = serialize(j.get());
  Json all(cJSON_Parse(get_positions().c_str()));
  if (!all)
    all.reset(cJSON_CreateObject());
  cJSON_DeleteItemFromObjectCaseSensitive(all.get(), p.document.c_str());
  cJSON_AddItemToObject(all.get(), p.document.c_str(),
                        cJSON_Duplicate(j.get(), true));
  auto map = serialize(all.get());
  while (cJSON_GetArraySize(all.get()) > 1 &&
         (cJSON_GetArraySize(all.get()) > 101 || map.size() > positions_limit)) {
    cJSON_DeleteItemFromObjectCaseSensitive(all.get(), all->child->string);
    map = serialize(all.get());
  }
  if (map == get_positions() && s == get_string("position"))
    return;
  // A string occupies one NVS page (at most 4000 bytes). Blobs span pages,
  // allowing the complete per-document position map to survive a restart.
  if (map.size() > positions_limit ||
      write_position_map(handle, map) != ESP_OK ||
      nvs_set_str(handle, "position", s.c_str()) != ESP_OK ||
      nvs_commit(handle) != ESP_OK)
    ESP_LOGW(TAG, "Position commit failed");
}
Position load_position(const std::string &doc) {
  Json all(cJSON_Parse(get_positions().c_str()));
  auto *p = cJSON_GetObjectItemCaseSensitive(all.get(), doc.c_str());
  if (!cJSON_IsObject(p))
    return {};
  return {str(p, "document"), str(p, "block"), str(p, "snapshot"),
          static_cast<unsigned>(num(p, "offset")), num(p, "page"),
          static_cast<uint32_t>(num(p, "font_revision"))};
}
static bool mark_cache_links(const std::string &id, bool progress,
                              std::set<std::string> &keep) {
  auto body = read_file(id, 512 * 1024);
  if (sha256(body) != id) {
    ESP_LOGW(TAG, "cache gc aborted stage=graph_read progress=%d bytes=%u",
             progress, static_cast<unsigned>(body.size()));
    return false;
  }
  Json root(cJSON_Parse(body.c_str()));
  auto *entries = cJSON_GetObjectItemCaseSensitive(root.get(), "entries");
  if (!root || !cJSON_IsObject(root.get()) || !cJSON_IsArray(entries) ||
      (!progress && num(root.get(), "schema_version") != 1)) {
    ESP_LOGW(TAG, "cache gc aborted stage=graph_structure progress=%d", progress);
    return false;
  }
  if (progress) {
    auto hash = str(root.get(), "hash");
    cJSON_DeleteItemFromObjectCaseSensitive(root.get(), "hash");
    if (!valid_hash(hash) || sha256(serialize(root.get())) != hash) {
      ESP_LOGW(TAG, "cache gc aborted stage=progress_hash");
      return false;
    }
  }
  keep.insert(id);
  cJSON *entry;
  cJSON_ArrayForEach(entry, entries) {
    auto *snapshot = cJSON_GetObjectItemCaseSensitive(entry, "snapshot_id");
    if (!cJSON_IsObject(entry) || !cJSON_IsString(snapshot) ||
        (snapshot->valuestring[0] && !valid_hash(snapshot->valuestring))) {
      ESP_LOGW(TAG, "cache gc aborted stage=graph_entry progress=%d", progress);
      return false;
    }
    if (snapshot->valuestring[0])
      keep.insert(snapshot->valuestring);
  }
  return true;
}
void collect_cache(bool reclaim_previous) {
  std::string active, previous, staged, displayed;
  if (!cache_pointer("active", active) || !cache_pointer("previous", previous) ||
      !cache_pointer("catalog_stage", staged) ||
      !cache_pointer("displayed", displayed))
    return;
  std::set<std::string> keep, previous_keep;
  if (!active.empty() && !mark_cache_links(active, false, keep))
    return;
  if (!previous.empty() && previous != active &&
      !mark_cache_links(previous, false, previous_keep))
    return;
  if (!staged.empty() && !mark_cache_links(staged, true, keep))
    return;
  if (reclaim_previous) {
    // Reclaim an older fallback only after proving the current active index
    // and every body it references are complete. The active index then stays
    // pinned throughout downloads and becomes previous at the next commit.
    std::vector<Entry> verified;
    std::vector<Source> sources;
    std::string etag;
    if (!active.empty() && active != previous &&
        load_index(active, verified, etag, sources)) {
      if (nvs_set_str(index_handle, "previous", active.c_str()) != ESP_OK ||
          nvs_commit(index_handle) != ESP_OK) {
        ESP_LOGW(TAG, "Unable to reclaim previous cache version");
        return;
      }
      previous = active;
      previous_keep.clear();
    }
  }
  keep.insert(previous_keep.begin(), previous_keep.end());
  if (!displayed.empty())
    keep.insert(displayed);
  DIR *dir = opendir(cache_directory().c_str());
  if (!dir)
    return;
  while (auto *e = readdir(dir)) {
    std::string name = e->d_name;
    if (name[0] != '.' && !keep.count(name))
      if (std::remove((cache_directory() + "/" + name).c_str()) == 0)
        forget_verified(name);
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
