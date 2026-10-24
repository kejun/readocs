#pragma once
#include <cstdint>

namespace rd {
enum class Key { Next, Previous, Check, Menu, PairConfirm, PreviousItem };

// Hardware-independent debounce/gesture state, driven by monotonic
// milliseconds.
class InputLogic {
  unsigned stable = 0, candidate = 0, held = 0, latched = 0;
  int64_t changed = 0, pressed[2] = {0, 0}, both = 0;
  bool combo = false, combo_sent = false;

public:
  template <typename Emit> void update(unsigned raw, int64_t now, Emit emit) {
    if (raw != candidate) {
      candidate = raw;
      changed = now;
    }
    if (now - changed >= 25 && stable != candidate) {
      unsigned down = candidate & ~stable, up = stable & ~candidate;
      stable = candidate;
      for (int i = 0; i < 2; ++i)
        if (down & (1U << i)) {
          pressed[i] = now;
          held |= 1U << i;
        }
      if (stable == 3 && !latched) {
        combo = true;
        both = now;
        combo_sent = false;
      }
      for (int i = 0; i < 2; ++i) {
        if (!(up & (1U << i)))
          continue;
        if (!combo && !(latched & (1U << i)) && (held & (1U << i)))
          emit(i == 0 ? Key::Next : Key::Previous);
        held &= ~(1U << i);
        latched &= ~(1U << i);
      }
      if (!stable) {
        // A quick A+B chord is the article-list "previous item" gesture.
        // Holding it for a second or more is not a short chord, and holding
        // for three seconds still becomes the Wi-Fi pairing confirmation.
        if (combo && !combo_sent && now - both <= 1000)
          emit(Key::PreviousItem);
        combo = false;
        combo_sent = false;
      }
    }
    if (combo) {
      if (stable == 3 && !combo_sent && now - both >= 3000) {
        emit(Key::PairConfirm);
        combo_sent = true;
      }
    } else {
      for (int i = 0; i < 2; ++i) {
        if ((stable & (1U << i)) && !(latched & (1U << i)) &&
            now - pressed[i] >= 1000) {
          latched |= 1U << i;
          emit(i == 0 ? Key::Check : Key::Menu);
        }
      }
    }
  }
};
} // namespace rd
