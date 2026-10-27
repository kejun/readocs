import shutil
import subprocess
from pathlib import Path


def test_actual_ui_catalog_resumes_download_during_next_network_action(tmp_path):
    root = Path(__file__).resolve().parents[1]
    source = (root / "firmware/device/main/main.cpp").read_text()
    # Consume exactly once, before reading the shared model. A publication later
    # in the loop must remain set until the next model snapshot.
    assert source.count("redraw.exchange(false)") == 1
    changed = "const bool changed = redraw.exchange(false);"
    assert source.index(changed) < source.index("if ((changed || all_entries.empty()")
    catalog = (
        "std::string status, ssid, ap_ssid, ap_password;"
        + source.split("std::string status, ssid, ap_ssid, ap_password;", 1)[1].split(
            "    if (ap != ap_was)", 1
        )[0]
    )
    repaint = (
        "if (changed && (view == View::Status ||"
        + source.split("if (changed && (view == View::Status ||", 1)[1].split(
            "    if (view == View::Reading && !current.document_id.empty()", 1
        )[0]
    )
    prefix = r"""
#include "article_list.hpp"
#include "input_logic.hpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <mutex>
#include <string>
#include <vector>
using rd::View;
struct Entry {
  std::string source_id, document_id, snapshot_id;
  bool operator==(const Entry &other) const {
    return source_id == other.source_id && document_id == other.document_id &&
           snapshot_id == other.snapshot_id;
  }
};
struct Source {
  std::string id;
  bool operator==(const Source &other) const { return id == other.id; }
};
struct Config {};
struct Model {
  std::mutex mutex;
  std::vector<Entry> entries;
  std::vector<Source> sources;
  std::string status, ssid, ap_ssid, ap_password;
  bool ap = false, sync_pending = false, check_notice = false, last_network_ok = true;
  Config config;
} model;
int main() {
  rd::SourceBrowser browser;
  std::vector<Entry> all_entries;
  std::vector<Source> sources{{"rss"}, {"feishu"}};
  model.sources = sources;
  browser.focus(sources, "rss");
  View view = View::Reading;
  std::vector<int> pages;
  Entry current{"rss", "same", ""};
  std::atomic<bool> networking{true}, redraw{false};
  bool previous_busy = true, need_draw = false;
  int opens = 0, refreshes = 0;
  auto refresh_source_articles = [&] { ++refreshes; };
  auto open_entry = [&](const Entry &entry) {
    ++opens;
    assert(entry.source_id == "rss" && entry.document_id == "same");
    current = entry;
    pages = {1};
  };
  auto ui_tick = [&](bool publish_after_snapshot) {
"""
    middle = r"""
    (void)sync_pending;
    (void)network_ok;
    if (publish_after_snapshot) {
      model.entries[1].snapshot_id = "downloaded";
      redraw = true;
      // No idle interval is observed: the next queued sync already started.
      networking = true;
    }
"""
    suffix = r"""
    previous_busy = networking;
  };
  // A different source with the same document ID must never be opened.
  all_entries = {{"feishu", "same", "other-body"}, {"rss", "same", ""}};
  model.entries = all_entries;
  redraw = true;
  ui_tick(false);
  assert(opens == 0 && pages.empty());
  need_draw = false;
  // Download commits after the model snapshot. This loop cannot consume the
  // new redraw while retaining its old directory copy.
  ui_tick(true);
  assert(redraw && all_entries[1].snapshot_id.empty() && opens == 0);
  ui_tick(false);
  assert(!redraw && networking && previous_busy && opens == 1);
  assert(!pages.empty() && current.snapshot_id == "downloaded" && need_draw);
  // An already displayed article keeps its existing 30-second update policy.
  need_draw = false;
  model.entries[1].snapshot_id = "later-version";
  redraw = true;
  ui_tick(false);
  assert(opens == 1 && current.snapshot_id == "downloaded" && !need_draw);
  // A completed body already present at the start of a loop also resumes
  // immediately while the next network request is active.
  pages.clear();
  current.snapshot_id.clear();
  model.entries[1].snapshot_id = "third-version";
  redraw = true;
  ui_tick(false);
  assert(opens == 2 && current.snapshot_id == "third-version" && networking);
  assert(refreshes >= 3);
  view=View::Sources;need_draw=false;redraw=true;
  ui_tick(false);assert(!need_draw); // Passive checks do not expose a footer.
  model.check_notice=true;redraw=true;
  ui_tick(false);assert(need_draw); // Manual checks paint their current status.
}
"""
    cpp = tmp_path / "download-resume.cpp"
    cpp.write_text(prefix + changed + "\n" + catalog + middle + repaint + suffix)
    compiler = shutil.which("clang++") or shutil.which("g++")
    assert compiler, "A host C++ compiler is required"
    binary = tmp_path / "download-resume"
    subprocess.run(
        [
            compiler,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(root / "firmware/device/main"),
            str(cpp),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
