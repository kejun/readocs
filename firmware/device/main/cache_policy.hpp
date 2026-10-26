#pragma once
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace rd {
// Directory order determines the offline prefetch set. A selected article is
// also retained even when it is outside that set.
template <typename Entries>
std::vector<bool> cached_article_mask(const Entries &entries,
                                      const std::string &source,
                                      const std::string &document,
                                      const std::string &pending_source = "",
                                      const std::string &pending_document = "",
                                      bool all_articles = false) {
  std::map<std::string, unsigned> counts;
  std::vector<bool> keep;
  keep.reserve(entries.size());
  for (const auto &entry : entries)
    keep.push_back(all_articles || counts[entry.source_id]++ < 5 ||
                   (entry.source_id == source && entry.document_id == document) ||
                   (entry.source_id == pending_source &&
                    entry.document_id == pending_document));
  return keep;
}

template <typename Entries>
void trim_cached_articles(Entries &entries, const std::string &source,
                          const std::string &document,
                          const std::string &pending_source = "",
                          const std::string &pending_document = "",
                                      bool all_articles = false) {
  const auto keep = cached_article_mask(entries, source, document,
                                         pending_source, pending_document, all_articles);
  for (size_t i = 0; i < entries.size(); ++i)
    if (!keep[i])
      entries[i].snapshot_id.clear();
}

template <typename Entries, typename Commit, typename ClearProgress,
          typename Reclaim>
bool compact_cache_for_download(Entries &entries, const std::string &source,
                                 const std::string &document, Commit commit,
                                 ClearProgress clear_progress, Reclaim reclaim,
                                 bool all_articles = false) {
  auto compact = entries;
  trim_cached_articles(compact, source, document, "", "", all_articles);
  if (compact == entries)
    return true;
  if (!commit(compact))
    return false;
  entries = std::move(compact);
  // Checkpoints may still reference the pre-quota bodies. Retire them only
  // after the complete reader index has been committed successfully.
  clear_progress();
  reclaim();
  return true;
}
} // namespace rd
