#pragma once
#include <algorithm>
#include <cJSON.h>
#include <cmath>
#include <limits>
#include <set>
#include <string>
#include <utility>

namespace rd {
inline bool valid_source_id(const std::string &id) {
  return !id.empty() && id.size() <= 128 &&
         id.find_first_not_of(
             "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") ==
             std::string::npos;
}

inline std::string fallback_source_title(const std::string &id) {
  return id == "feishu" ? "飞书文档" : "RSS订阅";
}

// Sources are independently useful while their paginated directories are still
// downloading. A declared empty array is authoritative, not a legacy fallback.
template <typename Sources>
bool read_source_metadata(const cJSON *array, Sources &out) {
  if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) > 64)
    return false;
  Sources result;
  std::set<std::string> ids;
  const cJSON *item;
  cJSON_ArrayForEach(item, array) {
    const auto *id = cJSON_GetObjectItemCaseSensitive(item, "id");
    const auto *title = cJSON_GetObjectItemCaseSensitive(item, "title");
    const auto *count = cJSON_GetObjectItemCaseSensitive(item, "count");
    if (!cJSON_IsObject(item) || !cJSON_IsString(id) ||
        !valid_source_id(id->valuestring) || !ids.insert(id->valuestring).second ||
        !cJSON_IsString(title) || std::string(title->valuestring).size() > 1024 ||
        !cJSON_IsNumber(count) || !std::isfinite(count->valuedouble) ||
        count->valuedouble < 0 ||
        count->valuedouble > std::numeric_limits<int>::max() ||
        std::floor(count->valuedouble) != count->valuedouble)
      return false;
    std::string name = title->valuestring;
    result.push_back({id->valuestring,
                      name.empty() ? fallback_source_title(id->valuestring) : name,
                      count->valueint});
  }
  out = std::move(result);
  return true;
}

template <typename Sources>
void write_source_metadata(cJSON *root, const Sources &sources) {
  auto *array = cJSON_AddArrayToObject(root, "sources");
  for (const auto &source : sources) {
    auto *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "id", source.id.c_str());
    cJSON_AddStringToObject(item, "title", source.title.c_str());
    cJSON_AddNumberToObject(item, "count", source.count);
    cJSON_AddItemToArray(array, item);
  }
}

template <typename Entries, typename Sources>
bool infer_source_metadata(const Entries &entries, Sources &out) {
  Sources result;
  for (const auto &entry : entries) {
    if (!valid_source_id(entry.source_id))
      return false;
    auto found = std::find_if(result.begin(), result.end(), [&](const auto &s) {
      return s.id == entry.source_id;
    });
    if (found == result.end())
      result.push_back({entry.source_id, fallback_source_title(entry.source_id), 1});
    else
      ++found->count;
  }
  out = std::move(result);
  return true;
}

template <typename Entries, typename Sources>
bool source_entries_valid(const Entries &entries, const Sources &sources) {
  return std::all_of(entries.begin(), entries.end(), [&](const auto &entry) {
    return std::any_of(sources.begin(), sources.end(), [&](const auto &source) {
      return source.id == entry.source_id;
    });
  });
}

template <typename Entries, typename Sources>
void retain_source_entries(Entries &entries, const Sources &sources) {
  entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const auto &e) {
                  return std::none_of(sources.begin(), sources.end(),
                                      [&](const auto &s) {
                                        return s.id == e.source_id;
                                      });
                }),
                entries.end());
}
} // namespace rd
