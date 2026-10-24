#pragma once
#include <algorithm>
#include <string>

namespace rd {
enum class View { Reading, Menu, Articles, Status, Pair, Fonts };
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
} // namespace rd
