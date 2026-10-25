#include "layout_width.hpp"
#include <cassert>

int main() {
  rd::LineExtent line;
  line.append({10, 12, -2});
  assert(line.width() == 12);
  line.append({7, 10, 1});
  assert(line.width() == 23);
  line.append({8, 6, 0});
  assert(line.width() == 27);
  auto candidate = line;
  candidate.append({9, 14, -1});
  assert(candidate.width() == 40 && line.width() == 27);
  rd::GlyphExtentCache cache;
  int calls = 0;
  auto measure = [&](uint32_t cp) {
    ++calls;
    return rd::GlyphExtent{static_cast<int>(cp), 3, -1};
  };
  assert(cache.get(65, measure).advance == 65);
  assert(cache.get(65, measure).advance == 65 && calls == 1);
  assert(cache.get(65 + 256, measure).advance == 65 + 256);
  assert(cache.get(65, measure).advance == 65 && calls == 3);
}
