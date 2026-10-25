#include "font_test_support.hpp"
#include <iostream>
using namespace rd;
static uint64_t render(const std::string &raw, int size, bool heading) {
  lgfx::LGFXBase display;
  const auto *font = reader_font(size, heading);
  lgfx::TextStyle style;
  style.fore_rgb888 = 0;
  style.back_rgb888 = 0xffffff;
  style.size_x = style.size_y = 1;
  lgfx::FontMetrics metrics{};
  font->getDefaultMetric(&metrics);
  const auto text = display_text(raw);
  int x = 0, filled = 0;
  for (size_t at = 0; at < text.size();) {
    const auto cp = text_codepoint(text, at);
    assert(cp <= 0xffff);
    assert(font->updateFontMetric(&metrics, cp));
    const auto advance = font->drawChar(&display, x, metrics.baseline, cp, &style, &metrics, filled);
    assert(advance == static_cast<unsigned>(metrics.x_advance));
    x += advance;
  }
  unsigned ink = 0;
  uint64_t hash = 0;
  for (auto pixel : display.pixels) {
    assert(pixel == 0 || pixel == 0xffffff); // No gray/dither introduced.
    ink += pixel == 0;
    hash = hash * 131 + pixel;
  }
  assert(ink > 10);
  return hash;
}
int main(int argc, char **argv) {
  assert(argc == 2);
  load_test_fonts(argv[1]);
  for (const std::string emoji : {"😀", "❤️", "👩‍💻", "👍🏽", "🇨🇳", "1️⃣", "👨‍👩‍👧‍👦"}) {
    size_t at = 0;
    const auto c = next_text_cluster(emoji, at, reader_emoji_map());
    assert(at == emoji.size() && c.emoji);
    auto shaped = display_text(emoji);
    size_t p = 0;
    assert(text_codepoint(shaped, p) == c.emoji && p == shaped.size());
    assert(last_text_cluster("中文" + emoji, reader_emoji_map()) == std::string("中文").size());
    for (auto size : {16, 24, 32}) {
      const auto a = render(emoji, size, false);
      assert(a != render("□", size, false));
    }
  }
  const std::string flags = "🇨🇳🇺🇸";
  size_t at = 0;
  assert(next_text_cluster(flags, at, reader_emoji_map()).codepoints == 2);
  assert(next_text_cluster(flags, at, reader_emoji_map()).codepoints == 2 && at == flags.size());
  for (auto size : {16, 24, 32}) {
    assert(render("中文阅读，ABC 0123。", size, false) != render("中文阅读，ABC 0123。", size, true));
    assert(render("𠀀", size, false) == render("□", size, false));
  }
  EmojiMap broken;
  auto bytes = test_assets[6];
  assert(!broken.load(bytes.data(), 8));
  bytes[4] = 255; bytes[5] = 255;
  assert(!broken.load(bytes.data(), bytes.size()));
  for (const auto &bad : {std::string("\xf0\x80\x80\x80",4), std::string("\xff",1)}) {
    at = 0;
    while (at < bad.size()) {
      const auto before = at;
      next_text_cluster(bad, at, reader_emoji_map());
      assert(at > before);
    }
  }
  FontData bounded;
  const uint8_t short_data[] = {1,2,3};
  bounded.set(short_data, 3);
  assert(!bounded.seek(4));
  uint8_t output[4]{};
  assert(bounded.read(output, 4) == 3 && bounded.read(output, 1) == 0);
  std::cout << "Native Chinese / Emoji assets, metrics, pixels and grapheme boundaries passed\n";
}
