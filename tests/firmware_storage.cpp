#include "cache_file.hpp"
#include "cache_policy.hpp"
#include "position_storage.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

static std::map<std::string, std::string> strings, blobs;
static bool fail_blob_write = false;
esp_err_t nvs_get_str(nvs_handle_t, const char *key, char *out, size_t *size) {
  const auto found = strings.find(key);
  if (found == strings.end())
    return ESP_ERR_NVS_NOT_FOUND;
  const auto required = found->second.size() + 1;
  if (out) {
    assert(*size >= required);
    std::memcpy(out, found->second.c_str(), required);
  }
  *size = required;
  return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t, const char *key, void *out, size_t *size) {
  const auto found = blobs.find(key);
  if (found == blobs.end())
    return ESP_ERR_NVS_NOT_FOUND;
  if (out) {
    assert(*size >= found->second.size());
    std::memcpy(out, found->second.data(), found->second.size());
  }
  *size = found->second.size();
  return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t, const char *key, const void *body, size_t size) {
  if (fail_blob_write)
    return ESP_FAIL;
  blobs[key] = std::string(static_cast<const char *>(body), size);
  return ESP_OK;
}

struct Entry {
  std::string source_id, document_id, snapshot_id;
  bool operator==(const Entry &other) const {
    return source_id == other.source_id && document_id == other.document_id &&
           snapshot_id == other.snapshot_id;
  }
};
static std::string digest(const std::string &body) {
  return std::to_string(std::hash<std::string>{}(body));
}

