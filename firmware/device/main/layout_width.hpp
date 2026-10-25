#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace rd {
struct GlyphExtent {
  int advance = 0, width = 0, offset = 0;
};

// Match LGFXBase::text_width, including the initial negative bearing and the
// last glyph's overhang. Summing textWidth(cluster) loses both of these rules.
class LineExtent {
  int left_ = 0, right_ = 0;

public:
  void append(const GlyphExtent &glyph) {
    if (left_ == 0 && right_ == 0 && glyph.offset < 0)
      left_ = right_ = -glyph.offset;
    right_ = left_ + std::max(glyph.advance, glyph.width + glyph.offset);
    left_ += glyph.advance;
  }
  int width() const { return right_; }
};

// A fixed memory budget, shared across all blocks using the same font during
// one layout. Collisions only repeat a font lookup; they cannot alter widths.
class GlyphExtentCache {
  struct Slot {
    uint32_t codepoint = UINT32_MAX;
    GlyphExtent extent;
  };
  std::vector<Slot> slots_ = std::vector<Slot>(256);

public:
  template <typename Measure>
  GlyphExtent get(uint32_t codepoint, Measure measure) {
    auto &slot = slots_[codepoint % slots_.size()];
    if (slot.codepoint != codepoint) {
      slot.extent = measure(codepoint);
      slot.codepoint = codepoint;
    }
    return slot.extent;
  }
};
} // namespace rd
