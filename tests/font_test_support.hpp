#pragma once
#include "reader_font_core.hpp"
#include "reader_fonts.hpp"
#include <array>
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <vector>

inline size_t font_metric_calls = 0;
namespace lgfx { inline namespace v1 {
static void *heap_alloc(size_t n) { return malloc(n); }
static void *heap_alloc_psram(size_t n) { return malloc(n); }
static void heap_free(void *p) { free(p); }
class LGFXBase {
  uint32_t color_ = 0;
public:
  int width = 640, height = 128;
  std::vector<uint32_t> pixels = std::vector<uint32_t>(640 * 128, 0xffffff);
  struct Converter { uint32_t convert(uint32_t c) { return c; } } converter;
  Converter *getColorConverter() { return &converter; }
  uint32_t getBaseColor() { return 0xffffff; }
  void startWrite() {} void endWrite() {}
  void setRawColor(uint32_t color) { color_ = color; }
  void writeFillRect(int x, int y, int w, int h) {
    for (int yy = std::max(0, y); yy < std::min(height, y + h); ++yy)
      for (int xx = std::max(0, x); xx < std::min(width, x + w); ++xx)
        pixels[yy * width + xx] = color_;
  }
};
size_t IFont::drawCharDummy(LGFXBase *, int32_t, int32_t, int32_t w, int32_t,
                            const TextStyle *, int32_t &) const { return w; }
#include "bff_under_test.hpp"
}}
namespace rd {
inline std::array<FontFace, 6> test_faces;
inline std::array<std::vector<uint8_t>, 7> test_assets;
inline EmojiMap test_emoji;
inline void load_test_fonts(const std::string &directory) {
  const char *names[] = {"noto_sc_16.bff", "noto_sc_24.bff", "noto_sc_32.bff",
                         "noto_emoji_16.bff", "noto_emoji_24.bff", "noto_emoji_32.bff", "emoji.emap"};
  for (unsigned i = 0; i < 7; ++i) {
    std::ifstream stream(directory + '/' + names[i], std::ios::binary);
    assert(stream.good());
    test_assets[i].assign(std::istreambuf_iterator<char>(stream), {});
    assert(!test_assets[i].empty());
    if (i < 6) assert(test_faces[i].load(test_assets[i].data(), test_assets[i].size()));
  }
  assert(test_emoji.load(test_assets[6].data(), test_assets[6].size()));
}
inline const lgfx::IFont *reader_font(int size, bool heading) {
  static ReaderFont regular[] = {{test_faces[0].font, test_faces[3].font, false},
                                {test_faces[1].font, test_faces[4].font, false},
                                {test_faces[2].font, test_faces[5].font, false}};
  static ReaderFont bold[] = {{test_faces[0].font, test_faces[3].font, true},
                             {test_faces[1].font, test_faces[4].font, true},
                             {test_faces[2].font, test_faces[5].font, true}};
  const int i = size == 16 ? 0 : size == 32 ? 2 : 1;
  return heading ? &bold[i] : &regular[i];
}
inline const EmojiMap &reader_emoji_map() { return test_emoji; }
inline std::string display_text(const std::string &s) { return shape_text(s, test_emoji); }
}
