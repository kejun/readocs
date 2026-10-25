#include "article_list.hpp"
#include "input_logic.hpp"
#include <cassert>
#include <string>
#include <vector>

struct Entry {
  std::string source_id, document_id;
};
struct Source {
  std::string id, title;
  int count;
};

int main() {
  static_assert(static_cast<int>(rd::View::Reading) == 0);
  static_assert(static_cast<int>(rd::View::Fonts) == 5);
  static_assert(static_cast<int>(rd::View::Sources) == 6);
  assert(std::string(rd::view_name(rd::View::Sources)) == "sources");
  std::vector<Source> sources{{"feishu", "飞书文档", 449},
                               {"rss", "订阅", 50},
                               {"pending", "新订阅", 50}};
  std::vector<Entry> all;
  for (int i = 0; i < 20; ++i)
    all.push_back({"feishu", std::to_string(i)});
  all.push_back({"rss", "same"});
  all.push_back({"feishu", "same"});
  all.push_back({"rss", "second"});
  auto feishu = rd::articles_for_source(all, "feishu");
  auto rss = rd::articles_for_source(all, "rss");
  assert(feishu.size() == 21 && rss.size() == 2);
  assert(rd::source_catalog(std::vector<Source>{}, all).empty());
  auto legacy = rd::source_catalog(std::vector<Source>{}, all, true);
  assert(legacy.size() == 2 && legacy[0].title == "飞书文档" &&
         legacy[0].count == 21 && legacy[1].title == "RSS订阅");

  rd::SourceBrowser browser;
  browser.reconcile(sources);
  assert(browser.source() == "feishu" && browser.index() == 0);
  browser.cursor().focus(feishu, "feishu", "12");
  browser.move(sources, 1);
  browser.cursor().focus(rss, "rss", "same");
  assert(rss[browser.cursor().index()].source_id == "rss");
  browser.cursor().next(rss);
  assert(browser.cursor().document() == "second");
  browser.move(sources, -1);
  assert(browser.cursor().document() == "12");
  browser.cursor().next_page(feishu);
  assert(browser.cursor().index() == 16);
  browser.move(sources, 1);
  assert(browser.cursor().document() == "second");

  rd::SourceSelection retained[rd::retained_source_count]{};
  browser.retain(retained, rd::retained_source_count);
  rd::SourceBrowser wake;
  wake.restore(retained, rd::retained_source_count, all);
  wake.reconcile(sources);
  assert(wake.focus(sources, "rss") && wake.cursor().document() == "second");
  assert(wake.focus(sources, "feishu") && wake.cursor().document() == "16");
  // Source reordering and renaming preserve identity and its own article.
  std::swap(sources[0], sources[1]);
  sources[1].title = "已改名的飞书文档";
  assert(wake.reconcile(sources) && wake.source() == "feishu" && wake.index() == 1);
  assert(wake.cursor().document() == "16");
  feishu.insert(feishu.begin(), {"feishu", "new"});
  wake.cursor().reconcile(feishu);
  assert(wake.cursor().document() == "16" && wake.cursor().index() == 17);
  feishu.erase(feishu.begin() + 17);
  wake.cursor().reconcile(feishu);
  assert(wake.cursor().document() == "17");
  assert(feishu[wake.cursor().index()].source_id == "feishu");

  // A declared source can be entered before its directory has arrived. Its
  // identity must remain observable even when its article cursor is empty.
  assert(wake.focus(sources, "pending"));
  wake.cursor().reconcile(rd::articles_for_source(all, "pending"));
  assert(wake.source() == "pending" && wake.cursor().document().empty());
  wake.next_page(sources);
  assert(wake.index() == 0);
  wake.move(sources, -1);
  assert(wake.source() == "pending");

  // Removal does not alias the old source's cursor to a different source.
  sources.pop_back();
  assert(!wake.reconcile(sources));
  assert(!wake.focus(sources, "pending"));
  assert(wake.source() != "pending");
  // Two old B confirmations accumulated during a waveform must not first
  // enter the replacement source and then open one of its articles.
  int opened_sources = 0;
  for (int queued_b = 0; queued_b < 2; ++queued_b)
    if (wake.can_open_source())
      ++opened_sources;
  assert(opened_sources == 0);
  wake.source_rendered();
  assert(!wake.can_open_source());
  // Input sampling continues during the source-list waveform. Its queued B
  // events are still fenced after rendering, and a release can arrive later.
  std::vector<rd::Key> during_waveform{rd::Key::Previous, rd::Key::Previous};
  for (const auto event : during_waveform) {
    assert(event == rd::Key::Previous && wake.source_input_fenced());
    wake.source_input_activity();
  }
  during_waveform.clear();
  rd::InputLogic pending_release;
  auto emit = [&](rd::Key key) { during_waveform.push_back(key); };
  pending_release.update(2, 0, emit);
  pending_release.update(2, 25, emit);
  pending_release.update(0, 35, emit);
  assert(!pending_release.idle());
  wake.settle_source_input(pending_release.idle(), true, true, 50);
  assert(!wake.can_open_source());
  pending_release.update(0, 60, emit);
  assert(pending_release.idle() && during_waveform.size() == 1);
  wake.settle_source_input(true, true, false, 60);
  assert(!wake.can_open_source());
  during_waveform.clear();
  wake.source_input_activity();
  wake.settle_source_input(true, true, true, 60);
  wake.settle_source_input(true, true, true, 109);
  assert(!wake.can_open_source());
  // Any new physical press or newly queued event resets the stable interval.
  wake.settle_source_input(true, false, true, 110);
  wake.settle_source_input(true, true, true, 120);
  wake.settle_source_input(true, true, false, 160);
  wake.settle_source_input(true, true, true, 170);
  wake.settle_source_input(true, true, true, 219);
  assert(!wake.can_open_source());
  wake.settle_source_input(true, true, true, 220);
  assert(wake.can_open_source());
  // Normal source browsing never enters this fence or discards valid input.
  assert(browser.can_open_source());
  browser.source_rendered();
  browser.settle_source_input(false, false, false, 0);
  assert(browser.can_open_source());
  sources.clear();
  assert(!wake.reconcile(sources) && wake.source().empty());
  wake.cursor().reconcile(std::vector<Entry>{});
  assert(wake.cursor().document().empty());

  std::vector<Source> many;
  for (int i = 0; i < 10; ++i)
    many.push_back({std::to_string(i), "source", 1});
  wake.reconcile(many);
  wake.next_page(many);
  assert(wake.index() == 8);
  wake.next_page(many);
  assert(wake.index() == 0);
}
