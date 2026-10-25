import shutil
import subprocess
from pathlib import Path


def test_actual_source_backlog_and_check_actions(tmp_path):
    root = Path(__file__).resolve().parents[1]
    source = (root / "firmware/device/main/main.cpp").read_text()
    backlog = (
        "auto discard_invalid_source_backlog = [&] {"
        + source.split("auto discard_invalid_source_backlog = [&] {", 1)[1].split(
            "\n  };", 1
        )[0]
        + "\n};\n"
    )
    check = (
        "if (event == Key::Check) {"
        + source.split("if (event == Key::Check) {", 1)[1].split(
            "\n      if (view == View::Reading)", 1
        )[0]
    )
    prefix = r"""
#include "article_list.hpp"
#include "input_logic.hpp"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>
using rd::Key;
using rd::View;
struct Entry { std::string source_id, document_id; };
static std::deque<Key> queued_keys;
static int64_t tick = 50000;
static int64_t now_ms() { return tick; }
static constexpr int pdTRUE = 1, buttons = 0;
static int xQueueReceive(int, Key *key, int) {
  if (queued_keys.empty()) return 0;
  *key = queued_keys.front(); queued_keys.pop_front(); return pdTRUE;
}
int main() {
  rd::SourceBrowser browser;
  std::atomic<int64_t> last_input{0};
  bool deferred_pair_confirm = false;
"""
    middle = r"""
  queued_keys = {Key::Previous, Key::PairConfirm, Key::Next};
  discard_invalid_source_backlog();
  assert(queued_keys.empty() && deferred_pair_confirm);
  assert(last_input == tick && tick - last_input < 1000);
  std::vector<Entry> entries;
  for (int i = 0; i < 20; ++i)
    entries.push_back({"feishu", std::to_string(i)});
  browser.cursor().focus(entries, "feishu", "0");
  int checks = 0;
  int64_t last_check = 0;
  bool need_draw = false;
  auto enqueue = [&](const std::string &kind) {
    assert(kind == "check"); ++checks;
  };
  auto press_check = [&](View view, int64_t at) {
    tick = at;
    for (int once = 0; once < 1; ++once) {
      Key event = Key::Check;
"""
    suffix = r"""
    }
  };
  press_check(View::Sources, 50000);
  assert(checks == 1 && browser.cursor().index() == 0 && !need_draw);
  press_check(View::Sources, 50010);
  assert(checks == 1);
  press_check(View::Sources, 80000);
  assert(checks == 2);
  press_check(View::Articles, 110000);
  assert(checks == 2 && browser.cursor().index() == 8 && need_draw);
  press_check(View::Reading, 110000);
  assert(checks == 3);
}
"""
    cpp = tmp_path / "source-actions.cpp"
    cpp.write_text(prefix + backlog + middle + check + suffix)
    compiler = shutil.which("clang++") or shutil.which("g++")
    assert compiler, "A host C++ compiler is required for firmware logic tests"
    binary = tmp_path / "source-actions"
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
