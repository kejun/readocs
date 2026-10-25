#include "cache_file.hpp"
#include "source_metadata.hpp"
#include <cassert>
#include <cstdarg>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace rd {
#include "gc_types.hpp"
using esp_err_t = int;
static constexpr int ESP_OK = 0, ESP_ERR_NVS_NOT_FOUND = 1,
                     ESP_ERR_INVALID_SIZE = 2, ESP_FAIL = 3, handle = 1;
static constexpr char TAG[] = "readocs-store";
static std::string cache_root, read_failure, pointer_failure, set_failure;
static std::map<std::string, std::string> keys;
static bool commit_failure = false, write_failure = false;
static unsigned commits = 0;
static std::vector<std::string> logs;

static void log_event(const char *, const char *format, ...) {
  char buffer[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  logs.emplace_back(buffer);
}
#define ESP_LOGE(...) log_event(__VA_ARGS__)
#define ESP_LOGW(...) log_event(__VA_ARGS__)
static const char *esp_err_to_name(int error) {
  return error == ESP_OK ? "ESP_OK" : error == ESP_FAIL ? "ESP_FAIL" : "NVS_ERROR";
}
static int esp_spiffs_info(const char *, size_t *total, size_t *used) {
  *total = 4096 * 1024;
  *used = 3000 * 1024;
  return ESP_OK;
}

// The digest is deterministic; the tested code still verifies every immutable
// filename and the progress payload's independent inner digest.
static std::string sha256(const std::string &body) {
  std::ostringstream out;
  out << std::hex << std::setw(64) << std::setfill('0')
      << std::hash<std::string>{}(body);
  return out.str();
}
static bool valid_hash(const std::string &id) {
  return id.size() == 64 &&
         id.find_first_not_of("0123456789abcdef") == std::string::npos;
}
static std::string read_file(const std::string &id, size_t limit = 310 * 1024) {
  if (id == read_failure) {
    errno = EIO;
    return "";
  }
  return read_cache_file(cache_root + "/" + id, limit);
}
static bool write_file(const std::string &id, const std::string &body) {
  if (write_failure) {
    errno = ENOSPC;
    return false;
  }
  return publish_cache_file(cache_root + "/" + id, body, id, sha256);
}
static std::string get_string(const char *key) { return keys[key]; }
static int nvs_get_str(int, const char *key, char *out, size_t *size) {
  if (pointer_failure == key)
    return ESP_FAIL;
  const auto item = keys.find(key);
  if (item == keys.end())
    return ESP_ERR_NVS_NOT_FOUND;
  const auto required = item->second.size() + 1;
  if (!out) {
    *size = required;
    return ESP_OK;
  }
  if (*size < required)
    return ESP_ERR_INVALID_SIZE;
  std::memcpy(out, item->second.c_str(), required);
  *size = required;
  return ESP_OK;
}
static int nvs_set_str(int, const char *key, const char *value) {
  if (set_failure == key)
    return ESP_FAIL;
  keys[key] = value;
  return ESP_OK;
}
static int nvs_commit(int) {
  ++commits;
  return commit_failure ? ESP_FAIL : ESP_OK;
}
#include "gc_under_test.hpp"

static bool exists(const std::string &id) {
  return std::filesystem::exists(cache_root + "/" + id);
}
static std::set<std::string> all_files() {
  std::set<std::string> result;
  for (const auto &entry : std::filesystem::directory_iterator(cache_root))
    result.insert(entry.path().filename());
  return result;
}
static void reset() {
  for (const auto &entry : std::filesystem::directory_iterator(cache_root))
    std::filesystem::remove(entry.path());
  keys.clear();
  read_failure.clear();
  pointer_failure.clear();
  set_failure.clear();
  commit_failure = write_failure = false;
  commits = 0;
  logs.clear();
}
static std::string put(const std::string &body) {
  const auto id = sha256(body);
  assert(write_file(id, body));
  return id;
}
static void overwrite(const std::string &id, const std::string &body) {
  FILE *file = std::fopen((cache_root + "/" + id).c_str(), "wb");
  assert(file);
  assert(std::fwrite(body.data(), 1, body.size(), file) == body.size());
  assert(std::fclose(file) == 0);
}
static std::string index(const std::vector<std::string> &snapshots) {
  Json root(cJSON_CreateObject());
  cJSON_AddNumberToObject(root.get(), "schema_version", 1);
  auto *entries = cJSON_AddArrayToObject(root.get(), "entries");
  for (size_t i = 0; i < snapshots.size(); ++i) {
    auto *entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "source_id", "rss");
    cJSON_AddStringToObject(entry, "document_id", std::to_string(i).c_str());
    cJSON_AddStringToObject(entry, "title", "Article");
    cJSON_AddStringToObject(entry, "snapshot_id", snapshots[i].c_str());
    cJSON_AddItemToArray(entries, entry);
  }
  return put(serialize(root.get()));
}
static std::string progress(const std::vector<std::string> &snapshots,
                            bool bad_inner_hash = false) {
  Json root(cJSON_Parse(read_file(index(snapshots)).c_str()));
  cJSON_DeleteItemFromObjectCaseSensitive(root.get(), "schema_version");
  cJSON_AddNumberToObject(root.get(), "source", 1);
  cJSON_AddNumberToObject(root.get(), "cursor", 30);
  const auto hash = bad_inner_hash ? std::string(64, '0') : sha256(serialize(root.get()));
  cJSON_AddStringToObject(root.get(), "hash", hash.c_str());
  return put(serialize(root.get()));
}
static bool logged(const char *text) {
  return std::any_of(logs.begin(), logs.end(), [text](const auto &line) {
    return line.find(text) != std::string::npos;
  });
}
static void expect_no_collection(bool reclaim = false) {
  const auto before = all_files();
  collect_cache(reclaim);
  assert(all_files() == before);
}

