#include "source_metadata.hpp"
#include <cassert>
#include <cerrno>
#include <functional>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace rd {
#include "source_types.hpp"
static std::map<std::string, std::string> files, keys;
static bool fail_write = false, fail_active = false, redraw = false;
static constexpr int handle = 1, ESP_OK = 0;
struct Model {
  std::mutex mutex;
  std::vector<Entry> entries;
  std::vector<Source> sources;
  bool sync_pending = false;
} model;
static std::string error_text;

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
  return files[id].size() <= limit ? files[id] : "";
}
static bool write_file(const std::string &id, const std::string &body) {
  if (fail_write)
    return false;
  files[id] = body;
  return true;
}
static std::string get_string(const char *key) { return keys[key]; }
static int nvs_set_str(int, const char *key, const char *value) {
  if (fail_active && std::string(key) == "active")
    return -1;
  keys[key] = value;
  return ESP_OK;
}
static int nvs_commit(int) { return ESP_OK; }
static bool cache_commit_failure(const char *, int = 0, int = ESP_OK,
                                  size_t = 0) {
  return false;
}
#include "source_storage_under_test.hpp"

static bool publish_sources(std::vector<Entry> &old,
                            const std::vector<Source> &old_sources,
                            const std::vector<Source> &declared_sources,
                            const std::string &etag) {
#include "source_publication_under_test.hpp"
  return true;
}

int main_tests() {
  std::vector<Source> sources;
  Json manifest(cJSON_Parse(
      R"([{"id":"feishu","title":"飞书文档","count":449},{"id":"rss","title":"Public Atom","count":50},{"id":"empty","title":"","count":0}])"));
  assert(read_source_metadata(manifest.get(), sources));
  assert(sources.size() == 3 && sources[0].count == 449 &&
         sources[1].title == "Public Atom" && sources[2].count == 0);
  assert(sources[2].title == "RSS订阅");
  for (const char *invalid : {
           R"([{"id":"bad/id","title":"Bad","count":0}])",
           R"([{"id":"rss","title":"Bad","count":-1}])",
           R"([{"id":"rss","title":"Bad","count":1.5}])",
           R"([{"id":"rss","title":"Bad","count":2147483648}])",
           R"([{"id":"rss","title":"Bad"}])",
           R"([{"id":"rss","title":"One","count":0},{"id":"rss","title":"Two","count":0}])"}) {
    Json json(cJSON_Parse(invalid));
    auto preserved = sources;
    assert(!read_source_metadata(json.get(), sources));
    assert(sources == preserved);
  }

  // Old immutable indexes are read without rewriting their bytes or pointers.
  Json legacy(cJSON_Parse(
      R"({"schema_version":1,"etag":"old-etag","entries":[{"source_id":"feishu","document_id":"f","title":"Old","snapshot_id":""},{"source_id":"rss","document_id":"r","title":"RSS","snapshot_id":""}]})"));
  auto body = serialize(legacy.get()), legacy_id = sha256(body);
  files[legacy_id] = body;
  std::vector<Entry> entries;
  std::string etag;
  std::vector<Source> fallback;
  assert(load_index(legacy_id, entries, etag, fallback));
  assert(fallback.size() == 2 && fallback[0].title == "飞书文档" &&
         fallback[1].title == "RSS订阅" && fallback[1].count == 1);
  assert(etag.empty()); // Fetch metadata, including sources absent from entries.
  assert(files[legacy_id] == body);

  std::vector<Entry> old;
  for (int i = 0; i < 449; ++i)
    old.push_back({"feishu", "d" + std::to_string(i), "Old title", ""});
  std::vector<Source> old_sources{{"feishu", "飞书文档", 449}};
  assert(commit_cache(old, "old-etag", old_sources));
  const auto before = keys["active"];
  model.entries = old;
  model.sources = old_sources;
  // Publish manifest metadata while the RSS directory still has zero local rows.
  assert(publish_sources(old, old_sources, sources, "old-etag"));
  assert(model.sources == sources && model.entries.size() == 449);
  assert(model.sync_pending && redraw);
  assert(keys["previous"] == before && keys["active"] != before);
  std::vector<Entry> loaded;
  std::vector<Source> loaded_sources;
  assert(load_index(keys["active"], loaded, etag, loaded_sources));
  assert(etag == "old-etag" && loaded_sources == sources && loaded == old);
  assert(std::none_of(loaded.begin(), loaded.end(),
                      [](const auto &e) { return e.source_id == "rss"; }));

  // An explicit empty source list removes old rows and survives a restart.
  const auto with_sources = keys["active"];
  assert(publish_sources(old, sources, {}, "old-etag"));
  assert(model.sources.empty() && model.entries.empty() && old.empty());
  assert(load_index(keys["active"], loaded, etag, loaded_sources));
  assert(loaded.empty() && loaded_sources.empty());
  assert(keys["previous"] == with_sources);

  // Inconsistent declared metadata cannot silently invoke legacy inference.
  cJSON_AddArrayToObject(legacy.get(), "sources");
  body = serialize(legacy.get());
  files[sha256(body)] = body;
  assert(!load_index(sha256(body), loaded, etag, loaded_sources));

  // Failed metadata publication leaves the active index and visible model intact.
  const auto empty_active = keys["active"];
  fail_write = true;
  assert(!publish_sources(old, {}, sources, "old-etag"));
  assert(keys["active"] == empty_active && model.sources.empty());
  fail_write = false;
  fail_active = true;
  assert(!publish_sources(old, {}, sources, "old-etag"));
  assert(keys["active"] == empty_active && model.sources.empty());
  assert(load_index(keys["previous"], loaded, etag, loaded_sources));
  assert(loaded.empty() && loaded_sources.empty());
  return 0;
}
} // namespace rd

int main() { return rd::main_tests(); }
