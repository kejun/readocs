#include <algorithm>
#include <cassert>
#include <cJSON.h>
#include <functional>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace rd {
#include "download_types.hpp"
struct Config { std::string token = "test-device-token"; };
struct Model {
  std::mutex mutex;
  std::vector<Entry> entries;
  std::vector<Source> sources;
  std::string etag;
} model;
static std::string error_text, visible_status, body;
static std::vector<std::string> stages, parts;
static std::map<std::string, std::string> files;
static Json reference;
static int failed_chunk, short_chunk, corrupt_chunk, reference_mode;
static unsigned chunk_requests;
static bool fail_write, fail_commit, fail_reference;

static void stage_log(const char *, const char *, const char *stage, int) {
  stages.push_back(stage);
}
#define ESP_LOGW stage_log
static std::string sha256(const std::string &text) {
  std::ostringstream hash;
  hash << std::hex << std::setfill('0') << std::setw(64)
       << std::hash<std::string>{}(text);
  return hash.str();
}
static bool valid_hash(const std::string &id) {
  return id.size() == 64 &&
         id.find_first_not_of("0123456789abcdef") == std::string::npos;
}
static std::string read_file(const std::string &id) { return files[id]; }
static bool write_file(const std::string &id, const std::string &text) {
  if (fail_write)
    return false;
  files[id] = text;
  return true;
}
static Config config_copy() { return {}; }
static bool ensure_wifi() { return true; }
static bool ensure_time() { return true; }
static void prepare_download_space() {}
static std::pair<std::string, std::string> current_article() {
  return {"rss", "document"};
}
static void trim_cached_articles(std::vector<Entry> &, const std::string &,
                                 const std::string &, const std::string &,
                                 const std::string &) {}
static bool commit_cache(const std::vector<Entry> &, const std::string &,
                          const std::vector<Source> &) { return !fail_commit; }
static void collect_cache() {}
static void set_status(const std::string &status) { visible_status = status; }

static int request(const Config &, const std::string &path, Http &out) {
  if (path.find("/documents/") != std::string::npos) {
    if (fail_reference) {
      error_text = "原始引用请求错误";
      return -1;
    }
    Json item(cJSON_CreateObject());
    cJSON_AddStringToObject(item.get(), "document_id", "document");
    if (reference_mode == 1)
      cJSON_AddNullToObject(item.get(), "snapshot");
    else if (reference_mode == 2) {
      // A missing field is malformed, not an upstream body still being fetched.
    } else if (reference_mode == 3)
      cJSON_AddStringToObject(item.get(), "snapshot", "invalid");
    else
      cJSON_AddItemToObject(item.get(), "snapshot", cJSON_Duplicate(reference.get(), true));
    out.body = serialize(item.get());
    return 200;
  }
  const auto index = std::stoi(path.substr(path.find_last_of('/') + 1));
  ++chunk_requests;
  if (index == failed_chunk) {
    error_text = "原始分块请求错误";
    return -1;
  }
  out.body = parts.at(index);
  if (index == short_chunk)
    out.body.pop_back();
  if (index == corrupt_chunk)
    out.body[0] ^= 1;
  return 200;
}
#include "download_under_test.hpp"