static void test_valid_graph() {
  reset();
  const auto current = put("current body"), previous = put("fallback body"),
             staged = put("staged body"), displayed = put("currently displayed"),
             orphan = put("orphan");
  keys["active"] = index({current, ""});
  keys["previous"] = index({previous});
  keys["catalog_stage"] = progress({staged});
  keys["displayed"] = displayed;
  const std::set<std::string> expected{keys["active"], keys["previous"],
                                       keys["catalog_stage"], current, previous,
                                       staged, displayed};
  collect_cache(false);
  assert(all_files() == expected && !exists(orphan));
}
static void test_missing_and_corrupt_graphs() {
  for (const auto &key : {"active", "previous", "catalog_stage"}) {
    for (int fault = 0; fault < 5; ++fault) {
      reset();
      const auto body = put("preserved body");
      keys["active"] = index({body});
      keys["previous"] = index({body, ""});
      keys["catalog_stage"] = progress({body});
      put("orphan retained because graph is unknown");
      const auto id = keys[key];
      if (fault == 0)
        std::filesystem::remove(cache_root + "/" + id);
      else if (fault == 1)
        read_failure = id;
      else if (fault == 2)
        overwrite(id, "corrupted bytes");
      else if (fault == 3)
        keys[key] = put(R"({"schema_version":1,"entries":null})");
      else
        keys[key] = put(R"({"schema_version":1,"entries":[{"snapshot_id":null}]})");
      // Even explicit previous reclamation cannot proceed on a partial graph.
      expect_no_collection(true);
      assert(logged("cache gc aborted"));
      assert(commits == 0);
    }
  }
  for (const char *invalid : {
           R"({"schema_version":2,"entries":[]})",
           R"({"schema_version":1,"entries":[{}]})",
           R"({"schema_version":1,"entries":[{"snapshot_id":"bad"}]})",
           R"({"schema_version":1,"entries":[{"snapshot_id":17}]})",
           R"({"schema_version":1,"entries":[null]})", "invalid JSON"}) {
    reset();
    keys["active"] = put(invalid);
    put("unreachable until valid index");
    expect_no_collection();
  }
  reset();
  const auto body = put("body");
  keys["active"] = index({body});
  keys["catalog_stage"] = progress({body}, true);
  put("orphan retained with invalid inner hash");
  expect_no_collection();
  assert(logged("stage=progress_hash"));
}
static void test_pointer_errors_and_absence() {
  for (const char *key : {"active", "previous", "catalog_stage", "displayed"}) {
    for (int fault = 0; fault < 3; ++fault) {
      reset();
      const auto body = put("body");
      keys["active"] = index({body});
      put("orphan retained for invalid pointer");
      if (fault == 0)
        pointer_failure = key;
      else
        keys[key] = fault == 1 ? "invalid" : std::string(64, 'z');
      expect_no_collection();
      assert(logged("stage=pointer"));
    }
  }
  // Absent optional pointers are normal; a valid empty progress graph is too.
  reset();
  const auto body = put("body"), orphan = put("orphan");
  keys["active"] = index({body});
  keys["catalog_stage"] = progress({});
  collect_cache(false);
  assert(exists(body) && !exists(orphan));
  assert(exists(keys["catalog_stage"]));
}
static void test_previous_retirement() {
  for (int failure = 0; failure < 3; ++failure) {
    reset();
    const auto current = put("current"), old = put("old"), orphan = put("orphan");
    keys["active"] = index({current});
    keys["previous"] = index({old});
    const auto old_index = keys["previous"];
    if (failure == 1)
      set_failure = "previous";
    if (failure == 2)
      commit_failure = true;
    const auto before = all_files();
    collect_cache(true);
    if (failure) {
      assert(all_files() == before);
    } else {
      assert(keys["previous"] == keys["active"]);
      assert(exists(current) && !exists(old) && !exists(old_index) && !exists(orphan));
    }
  }
  reset();
  const auto current = put("current"), old = put("old");
  keys["active"] = index({current});
  keys["previous"] = index({old});
  read_failure = current;
  collect_cache(true);
  assert(exists(old)); // Active body must validate before retiring old fallback.
  assert(commits == 0);
}
static void test_commit_diagnostics() {
  const std::vector<Source> sources{{"rss", "Public feed", 1}};
  for (int failure = 0; failure < 6; ++failure) {
    reset();
    const auto body = put("valid body");
    const std::vector<Entry> entries{{"rss", "article", "Title", body}};
    keys["active"] = index({});
    if (failure == 0)
      read_failure = body;
    else if (failure == 1)
      write_failure = true;
    else if (failure == 2)
      set_failure = "previous";
    else if (failure == 3)
      commit_failure = true;
    else if (failure == 4)
      set_failure = "active";
    else {
      // No previous pointer means the first NVS commit publishes active.
      keys.erase("active");
      commit_failure = true;
    }
    assert(!commit_cache(entries, "etag", sources));
    const char *stages[]{"body_verify", "index_write", "previous_set",
                         "previous_commit", "active_set", "active_commit"};
    assert(logged((std::string("stage=") + stages[failure]).c_str()));
    assert(logged("used=3072000 total=4194304 info=ESP_OK"));
    if (failure == 0)
      assert(logged((std::string("errno=") + std::to_string(EIO)).c_str()));
    else if (failure == 1)
      assert(logged((std::string("errno=") + std::to_string(ENOSPC)).c_str()));
    else
      assert(logged("nvs=ESP_FAIL"));
  }
}
} // namespace rd

int main(int argc, char **argv) {
  assert(argc == 2);
  rd::cache_root = argv[1];
  rd::test_valid_graph();
  rd::test_missing_and_corrupt_graphs();
  rd::test_pointer_errors_and_absence();
  rd::test_previous_retirement();
  rd::test_commit_diagnostics();
}
