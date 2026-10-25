#pragma once
#include "text_clusters.hpp"
#include "../fonts/revision.hpp"
#include <lgfx/v1/lgfx_fonts.hpp>

namespace rd {
bool init_reader_fonts();
bool reader_fonts_ready();
const lgfx::IFont *reader_font(int size, bool heading = false);
const EmojiMap &reader_emoji_map();
std::string display_text(const std::string &text);
int text_width(const std::string &text);
void print_text(const std::string &text);
} // namespace rd
