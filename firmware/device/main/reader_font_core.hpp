#pragma once
#include <lgfx/v1/lgfx_fonts.hpp>
#include <lgfx/v1/misc/DataWrapper.hpp>
#include <algorithm>
#include <cstring>

namespace rd {
// Generated assets are immutable application data. Keep seeks bounded even if
// an asset is truncated; glyphs use memory-mapped data without filesystem I/O.
class FontData : public lgfx::DataWrapper {
  const uint8_t *data_ = nullptr;
  uint32_t size_ = 0, position_ = 0;

public:
  void set(const uint8_t *data, uint32_t size) { data_ = data; size_ = size; position_ = 0; }
  int read(uint8_t *out, uint32_t length) override {
    const auto count = std::min(length, size_ - position_);
    if (!count) return 0;
    std::memcpy(out, data_ + position_, count);
    position_ += count;
    return count;
  }
  void skip(int32_t offset) override {
    const int64_t next = int64_t(position_) + offset;
    if (next >= 0 && next <= size_) position_ = next;
  }
  bool seek(uint32_t offset) override {
    if (offset > size_) return false;
    position_ = offset;
    return true;
  }
  void close() override {}
  int32_t tell() override { return position_; }
};
struct FontFace {
  FontData data;
  lgfx::BFFfont font;
  bool load(const uint8_t *bytes, uint32_t size) {
    data.set(bytes, size);
    return font.loadFont(&data) && font.bits_per_pixel == 1 &&
           font.advance_width_format == 0;
  }
};
// One interface supplies identical metrics and drawing for Chinese, Latin,
// mapped whole Emoji sequences and the visible missing-character fallback.
class ReaderFont : public lgfx::IFont {
  const lgfx::BFFfont &body_, &emoji_;
  bool bold_;
  const lgfx::BFFfont &face(uint16_t cp) const {
    return cp >= 0xe000 && cp <= 0xf8ff ? emoji_ : body_;
  }
  uint16_t glyph(uint16_t cp) const {
    lgfx::FontMetrics m{};
    return face(cp).updateFontMetric(&m, cp) ? cp : 0x25a1;
  }

public:
  ReaderFont(const lgfx::BFFfont &body, const lgfx::BFFfont &emoji, bool bold)
      : body_(body), emoji_(emoji), bold_(bold) {}
  void getDefaultMetric(lgfx::FontMetrics *metrics) const override {
    body_.getDefaultMetric(metrics);
  }
  bool updateFontMetric(lgfx::FontMetrics *metrics, uint16_t cp) const override {
    const auto code = glyph(cp);
    face(code).updateFontMetric(metrics, code);
    if (bold_ && code < 0xe000) {
      ++metrics->width;
      ++metrics->x_advance;
    }
    return true;
  }
  size_t drawChar(lgfx::LGFXBase *gfx, int32_t x, int32_t y, uint16_t cp,
                  const lgfx::TextStyle *style, lgfx::FontMetrics *metrics,
                  int32_t &filled_x) const override {
    const auto code = glyph(cp);
    const auto &selected = face(code);
    const auto advance = selected.drawChar(gfx, x, y, code, style, metrics, filled_x);
    if (bold_ && code < 0xe000) {
      // Transparent second pass thickens the native bitmap by one pixel.
      auto overlay = *style;
      overlay.back_rgb888 = overlay.fore_rgb888;
      selected.drawChar(gfx, x + 1, y, code, &overlay, metrics, filled_x);
      return advance + 1;
    }
    return advance;
  }
};
} // namespace rd