static Json document() {
  Json root(cJSON_CreateObject());
  cJSON_AddNumberToObject(root.get(), "schema_version", 1);
  cJSON_AddStringToObject(root.get(), "document_id", "document");
  auto *blocks = cJSON_AddArrayToObject(root.get(), "blocks");
  for (int i = 0; i < 233; ++i) {
    auto *block = cJSON_CreateObject();
    cJSON_AddStringToObject(block, "block_id", ("block" + std::to_string(i)).c_str());
    cJSON_AddStringToObject(block, "type", "paragraph");
    cJSON_AddStringToObject(block, "text", std::string(240, 'r').c_str());
    cJSON_AddItemToArray(blocks, block);
  }
  return root;
}
static void set_body(cJSON *root) {
  body = serialize(root);
  parts.clear();
  reference.reset(cJSON_CreateObject());
  const auto id = sha256(body);
  cJSON_AddStringToObject(reference.get(), "snapshot_id", id.c_str());
  cJSON_AddStringToObject(reference.get(), "sha256", id.c_str());
  cJSON_AddNumberToObject(reference.get(), "bytes", body.size());
  auto *chunks = cJSON_AddArrayToObject(reference.get(), "chunks");
  for (size_t offset = 0; offset < body.size(); offset += 32768) {
    auto part = body.substr(offset, 32768);
    parts.push_back(part);
    auto *chunk = cJSON_CreateObject();
    cJSON_AddNumberToObject(chunk, "index", parts.size() - 1);
    cJSON_AddNumberToObject(chunk, "bytes", part.size());
    cJSON_AddStringToObject(chunk, "sha256", sha256(part).c_str());
    cJSON_AddItemToArray(chunks, chunk);
  }
}
static void reset() {
  error_text.clear(); visible_status.clear(); stages.clear(); files.clear();
  failed_chunk = short_chunk = corrupt_chunk = -1;
  reference_mode = 0; chunk_requests = 0;
  fail_write = fail_commit = fail_reference = false;
  model.entries = {{"rss", "document", "Article", ""}};
  model.sources = {{"rss", "RSS", 1}};
  model.etag = "existing-etag";
  auto root = document();
  set_body(root.get());
}
static bool open_article() {
  Json input(cJSON_CreateObject());
  cJSON_AddStringToObject(input.get(), "source_id", "rss");
  cJSON_AddStringToObject(input.get(), "document_id", "document");
  return download_article(input.get());
}
int main_tests() {
  reset();
  assert(parts.size() == 3);
  assert(open_article());
  assert(chunk_requests == 3 && model.entries[0].snapshot_id == sha256(body));
  assert(files[sha256(body)] == body && visible_status == "正文已下载");

  reset();
  auto *chunks = cJSON_GetObjectItemCaseSensitive(reference.get(), "chunks");
  cJSON_DeleteItemFromArray(chunks, 1);
  assert(!open_article() && stages.back() == "chunk_list");
  assert(chunk_requests == 0 && model.entries[0].snapshot_id.empty());

  reset(); short_chunk = 0;
  assert(!open_article() && stages.back() == "chunk_size");
  assert(error_text.find("长度不符") != std::string::npos);
  reset(); corrupt_chunk = 1;
  assert(!open_article() && stages.back() == "chunk_hash");
  assert(error_text.find("分块校验失败") != std::string::npos);

  reset();
  cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(reference.get(), "snapshot_id"), std::string(64, 'e').c_str());
  cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(reference.get(), "sha256"), std::string(64, 'e').c_str());
  assert(!open_article() && stages.back() == "body_hash");

  reset();
  auto root = document();
  auto *blocks = cJSON_GetObjectItemCaseSensitive(root.get(), "blocks");
  cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(blocks, 1), "block_id"), "block0");
  set_body(root.get());
  assert(!open_article() && stages.back() == "structure");
  reset(); root = document();
  cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(root.get(), "schema_version"), 2);
  set_body(root.get());
  assert(!open_article() && stages.back() == "schema");

  reset(); fail_write = true;
  assert(!open_article() && stages.back() == "body_write");
  assert(error_text.find("正文缓存写入失败") != std::string::npos);
  assert(model.entries[0].snapshot_id.empty());
  reset(); fail_commit = true;
  assert(!open_article() && stages.back() == "cache_commit");
  assert(model.entries[0].snapshot_id.empty());

  reset(); fail_reference = true;
  assert(!open_article() && stages.back() == "reference_request");
  assert(error_text == "原始引用请求错误");
  reset(); failed_chunk = 1;
  assert(!open_article() && stages.back() == "chunk_request");
  assert(error_text == "原始分块请求错误");

  reset(); reference_mode = 1;
  assert(!open_article() && stages.back() == "not_ready");
  assert(error_text == "正文尚未同步，请稍后重试" && chunk_requests == 0);
  for (int mode : {2, 3}) {
    reset(); reference_mode = mode;
    assert(!open_article() && stages.back() == "reference_json");
    assert(error_text != "正文尚未同步，请稍后重试");
  }
  return 0;
}
} // namespace rd
int main() { return rd::main_tests(); }
