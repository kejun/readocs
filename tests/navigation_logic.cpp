#include "article_list.hpp"
#include "cache_merge.hpp"
#include <cassert>
#include <string>
#include <vector>
struct Entry {
  std::string source_id, document_id;
};
int main() {
  {
    std::vector<Entry> list{{"feishu", "0"}, {"feishu", "1"}, {"feishu", "2"}};
    rd::ArticleCursor cursor;
    cursor.focus(list, "feishu", "0");
    cursor.previous(list);
    assert(cursor.index() == 2);
    cursor.previous(list);
    assert(cursor.index() == 1);
    cursor.next(list);
    assert(cursor.index() == 2);
    cursor.next(list);
    assert(cursor.index() == 0);
  }
  rd::ArticleCursor cursor;
  std::vector<Entry> entries;
  cursor.next(entries);
  cursor.next_page(entries);
  assert(cursor.index() == 0 && cursor.document().empty());
  for (int i = 0; i < 459; ++i)
    entries.push_back({"feishu", std::to_string(i)});
  cursor.focus(entries, "feishu", "320");
  assert(cursor.index() == 320);
  cursor.next_page(entries);
  assert(cursor.index() == 328);
  cursor.focus(entries, "feishu", "458");
  cursor.next_page(entries);
  assert(cursor.index() == 0);
  cursor.focus(entries, "feishu", "458");
  cursor.next(entries);
  assert(cursor.index() == 0);
  cursor.focus(entries, "feishu", "320");
  entries.insert(entries.begin(), {"rss", "320"});
  cursor.reconcile(entries);
  assert(cursor.index() == 321 && cursor.source() == "feishu");
  // Waking from sleep or returning from an article restores its identity.
  rd::ArticleCursor restored;
  restored.focus(entries, cursor.source(), cursor.document());
  assert(restored.index() == 321);
  entries.erase(entries.begin() + 321);
  restored.reconcile(entries);
  assert(restored.document() == "321");
  entries.resize(2);
  restored.reconcile(entries);
  assert(restored.index() == 1);
  entries.clear();
  restored.reconcile(entries);
  assert(restored.index() == 0 && restored.document().empty());
  struct CachedEntry {
    std::string source_id, document_id, snapshot_id;
  };
  std::vector<CachedEntry> staged{{"feishu", "same", "stale"},
                                  {"rss", "same", ""},
                                  {"feishu", "downloaded-in-stage", "kept"}};
  std::vector<CachedEntry> active{{"feishu", "same", "new-download"},
                                  {"rss", "same", "feed-body"},
                                  {"feishu", "downloaded-in-stage", ""}};
  rd::merge_cached_entries(staged, active);
  assert(staged[0].snapshot_id == "new-download");
  assert(staged[1].snapshot_id == "feed-body");
  assert(staged[2].snapshot_id == "kept");
}
