#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

namespace rd {
inline uint16_t font_u16(const uint8_t *p) { return p[0] | (p[1] << 8); }
inline uint32_t font_u32(const uint8_t *p) {
  return uint32_t(font_u16(p)) | (uint32_t(font_u16(p + 2)) << 16);
}
class EmojiMap {
  const uint8_t *data_ = nullptr;
  uint32_t count_ = 0, strings_ = 0;

public:
  bool load(const uint8_t *data, size_t size) {
    data_ = nullptr;
    if (size < 16 || std::memcmp(data, "RDEM", 4))
      return false;
    const auto count = font_u32(data + 4), strings = font_u32(data + 8);
    if (count > 6400 || strings != 16 + count * 8 || strings > size)
      return false;
    std::string previous;
    for (uint32_t i = 0; i < count; ++i) {
      const auto *entry = data + 16 + i * 8;
      const auto offset = font_u32(entry);
      const auto length = font_u16(entry + 4), code = font_u16(entry + 6);
      if (!length || length > 256 || offset > size - strings ||
          length > size - strings - offset || code < 0xe000 || code > 0xf8ff)
        return false;
      std::string text(reinterpret_cast<const char *>(data + strings + offset), length);
      if (i && previous >= text)
        return false;
      previous = std::move(text);
    }
    data_ = data;
    count_ = count;
    strings_ = strings;
    return true;
  }
  uint16_t lookup(const std::string &text, size_t start, size_t end) const {
    if (!data_ || start > end || end > text.size())
      return 0;
    uint32_t lo = 0, hi = count_;
    while (lo < hi) {
      const auto mid = lo + (hi - lo) / 2;
      const auto *entry = data_ + 16 + mid * 8;
      const auto *value = reinterpret_cast<const char *>(data_ + strings_ + font_u32(entry));
      const auto length = font_u16(entry + 4);
      const int order = text.compare(start, end - start, value, length);
      if (!order)
        return font_u16(entry + 6);
      if (order > 0)
        lo = mid + 1;
      else
        hi = mid;
    }
    return 0;
  }
};

inline uint32_t text_codepoint(const std::string &s, size_t &at) {
  if (at >= s.size())
    return 0;
  const auto first = static_cast<uint8_t>(s[at++]);
  if (first < 0x80)
    return first;
  const int count = first >= 0xc2 && first < 0xe0 ? 1
                    : first < 0xf0 && first >= 0xe0 ? 2
                    : first >= 0xf0 && first <= 0xf4 ? 3 : 0;
  if (!count || s.size() - at < static_cast<size_t>(count))
    return 0xfffd;
  uint32_t cp = first & ((1U << (6 - count)) - 1);
  for (int i = 0; i < count; ++i) {
    const auto next = static_cast<uint8_t>(s[at]);
    if ((next & 0xc0) != 0x80)
      return 0xfffd;
    ++at;
    cp = (cp << 6) | (next & 63);
  }
  if (cp < (count == 1 ? 0x80U : count == 2 ? 0x800U : 0x10000U) ||
      cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
    return 0xfffd;
  return cp;
}
inline void append_codepoint(std::string &out, uint32_t cp) {
  if (cp < 0x80) out += static_cast<char>(cp);
  else if (cp < 0x800) {
    out += static_cast<char>(0xc0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 63));
  } else {
    out += static_cast<char>(0xe0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 63));
    out += static_cast<char>(0x80 | (cp & 63));
  }
}
inline bool text_extension(uint32_t cp) {
  return (cp >= 0x300 && cp <= 0x36f) || (cp >= 0x1ab0 && cp <= 0x1aff) ||
         (cp >= 0x1dc0 && cp <= 0x1dff) || (cp >= 0x20d0 && cp <= 0x20ff) ||
         (cp >= 0xfe00 && cp <= 0xfe0f) || (cp >= 0xfe20 && cp <= 0xfe2f) ||
         (cp >= 0x1f3fb && cp <= 0x1f3ff) || (cp >= 0xe0020 && cp <= 0xe007f) ||
         (cp >= 0xe0100 && cp <= 0xe01ef);
}
struct TextCluster {
  size_t start = 0, end = 0;
  unsigned codepoints = 0;
  uint16_t emoji = 0;
  bool complex = false;
};
inline TextCluster next_text_cluster(const std::string &text, size_t &at,
                                     const EmojiMap &map) {
  TextCluster cluster;
  cluster.start = at;
  if (at >= text.size()) { cluster.end = at; return cluster; }
  const auto first = text_codepoint(text, at);
  cluster.codepoints = 1;
  bool joined = false;
  const bool regional = first >= 0x1f1e6 && first <= 0x1f1ff;
  while (at < text.size()) {
    auto look = at;
    const auto next = text_codepoint(text, look);
    const bool pair = regional && cluster.codepoints == 1 &&
                      next >= 0x1f1e6 && next <= 0x1f1ff;
    if (!text_extension(next) && next != 0x200d && !joined && !pair)
      break;
    cluster.complex |= pair || next == 0x200d ||
                       (next >= 0x1f3fb && next <= 0x1f3ff) ||
                       (next >= 0xe0020 && next <= 0xe007f);
    joined = next == 0x200d;
    ++cluster.codepoints;
    at = look;
  }
  cluster.end = at;
  cluster.emoji = map.lookup(text, cluster.start, cluster.end);
  return cluster;
}
inline std::string shape_cluster(const std::string &text, const TextCluster &c) {
  std::string out;
  if (c.emoji) {
    append_codepoint(out, c.emoji);
  } else if (c.complex) {
    append_codepoint(out, 0x25a1);
  } else {
    for (auto at = c.start; at < c.end;) {
      auto cp = text_codepoint(text, at);
      if ((cp >= 0xfe00 && cp <= 0xfe0f) || (cp >= 0xe0100 && cp <= 0xe01ef))
        continue;
      if (cp > 0xffff || (cp >= 0xe000 && cp <= 0xf8ff))
        cp = 0x25a1;
      append_codepoint(out, cp);
    }
  }
  return out;
}
inline std::string shape_text(const std::string &text, const EmojiMap &map) {
  std::string out;
  out.reserve(text.size());
  for (size_t at = 0; at < text.size();) {
    const auto c = next_text_cluster(text, at, map);
    out += shape_cluster(text, c);
  }
  return out;
}
inline size_t last_text_cluster(const std::string &text, const EmojiMap &map) {
  size_t last = 0;
  for (size_t at = 0; at < text.size();) {
    last = at;
    next_text_cluster(text, at, map);
  }
  return last;
}
} // namespace rd
