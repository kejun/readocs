#pragma once
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace rd {
enum class View { Reading, Menu, Articles, Status, Pair, Fonts, Sources };
inline const char *view_name(View view) {
  switch (view) {
  case View::Reading:
    return "reading";
  case View::Menu:
    return "menu";
  case View::Articles:
    return "articles";
  case View::Status:
    return "status";
  case View::Pair:
    return "pair";
  case View::Fonts:
    return "fonts";
  case View::Sources:
    return "sources";
  }
  return "articles";
}
inline constexpr int article_page_size = 8;

// Track the highlighted article by identity, so background reordering or
// removal cannot silently make B open a different article.
class ArticleCursor {
  int index_ = 0;
  std::string source_, document_;

  template <typename Entries> void remember(const Entries &entries) {
    if (entries.empty()) {
      index_ = 0;
      source_.clear();
      document_.clear();
    } else {
      index_ = std::clamp(index_, 0, static_cast<int>(entries.size()) - 1);
      source_ = entries[index_].source_id;
      document_ = entries[index_].document_id;
    }
  }

public:
  int index() const { return index_; }
  const std::string &source() const { return source_; }
  const std::string &document() const { return document_; }
  template <typename Entries> void reconcile(const Entries &entries) {
    for (int i = 0; i < static_cast<int>(entries.size()); ++i)
      if (entries[i].source_id == source_ &&
          entries[i].document_id == document_) {
        index_ = i;
        break;
      }
    remember(entries);
  }
  template <typename Entries>
  void focus(const Entries &entries, const std::string &source,
             const std::string &document) {
    index_ = 0;
    source_ = source;
    document_ = document;
    reconcile(entries);
  }
  template <typename Entries> void next(const Entries &entries) {
    reconcile(entries);
    if (!entries.empty())
      index_ = (index_ + 1) % entries.size();
    remember(entries);
  }
  template <typename Entries> void previous(const Entries &entries) {
    reconcile(entries);
    if (!entries.empty())
      index_ = (index_ + static_cast<int>(entries.size()) - 1) %
               static_cast<int>(entries.size());
    remember(entries);
  }
  template <typename Entries> void next_page(const Entries &entries) {
    reconcile(entries);
    index_ = (index_ / article_page_size + 1) * article_page_size;
    if (index_ >= static_cast<int>(entries.size()))
      index_ = 0;
    remember(entries);
  }
};

template <typename Sources, typename Entries>
Sources source_catalog(const Sources &declared, const Entries &entries,
                        bool legacy = false) {
  if (!legacy || !declared.empty())
    return declared;
  Sources sources;
  for (const auto &entry : entries) {
    auto source = std::find_if(sources.begin(), sources.end(), [&](const auto &s) {
      return s.id == entry.source_id;
    });
    if (source == sources.end())
      sources.push_back({entry.source_id,
                         entry.source_id == "feishu" ? "飞书文档" : "RSS订阅", 1});
    else
      ++source->count;
  }
  return sources;
}

template <typename Entries>
Entries articles_for_source(const Entries &entries, const std::string &source) {
  Entries selected;
  for (const auto &entry : entries)
    if (entry.source_id == source)
      selected.push_back(entry);
  return selected;
}

struct SourceSelection {
  char source[129]{}, document[129]{};
};
inline constexpr size_t retained_source_count = 8;

class SourceBrowser {
  std::string source_;
  int index_ = 0;
  std::map<std::string, ArticleCursor> cursors_;
  bool source_ready_ = true;
  bool source_page_rendered_ = false;
  int64_t source_released_at_ = -1;

public:
  const std::string &source() const { return source_; }
  int index() const { return index_; }
  bool can_open_source() const { return source_ready_ && !source_.empty(); }
  bool source_input_fenced() const { return !source_ready_; }
  void source_input_activity() { source_released_at_ = -1; }
  void source_rendered() {
    if (!source_ready_) {
      source_page_rendered_ = true;
      source_released_at_ = -1;
    }
  }
  void settle_source_input(bool idle, bool released, bool queue_empty,
                            int64_t now) {
    if (source_ready_)
      return;
    if (!source_page_rendered_ || !idle || !released || !queue_empty) {
      source_released_at_ = -1;
      return;
    }
    if (source_released_at_ < 0)
      source_released_at_ = now;
    else if (now - source_released_at_ >= 50)
      source_ready_ = true;
  }
  ArticleCursor &cursor() { return cursors_[source_]; }
  template <typename Sources> bool reconcile(const Sources &sources) {
    auto found = std::find_if(sources.begin(), sources.end(), [&](const auto &s) {
      return s.id == source_;
    });
    const bool preserved = found != sources.end();
    if (!preserved && !source_.empty()) {
      source_ready_ = false;
      source_page_rendered_ = false;
      source_released_at_ = -1;
    }
    if (preserved)
      index_ = static_cast<int>(found - sources.begin());
    else if (sources.empty()) {
      source_.clear();
      index_ = 0;
    } else {
      index_ = std::clamp(index_, 0, static_cast<int>(sources.size()) - 1);
      source_ = sources[index_].id;
    }
    for (auto it = cursors_.begin(); it != cursors_.end();) {
      if (std::none_of(sources.begin(), sources.end(), [&](const auto &s) {
            return s.id == it->first;
          }))
        it = cursors_.erase(it);
      else
        ++it;
    }
    return preserved;
  }
  template <typename Sources>
  bool focus(const Sources &sources, const std::string &source) {
    if (std::none_of(sources.begin(), sources.end(), [&](const auto &s) {
          return s.id == source;
        }))
      return false;
    source_ = source;
    reconcile(sources);
    return true;
  }
  template <typename Sources> void move(const Sources &sources, int delta) {
    reconcile(sources);
    if (sources.empty())
      return;
    const int count = static_cast<int>(sources.size());
    index_ = (index_ + delta % count + count) % count;
    source_ = sources[index_].id;
  }
  template <typename Sources> void next_page(const Sources &sources) {
    reconcile(sources);
    if (sources.empty())
      return;
    index_ = (index_ / article_page_size + 1) * article_page_size;
    if (index_ >= static_cast<int>(sources.size()))
      index_ = 0;
    source_ = sources[index_].id;
  }
  template <typename Entries>
  void restore(const SourceSelection *saved, size_t count, const Entries &all) {
    for (size_t i = 0; i < count; ++i) {
      // RTC arrays are bounded even when retained bytes come from older code.
      if (!saved[i].source[0] || saved[i].source[128] || saved[i].document[128])
        continue;
      const std::string source = saved[i].source;
      cursors_[source].focus(articles_for_source(all, source), source,
                              saved[i].document);
    }
  }
  void retain(SourceSelection *saved, size_t count) const {
    for (size_t i = 0; i < count; ++i)
      saved[i] = {};
    size_t i = 0;
    for (const auto &item : cursors_) {
      if (i >= count)
        break;
      std::snprintf(saved[i].source, sizeof(saved[i].source), "%s", item.first.c_str());
      std::snprintf(saved[i].document, sizeof(saved[i].document), "%s",
                    item.second.document().c_str());
      ++i;
    }
  }
};
} // namespace rd
