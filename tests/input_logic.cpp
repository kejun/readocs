#include "input_logic.hpp"
#include <cassert>
#include <vector>
using rd::InputLogic;
using rd::Key;
int main() {
  std::vector<Key> events;
  auto emit = [&](Key k) { events.push_back(k); };
  {
    InputLogic b;
    b.update(1, 0, emit);
    b.update(0, 10, emit);
    b.update(0, 40, emit);
    assert(events.empty());
  }
  {
    InputLogic b;
    b.update(1, 0, emit);
    b.update(1, 25, emit);
    b.update(0, 40, emit);
    b.update(0, 65, emit);
    assert(events == std::vector<Key>{Key::Next});
    events.clear();
  }
  {
    InputLogic b;
    b.update(1, 0, emit);
    b.update(1, 25, emit);
    b.update(1, 1024, emit);
    assert(events.empty());
    b.update(1, 1025, emit);
    b.update(1, 2000, emit);
    b.update(0, 2010, emit);
    b.update(0, 2035, emit);
    assert(events == std::vector<Key>{Key::Check});
    events.clear();
  }
  {
    InputLogic b;
    b.update(2, 0, emit);
    b.update(2, 25, emit);
    b.update(2, 1025, emit);
    b.update(2, 2000, emit);
    b.update(0, 2010, emit);
    b.update(0, 2035, emit);
    // Returning to the list with held B must not open an article on release.
    assert(events == std::vector<Key>{Key::Menu});
    events.clear();
  }
  {
    InputLogic b;
    b.update(3, 0, emit);
    b.update(3, 25, emit);
    b.update(0, 150, emit);
    b.update(0, 175, emit);
    assert(events == std::vector<Key>{Key::PreviousItem});
    events.clear();
  }
  {
    InputLogic b;
    b.update(3, 0, emit);
    b.update(3, 25, emit);
    b.update(3, 3024, emit);
    assert(events.empty());
    b.update(3, 3025, emit);
    b.update(3, 4000, emit);
    b.update(0, 4010, emit);
    b.update(0, 4035, emit);
    assert(events == std::vector<Key>{Key::PairConfirm});
    events.clear();
  }
  {
    InputLogic b;
    b.update(3, 0, emit);
    b.update(3, 25, emit);
    b.update(1, 1000, emit);
    b.update(1, 1025, emit);
    b.update(0, 2000, emit);
    b.update(0, 2025, emit);
    assert(events.empty());
  }
  {
    InputLogic b;
    for (int i = 0; i < 5; i++) {
      int t = i * 100;
      b.update(1, t, emit);
      b.update(1, t + 25, emit);
      b.update(0, t + 50, emit);
      b.update(0, t + 75, emit);
    }
    assert(events.size() == 5);
  }
}
