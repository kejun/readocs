#include "readocs.hpp"
#include "reader_fonts.hpp"
#include "reader_font_core.hpp"
#include <array>
#include <esp_log.h>

namespace rd {
#define FONT_ASSET(name) \
  extern const uint8_t name##_start[] asm("_binary_" #name "_start"); \
  extern const uint32_t name##_length asm(#name "_length")
FONT_ASSET(noto_sc_16);
FONT_ASSET(noto_sc_24);
FONT_ASSET(noto_sc_32);
FONT_ASSET(noto_emoji_16);
FONT_ASSET(noto_emoji_24);
FONT_ASSET(noto_emoji_32);
FONT_ASSET(emoji_map);
#undef FONT_ASSET
static std::array<FontFace, 6> faces;
static EmojiMap emoji;
static bool fonts_ready = false;
bool reader_fonts_ready() { return fonts_ready; }
bool init_reader_fonts() {
  if (fonts_ready) return true;
  const uint8_t *data[] = {noto_sc_16_start, noto_sc_24_start, noto_sc_32_start,
                          noto_emoji_16_start, noto_emoji_24_start, noto_emoji_32_start};
  const uint32_t lengths[] = {noto_sc_16_length, noto_sc_24_length, noto_sc_32_length,
                             noto_emoji_16_length, noto_emoji_24_length, noto_emoji_32_length};
  for (unsigned i = 0; i < faces.size(); ++i)
    if (!faces[i].load(data[i], lengths[i])) {
      ESP_LOGE("readocs", "Font asset %u failed validation", i);
      return false;
    }
  fonts_ready = emoji.load(emoji_map_start, emoji_map_length);
  ESP_LOGI("readocs", "fonts Noto Sans SC native=16/24/32 emoji_sequences=%u ready=%d revision=%08lx",
           emoji_sequence_count, fonts_ready, static_cast<unsigned long>(font_asset_revision));
  return fonts_ready;
}
const lgfx::IFont *reader_font(int size, bool heading) {
  static const ReaderFont regular[] = {{faces[0].font, faces[3].font, false},
                                      {faces[1].font, faces[4].font, false},
                                      {faces[2].font, faces[5].font, false}};
  static const ReaderFont bold[] = {{faces[0].font, faces[3].font, true},
                                   {faces[1].font, faces[4].font, true},
                                   {faces[2].font, faces[5].font, true}};
  const int index = size == 16 ? 0 : size == 32 ? 2 : 1;
  return heading ? &bold[index] : &regular[index];
}
const EmojiMap &reader_emoji_map() { return emoji; }
std::string display_text(const std::string &text) { return shape_text(text, emoji); }
int text_width(const std::string &text) { return M5.Display.textWidth(display_text(text).c_str()); }
void print_text(const std::string &text) { M5.Display.print(display_text(text).c_str()); }
} // namespace rd