int main(int argc, char **argv) {
  assert(argc == 2);
  const auto directory = std::filesystem::path(argv[1]);
  const auto path = (directory / "object").string();
  const std::string valid = "A complete downloaded document";
  const auto hash = digest(valid);
  auto put = [](const std::string &file, const std::string &body) {
    std::ofstream stream(file, std::ios::binary);
    stream << body;
  };
  const std::string large_file(300 * 1024 + 17, 'r');
  put(path, large_file);
  size_t yields = 0;
  assert(rd::read_cache_file(path, large_file.size(), [&] { ++yields; }) ==
         large_file);
  assert(yields == (large_file.size() + rd::cache_read_chunk - 1) /
                       rd::cache_read_chunk);
  yields = 0;
  assert(rd::read_cache_file(path, large_file.size() - 1,
                             [&] { ++yields; }).empty());
  assert(yields == 0); // Reject oversize objects before allocating or reading.
  // Simulate an incomplete object after fstat captured its original length.
  assert(rd::read_cache_file(path, large_file.size(), [&] {
           if (++yields == 1)
             std::filesystem::resize_file(path, rd::cache_read_chunk);
         }).empty());
  assert(yields == 2);
  // A damaged immutable object is replaceable on SPIFFS without relying on
  // POSIX rename-over-existing semantics.
  put(path, "bad");
  assert(rd::publish_cache_file(path, valid, hash, digest));
  assert(rd::read_cache_file(path, 1024) == valid);
  assert(!std::filesystem::exists(path + ".part"));
  // Invalid downloads and failed staging do not destroy an existing object.
  assert(!rd::publish_cache_file(path, "bad replacement", hash, digest));
  assert(rd::read_cache_file(path, 1024) == valid);
  std::filesystem::create_directory(path + ".part");
  assert(rd::publish_cache_file(path, valid, hash, digest));
  put(path, "bad");
  assert(!rd::publish_cache_file(path, valid, hash, digest));
  assert(rd::read_cache_file(path, 1024) == "bad");
  std::filesystem::remove(path + ".part");
  // A reset after removing a corrupt destination can be recovered next time.
  std::filesystem::remove(path);
  put(path + ".part", valid);
  assert(rd::publish_cache_file(path, valid, hash, digest));
  assert(rd::read_cache_file(path, 1024) == valid);

  // Preserve legacy data on an interrupted migration, then restore a map much
  // larger than both the old NVS string limit and the old 24 KiB read limit.
  strings["positions"] = "{\"legacy\":{\"page\":2}}";
  assert(rd::read_position_map(1) == strings["positions"]);
  const std::string large = "{\"positions\":\"" + std::string(32000, 'x') + "\"}";
  fail_blob_write = true;
  assert(rd::write_position_map(1, large) != ESP_OK);
  assert(rd::read_position_map(1) == strings["positions"]);
  fail_blob_write = false;
  assert(rd::write_position_map(1, large) == ESP_OK);
  assert(rd::read_position_map(1) == large);
  assert(rd::write_position_map(1, std::string(rd::positions_limit + 1, 'x')) ==
         ESP_ERR_NVS_VALUE_TOO_LONG);
  assert(rd::read_position_map(1) == large);

  // Ordinary feed growth must never pin every article ever prefetched. The
  // current article remains fetchable even when it has fallen past item five.
  std::vector<Entry> entries;
  for (int total = 5; total <= 50; ++total) {
    entries.insert(entries.begin(), {"feed", std::to_string(total), "download"});
    while (entries.size() < static_cast<size_t>(total))
      entries.push_back({"feed", std::to_string(entries.size()), "download"});
    const auto keep = rd::cached_article_mask(entries, "", "");
    for (size_t i = 0; i < entries.size(); ++i)
      if (keep[i])
        entries[i].snapshot_id = "download";
    rd::trim_cached_articles(entries, "", "");
    assert(std::count_if(entries.begin(), entries.end(), [](const Entry &e) {
             return !e.snapshot_id.empty();
           }) == 5);
  }
  entries.push_back({"other-feed", "current", "other"});
  entries.push_back({"feed", "current", "old-snapshot"});
  const auto keep = rd::cached_article_mask(entries, "feed", "current");
  assert(keep.back());
  rd::trim_cached_articles(entries, "feed", "current");
  assert(entries.back().snapshot_id == "old-snapshot");
  assert(std::count_if(entries.begin(), entries.end(), [](const Entry &e) {
           return e.source_id == "feed" && !e.snapshot_id.empty();
         }) == 6);
  // Moving to another article releases the previous extra cache reference.
  rd::trim_cached_articles(entries, "other-feed", "current");
  assert(entries.back().snapshot_id.empty());
  // A queued download completing after selection changes must not evict the
  // current article or discard its own result before the UI can open it.
  entries.back().snapshot_id = "newly-downloaded";
  entries.push_back({"feed", "displayed", "displayed-body"});
  rd::trim_cached_articles(entries, "feed", "displayed", "feed", "current");
  assert(entries[entries.size() - 2].snapshot_id == "newly-downloaded");
  assert(entries.back().snapshot_id == "displayed-body");

  // Exercise the same migration transaction used by sync: an old full cache
  // and its directory checkpoint must stop pinning the surplus bodies before
  // another download is attempted, even if no new body was committed yet.
  entries.clear();
  for (int i = 0; i < 50; ++i)
    entries.push_back({"feed", std::to_string(i), "body-" + std::to_string(i)});
  auto active = entries, previous = entries, checkpoint = entries;
  bool committed = false, reclaimed = false;
  assert(!rd::compact_cache_for_download(
      entries, "feed", "49", [](const auto &) { return false; },
      [&] { assert(false); }, [&] { assert(false); }));
  assert(entries == active && entries == checkpoint);
  assert(rd::compact_cache_for_download(
      entries, "feed", "49",
      [&](const auto &compact) {
        previous = active;
        active = compact;
        committed = true;
        return true;
      },
      [&] {
        assert(committed && active != checkpoint);
        checkpoint.clear();
      },
      [&] {
        assert(committed && checkpoint.empty());
        previous = active; // Only a verified-complete active may replace it.
        reclaimed = true;
      }));
  assert(reclaimed && previous == active && active == entries);
  assert(std::count_if(previous.begin(), previous.end(), [](const Entry &e) {
           return !e.snapshot_id.empty();
         }) == 6);
  // An unchanged follow-up may take commit_cache's early return: it no longer
  // keeps the old 50-body fallback or checkpoint alive.
  assert(rd::compact_cache_for_download(
      entries, "feed", "49", [](const auto &) { assert(false); return false; },
      [] { assert(false); }, [] { assert(false); }));
}
