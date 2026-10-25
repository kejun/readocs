#include "readocs.hpp"
#include "layout_width.hpp"
#include <algorithm>
#include <cctype>
#include <esp_log.h>
#include <esp_timer.h>

namespace rd {
extern const uint8_t cover_jpg_start[] asm("_binary_cover_jpg_start");
extern const uint32_t cover_jpg_length asm("cover_jpg_length");

static std::atomic<bool> probe_pending{false};
static int probe_mode = 0;
static int probe_area = 0;
static int probe_count = 2;

static constexpr auto kFullMode = m5gfx::epd_mode_t::epd_text;
static constexpr auto kQualityMode = m5gfx::epd_mode_t::epd_quality;
static constexpr auto kPartialMode = m5gfx::epd_mode_t::epd_fast;

static std::atomic<int> partial_updates{0};
static std::atomic<int> partial_limit{20};
static std::atomic<bool> force_quality{false};
static bool quality_full_draw = false;

static bool menu_valid = false;
static std::string menu_signature;
static int menu_selected = -1;
static int menu_start = -1;
static bool articles_valid = false;
static std::string articles_signature;
static int articles_selected = -1;
static int articles_start = -1;
static bool sources_valid = false;
static std::string sources_signature;
static int sources_selected = -1, sources_start = -1;
static bool page_valid = false;
static std::string page_source, page_document, page_snapshot, page_title;
static std::string metadata_snapshot, content_timestamp;
static unsigned page_font_size = 0;
static int page_index = -1, page_count = 0;

static void invalidate_navigation() {
  menu_valid = false;
  articles_valid = false;
  sources_valid = false;
  page_valid = false;
}

static const char *epd_mode_name(int mode) {
  switch (mode) {
  case 1:
    return "quality";
  case 2:
    return "text";
  case 3:
    return "fast";
  case 4:
    return "fastest";
  default:
    return "all";
  }
}

// Locate the JPEG frame header without pulling in another decoder. This lets
// the embedded cover keep working if it is replaced with different dimensions.
static bool jpeg_size(const uint8_t *data, size_t len, int &width, int &height) {
  if (len < 4 || data[0] != 0xff || data[1] != 0xd8)
    return false;
  size_t pos = 2;
  while (pos + 3 < len) {
    if (data[pos] != 0xff) {
      ++pos;
      continue;
    }
    const uint8_t marker = data[pos + 1];
    if (marker == 0xff) {
      ++pos;
      continue;
    }
    if (marker == 0xd8 || marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) {
      pos += 2;
      continue;
    }
    if (pos + 4 > len)
      break;
    const size_t segment_len = (static_cast<size_t>(data[pos + 2]) << 8) |
                               data[pos + 3];
    if (segment_len < 2 || pos + 2 + segment_len > len)
      break;
    const bool is_sof = (marker >= 0xc0 && marker <= 0xc3) ||
                        (marker >= 0xc5 && marker <= 0xc7) ||
                        (marker >= 0xc9 && marker <= 0xcb) ||
                        (marker >= 0xcd && marker <= 0xcf);
    if (is_sof) {
      if (segment_len < 7)
        break;
      height = (static_cast<int>(data[pos + 5]) << 8) | data[pos + 6];
      width = (static_cast<int>(data[pos + 7]) << 8) | data[pos + 8];
      return width > 0 && height > 0;
    }
    pos += 2 + segment_len;
  }
  return false;
}
static void font(int size, bool heading = false) {
  M5.Display.setFont(
      heading ? &fonts::efontCN_24_b
              : (size == 16 ? &fonts::efontCN_16 : &fonts::efontCN_24));
  M5.Display.setTextSize(size == 32 ? 32.0f / 24.0f : 1.0f);
}
static uint32_t codepoint(const std::string &s, size_t &at) {
  auto c = static_cast<uint8_t>(s[at++]);
  if (c < 128)
    return c;
  int n = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
  uint32_t value = c & ((1U << (6 - n)) - 1);
  while (n-- && at < s.size())
    value = (value << 6) | (static_cast<uint8_t>(s[at++]) & 63);
  return value;
}
static bool extends(uint32_t cp) {
  return (cp >= 0x300 && cp <= 0x36f) || (cp >= 0x1ab0 && cp <= 0x1aff) ||
         (cp >= 0x1dc0 && cp <= 0x1dff) || (cp >= 0xfe00 && cp <= 0xfe0f) ||
         (cp >= 0xfe20 && cp <= 0xfe2f) || (cp >= 0x1f3fb && cp <= 0x1f3ff) ||
         (cp >= 0xe0100 && cp <= 0xe01ef);
}
std::vector<Page> layout(const std::string &snapshot, int size) {
  const int64_t started = esp_timer_get_time();
  Json doc(cJSON_Parse(read_file(snapshot).c_str()));
  if (!doc) {
    ESP_LOGW("readocs", "layout invalid snapshot=%s", snapshot.c_str());
    return {};
  }
  ESP_LOGI("readocs", "layout start snapshot=%s size=%d", snapshot.c_str(), size);
  GlyphExtentCache glyphs[2];
  std::vector<Page> pages(1);
  int y = 94;
  const int bottom = 887, line_height = size + 14;
  std::string title = str(doc.get(), "title");
  cJSON *blocks = cJSON_GetObjectItemCaseSensitive(doc.get(), "blocks");
  auto add = [&](const std::string &text, bool heading, const std::string &bid,
                 unsigned offset) {
    if (y + line_height > bottom) {
      pages.emplace_back();
      y = 94;
    }
    if (pages.back().lines.empty()) {
      pages.back().block = bid;
      pages.back().offset = offset;
    }
    pages.back().lines.push_back({text, y, heading});
    y += line_height;
  };
  cJSON *block;
  cJSON_ArrayForEach(block, blocks) {
    std::string bid = str(block, "block_id"), type = str(block, "type"),
                text = str(block, "text");
    bool heading = type == "h1" || type == "h2" || type == "h3";
    if (type == "separator") {
      add("────────────────────", false, bid, 0);
      continue;
    }
    std::string line;
    unsigned offset = 0, line_start = 0;
    int indent = std::min(3, std::max(0, num(block, "depth"))) * 12;
    if (type == "bullet")
      line = "• ";
    else if (type == "ordered")
      line = "1. ";
    else if (type == "quote")
      line = "│ ";
    font(size, heading);
    const auto *active_font = M5.Display.getFont();
    const int scale = 65536 * (size == 32 ? 32.0f / 24.0f : 1.0f);
    auto measure = [&](uint32_t cp) {
      lgfx::FontMetrics metrics{};
      active_font->getDefaultMetric(&metrics);
      if (cp > 0xffff ||
          (!active_font->updateFontMetric(&metrics, cp) && cp != 0))
        active_font->updateFontMetric(&metrics, 0);
      return GlyphExtent{(metrics.x_advance * scale) >> 16,
                         (metrics.width * scale) >> 16,
                         (metrics.x_offset * scale) >> 16};
    };
    auto append_width = [&](LineExtent &width, const std::string &part) {
      for (size_t at = 0; at < part.size();) {
        const auto cp = codepoint(part, at);
        if (cp < 0x20 || (cp >= 0xfe00 && cp < 0xfe10))
          continue;
        width.append(glyphs[heading].get(cp, measure));
      }
    };
    LineExtent line_width;
    append_width(line_width, line);
    for (size_t i = 0; i < text.size();) {
      // Long documents must let the idle task run during font measurement.
      if ((offset & 63) == 0)
        vTaskDelay(1);
      size_t start = i;
      uint32_t cp = codepoint(text, i);
      unsigned cluster_count = 1;
      bool joined = false;
      while (i < text.size()) {
        size_t look = i;
        uint32_t next = codepoint(text, look);
        if (extends(next) || next == 0x200d || joined) {
          i = look;
          ++cluster_count;
          joined = next == 0x200d;
        } else
          break;
      }
      if (cp == '\r')
        continue;
      if (cp == '\n') {
        add(std::string(indent / 8, ' ') + line, heading, bid, line_start);
        line.clear();
        line_width = {};
        offset += cluster_count;
        line_start = offset;
        continue;
      }
      auto cluster = text.substr(start, i - start);
      auto next_width = line_width;
      append_width(next_width, cluster);
      if (!line.empty() && next_width.width() > 476 - indent) {
        add(std::string(indent / 8, ' ') + line, heading, bid, line_start);
        line.clear();
        line_start = offset;
        next_width = {};
        append_width(next_width, cluster);
      }
      line_width = next_width;
      line += cluster;
      offset += cluster_count;
    }
    if (!line.empty())
      add(std::string(indent / 8, ' ') + line, heading, bid, line_start);
    y += heading ? 16 : 12;
  }
  if (pages.size() == 1 && pages[0].lines.empty())
    add("[此文档没有可显示的正文]", false, "", 0);
  ESP_LOGI("readocs", "layout done pages=%u time_ms=%d",
           static_cast<unsigned>(pages.size()),
           static_cast<int>((esp_timer_get_time() - started) / 1000));
  return pages;
}
static void begin() {
  invalidate_navigation();
  auto &d = M5.Display;
  quality_full_draw =
      force_quality.load() || partial_updates.load() >= partial_limit.load();
  d.setEpdMode(quality_full_draw ? kQualityMode : kFullMode);
  d.fillScreen(TFT_WHITE);
  d.setTextColor(TFT_BLACK, TFT_WHITE);
  d.setTextDatum(m5gfx::textdatum_t::top_left);
  d.setTextWrap(false);
}
static void finish(const std::string &snapshot = "") {
  display_marker(true);
  const auto mode = quality_full_draw ? kQualityMode : kFullMode;
  M5.Display.setEpdMode(mode);
  const int64_t started = esp_timer_get_time();
  M5.Display.display();
  M5.Display.waitDisplay();
  const int before = partial_updates.exchange(0);
  force_quality.store(false);
  ESP_LOGI("readocs", "epd ui mode=%s time_ms=%d cleanup=%d partial_before=%d",
           epd_mode_name(static_cast<int>(mode)),
           static_cast<int>((esp_timer_get_time() - started) / 1000),
           quality_full_draw ? 1 : 0, before);
  display_marker(false, snapshot);
  quality_full_draw = false;
}
static void finish_partial(int x, int y, int w, int h) {
  M5.Display.setEpdMode(kPartialMode);
  const int64_t started = esp_timer_get_time();
  M5.Display.display(x, y, w, h);
  M5.Display.waitDisplay();
  const int count = partial_updates.fetch_add(1) + 1;
  ESP_LOGI("readocs", "epd partial mode=%s time_ms=%d partial=%d/%d rect=%d,%d,%d,%d",
           epd_mode_name(static_cast<int>(kPartialMode)),
           static_cast<int>((esp_timer_get_time() - started) / 1000), count,
           partial_limit.load(), x, y, w, h);
}

bool quality_cleanup_due() {
  return force_quality.load() || partial_updates.load() >= partial_limit.load();
}
void request_quality_cleanup() {
  force_quality.store(true);
  invalidate_navigation();
}
int display_partial_count() { return partial_updates.load(); }
int display_partial_limit() { return partial_limit.load(); }
void set_display_partial_limit(int limit) {
  partial_limit.store(std::clamp(limit, 1, 200));
}
void draw_page(const Entry &e, const std::vector<Page> &pages, int index) {
  if (pages.empty())
    return;
  index = std::clamp(index, 0, static_cast<int>(pages.size()) - 1);
  auto &d = M5.Display;
  unsigned size;
  {
    std::lock_guard<std::mutex> g(model.mutex);
    size = model.config.font_size;
  }
  const bool same_layout =
      page_valid && page_source == e.source_id && page_document == e.document_id &&
      page_snapshot == e.snapshot_id && page_font_size == size &&
      page_title == e.title && page_count == static_cast<int>(pages.size());
  if (same_layout && page_index == index && !quality_cleanup_due())
    return;
  const bool partial = same_layout && page_index != index &&
      !force_quality.load() && partial_updates.load() < partial_limit.load();
  if (partial) {
    // A page turn only changes the body and footer. Keep the title and its rule
    // on the panel, clear the rest of the persistent framebuffer, and use the
    // fast 1-bit waveform. Periodic cleanup restores gray levels.
    d.setEpdMode(kPartialMode);
    d.fillRect(0, 63, d.width(), d.height() - 63, TFT_WHITE);
    d.setTextColor(TFT_BLACK, TFT_WHITE);
    d.setTextDatum(m5gfx::textdatum_t::top_left);
    d.setTextWrap(false);
  } else {
    begin();
    font(16);
    d.setCursor(32, 25);
    std::string title = e.title;
    while (!title.empty() && d.textWidth(title.c_str()) > 475) {
      size_t i = title.size() - 1;
      while (i > 0 && (static_cast<unsigned char>(title[i]) & 0xc0) == 0x80)
        --i;
      title.resize(i);
    }
    d.print(title.c_str());
    d.drawFastHLine(32, 62, 476, TFT_BLACK);
  }
  font(size);
  for (const auto &line : pages[index].lines) {
    font(size, line.heading);
    d.setCursor(32, line.y);
    d.print(line.text.c_str());
  }
  font(16);
  d.setCursor(32, 896);
  d.print("A 下一页   B 上一页   按住 B 列表");
  d.setCursor(32, 922);
  d.printf("%d / %u", index + 1, static_cast<unsigned>(pages.size()));
  if (metadata_snapshot != e.snapshot_id) {
    Json doc(cJSON_Parse(read_file(e.snapshot_id).c_str()));
    content_timestamp = str(doc.get(), "content_updated_at");
    metadata_snapshot = e.snapshot_id;
  }
  if (!content_timestamp.empty()) {
    d.setCursor(245, 922);
    d.print(content_timestamp.substr(0, 19).c_str());
  }
  if (partial)
    finish_partial(0, 63, d.width(), d.height() - 63);
  else
    finish(e.snapshot_id);
  page_valid = true;
  page_source = e.source_id;
  page_document = e.document_id;
  page_snapshot = e.snapshot_id;
  page_title = e.title;
  page_font_size = size;
  page_index = index;
  page_count = static_cast<int>(pages.size());
  {
    std::lock_guard<std::mutex> g(model.mutex);
    model.page = index;
  }
  position = {e.document_id, pages[index].block, e.snapshot_id,
              pages[index].offset, index};
  save_position(position);
}
static std::vector<std::string> article_title(const std::string &title) {
  std::vector<std::string> lines(1);
  auto &d = M5.Display;
  for (size_t at = 0; at < title.size();) {
    size_t start = at;
    codepoint(title, at);
    auto cluster = title.substr(start, at - start);
    if (cluster == "\n" || cluster == "\r")
      cluster = " ";
    if (d.textWidth((lines.back() + cluster).c_str()) > 450) {
      if (lines.size() == 2) {
        while (!lines.back().empty() &&
               d.textWidth((lines.back() + "…").c_str()) > 450) {
          size_t end = lines.back().size() - 1;
          while (end > 0 &&
                 (static_cast<unsigned char>(lines.back()[end]) & 0xc0) == 0x80)
            --end;
          lines.back().resize(end);
        }
        lines.back() += "…";
        break;
      }
      lines.emplace_back();
    }
    lines.back() += cluster;
  }
  return lines;
}
static std::string article_signature_of(const std::vector<Entry> &entries,
                                         int start) {
  std::string signature = std::to_string(entries.size());
  const int end = std::min(start + article_page_size, static_cast<int>(entries.size()));
  for (int i = start; i < end; ++i) {
    const auto &entry = entries[i];
    signature += '\n';
    signature += entry.source_id;
    signature += '\x1f';
    signature += entry.document_id;
    signature += '\x1f';
    signature += entry.title;
    signature += '\x1f';
    signature += entry.snapshot_id.empty() ? '0' : '1';
  }
  return signature;
}

static std::string menu_signature_of(const std::vector<std::string> &options,
                                     const std::string &title) {
  std::string signature = title;
  for (const auto &option : options) {
    signature += '\n';
    signature += option;
  }
  return signature;
}

static void draw_article_row(const std::vector<Entry> &entries, int selected,
                             int index, int y) {
  auto &d = M5.Display;
  const bool active = index == selected;
  d.fillRect(24, y - 8, 492, 80, active ? TFT_BLACK : TFT_WHITE);
  d.setTextColor(active ? TFT_WHITE : TFT_BLACK,
                 active ? TFT_BLACK : TFT_WHITE);
  font(24);
  auto lines = article_title(entries[index].title);
  for (size_t n = 0; n < lines.size(); ++n) {
    d.setCursor(40, y + n * 30);
    d.print(lines[n].c_str());
  }
  if (entries[index].snapshot_id.empty()) {
    font(16);
    d.setCursor(444, y + 58);
    d.print("未下载");
  }
  d.setTextColor(TFT_BLACK, TFT_WHITE);
}

static void draw_menu_row(const std::vector<std::string> &options, int selected,
                          int index, int y) {
  auto &d = M5.Display;
  const bool active = index == selected;
  d.fillRect(24, y - 8, 492, 54, active ? TFT_BLACK : TFT_WHITE);
  d.setTextColor(active ? TFT_WHITE : TFT_BLACK,
                 active ? TFT_BLACK : TFT_WHITE);
  font(24);
  std::string name = options[index];
  while (!name.empty() && d.textWidth(name.c_str()) > 450) {
    size_t n = name.size() - 1;
    while (n > 0 && (static_cast<unsigned char>(name[n]) & 0xc0) == 0x80)
      --n;
    name.resize(n);
  }
  d.setCursor(40, y);
  d.print(name.c_str());
  d.setTextColor(TFT_BLACK, TFT_WHITE);
}

static std::string fit_header(std::string title) {
  if (M5.Display.textWidth(title.c_str()) <= 330)
    return title;
  while (!title.empty() && M5.Display.textWidth((title + "…").c_str()) > 330) {
    size_t end = title.size() - 1;
    while (end > 0 && (static_cast<unsigned char>(title[end]) & 0xc0) == 0x80)
      --end;
    title.resize(end);
  }
  return title + "…";
}

static void draw_source_row(const std::vector<Source> &sources, int selected,
                             int index, int y) {
  auto &d = M5.Display;
  const bool active = index == selected;
  d.fillRect(24, y - 8, 492, 80, active ? TFT_BLACK : TFT_WHITE);
  d.setTextColor(active ? TFT_WHITE : TFT_BLACK,
                 active ? TFT_BLACK : TFT_WHITE);
  font(24);
  const auto lines = article_title(sources[index].title);
  for (size_t n = 0; n < lines.size(); ++n) {
    d.setCursor(40, y + n * 30);
    d.print(lines[n].c_str());
  }
  font(16);
  const auto count = std::to_string(sources[index].count) + " 篇";
  d.setCursor(496 - d.textWidth(count.c_str()), y + 58);
  d.print(count.c_str());
  d.setTextColor(TFT_BLACK, TFT_WHITE);
}

void draw_sources(const std::vector<Source> &sources, int selected) {
  auto &d = M5.Display;
  std::string signature;
  for (const auto &source : sources)
    signature += source.id + '\x1f' + source.title + '\x1f' +
                 std::to_string(source.count) + '\n';
  selected = sources.empty() ? 0
                             : std::clamp(selected, 0, static_cast<int>(sources.size()) - 1);
  const int start = selected / article_page_size * article_page_size;
  if (sources_valid && signature == sources_signature && start == sources_start) {
    if (selected == sources_selected)
      return;
    const int old_y = 120 + (sources_selected - start) * 88;
    const int new_y = 120 + (selected - start) * 88;
    const int top = std::min(old_y, new_y) - 8;
    const int bottom = std::max(old_y, new_y) + 72;
    d.setEpdMode(kPartialMode);
    d.setClipRect(24, top, 492, bottom - top);
    draw_source_row(sources, selected, sources_selected, old_y);
    draw_source_row(sources, selected, selected, new_y);
    d.clearClipRect();
    finish_partial(24, top, 492, bottom - top);
    sources_selected = selected;
    return;
  }
  begin();
  font(24, true);
  d.setCursor(32, 36);
  d.print("内容来源");
  d.drawFastHLine(32, 86, 476, TFT_BLACK);
  if (sources.empty()) {
    font(24);
    d.setCursor(32, 160);
    d.print("暂无内容来源");
  } else
    for (int i = start;
         i < std::min(start + article_page_size, static_cast<int>(sources.size())); ++i)
      draw_source_row(sources, selected, i, 120 + (i - start) * 88);
  font(16);
  d.setCursor(32, 880);
  d.print("A 下一来源    A+B 上一来源    B 进入");
  d.setCursor(32, 922);
  d.print("按住 A 检查更新    按住 B 菜单");
  finish();
  sources_valid = true;
  sources_signature = signature;
  sources_selected = selected;
  sources_start = start;
}

void draw_articles(const std::vector<Entry> &entries, int selected,
                   const std::string &source_title, int source_count) {
  auto &d = M5.Display;
  if (!entries.empty())
    selected = std::clamp(selected, 0, static_cast<int>(entries.size()) - 1);
  const int start =
      entries.empty() ? 0 : selected / article_page_size * article_page_size;
  // Only pixels on this screen determine its baseline. An offscreen download
  // or a new hash for an already-cached body must not force a full list redraw.
  const std::string signature = source_title + '\n' + std::to_string(source_count) +
                                '\n' + article_signature_of(entries, start);
  if (articles_valid && signature == articles_signature && start == articles_start) {
    if (selected == articles_selected)
      return;
    const int old_y = 120 + (articles_selected - articles_start) * 88;
    const int new_y = 120 + (selected - articles_start) * 88;
    const int top = std::min(old_y, new_y) - 8;
    const int bottom = std::max(old_y, new_y) + 72;
    d.setEpdMode(kPartialMode);
    d.setClipRect(24, top, 492, bottom - top);
    draw_article_row(entries, selected, articles_selected, old_y);
    draw_article_row(entries, selected, selected, new_y);
    d.clearClipRect();
    finish_partial(24, top, 492, bottom - top);
    articles_selected = selected;
    return;
  }

  begin();
  font(24, true);
  d.setCursor(32, 36);
  d.print(fit_header(source_title).c_str());
  d.drawFastHLine(32, 86, 476, TFT_BLACK);
  if (entries.empty()) {
    font(24);
    d.setCursor(32, 160);
    d.print(source_count > 0 ? "目录同步中" : "暂无文章");
  } else {
    const int end =
        std::min(start + article_page_size, static_cast<int>(entries.size()));
    font(16);
    const auto counter = std::to_string(start + 1) + "–" + std::to_string(end) +
                         " / " + std::to_string(entries.size());
    d.setCursor(508 - d.textWidth(counter.c_str()), 43);
    d.print(counter.c_str());
    for (int i = start; i < end; ++i)
      draw_article_row(entries, selected, i, 120 + (i - start) * 88);
  }
  d.setTextColor(TFT_BLACK, TFT_WHITE);
  font(16);
  if (!entries.empty()) {
    d.setCursor(32, 880);
    d.print("A 下一项    A+B 上一项    B 打开");
    d.setCursor(32, 922);
    d.print("按住 A 翻页    按住 B 返回来源");
  } else {
    d.setCursor(32, 922);
    d.print("按住 B 返回来源");
  }
  finish();
  articles_valid = true;
  articles_signature = signature;
  articles_selected = selected;
  articles_start = start;
}

void draw_menu(const std::vector<std::string> &options, int selected,
               const std::string &title) {
  auto &d = M5.Display;
  const std::string signature = menu_signature_of(options, title);
  const int start = selected / 10 * 10;
  if (menu_valid && signature == menu_signature && start == menu_start) {
    if (selected == menu_selected)
      return;
    const int old_y = 120 + (menu_selected - menu_start) * 68;
    const int new_y = 120 + (selected - menu_start) * 68;
    const int top = std::min(old_y, new_y) - 8;
    const int bottom = std::max(old_y, new_y) + 46;
    d.setEpdMode(kPartialMode);
    d.setClipRect(24, top, 492, bottom - top);
    draw_menu_row(options, selected, menu_selected, old_y);
    draw_menu_row(options, selected, selected, new_y);
    d.clearClipRect();
    finish_partial(24, top, 492, bottom - top);
    menu_selected = selected;
    return;
  }

  begin();
  font(24, true);
  d.setCursor(32, 36);
  d.print(title.c_str());
  d.drawFastHLine(32, 86, 476, TFT_BLACK);
  for (int i = start;
       i < std::min(start + 10, static_cast<int>(options.size())); ++i)
    draw_menu_row(options, selected, i, 120 + (i - start) * 68);
  d.setTextColor(TFT_BLACK, TFT_WHITE);
  font(16);
  d.setCursor(32, 922);
  d.print("A 下一项    B 确认    按住 B 返回");
  finish();
  menu_valid = true;
  menu_signature = signature;
  menu_selected = selected;
  menu_start = start;
}
void draw_message(const std::string &title,
                  const std::vector<std::string> &lines, int size) {
  begin();
  auto &d = M5.Display;
  font(24, true);
  d.setCursor(32, 50);
  d.print(title.c_str());
  font(size);
  int y = 160;
  for (const auto &line : lines) {
    d.setCursor(32, y);
    d.println(line.c_str());
    y += 60;
  }
  finish();
}
void draw_cover() {
  auto &d = M5.Display;
  d.waitDisplay();
  begin();
  d.setEpdMode(m5gfx::epd_mode_t::epd_quality);
  const auto *jpg = cover_jpg_start;
  const auto len = static_cast<size_t>(cover_jpg_length);
  int width = 0, height = 0;
  bool drawn = false;
  if (jpeg_size(jpg, len, width, height)) {
    // Fill the screen like a paper cover: scale to the larger edge and crop the
    // overflow, instead of leaving white bars around the image.
    const float scale = std::max(static_cast<float>(d.width()) / width,
                                 static_cast<float>(d.height()) / height);
    const int off_x =
        std::max(0, static_cast<int>((width * scale - d.width()) / 2));
    const int off_y =
        std::max(0, static_cast<int>((height * scale - d.height()) / 2));
    drawn = d.drawJpg(jpg, static_cast<uint32_t>(len), 0, 0, d.width(),
                      d.height(), off_x, off_y, scale, scale);
  }
  if (!drawn)
    drawn = d.drawJpg(jpg, static_cast<uint32_t>(len), 0, 0, d.width(),
                      d.height());
  if (!drawn) {
    d.setTextDatum(m5gfx::textdatum_t::middle_center);
    font(24, true);
    d.drawString("Readocs", d.width() / 2, d.height() / 2);
  }
  d.display();
  d.waitDisplay();
  partial_updates.store(0);
  force_quality.store(false);
  quality_full_draw = false;
  ESP_LOGI("readocs", "cover %s: %dx%d -> %dx%d, %u bytes",
           drawn ? "drawn" : "fallback", width, height, d.width(), d.height(),
           static_cast<unsigned>(len));
}

static void draw_probe(int x, int y, int w, int h, int mode, int iteration) {
  auto &d = M5.Display;
  d.setEpdMode(static_cast<m5gfx::epd_mode_t>(mode));
  d.fillRect(x, y, w, h, TFT_WHITE);
  d.setTextColor(TFT_BLACK, TFT_WHITE);
  d.setTextDatum(m5gfx::textdatum_t::top_left);
  d.setTextWrap(false);
  d.setFont(&fonts::efontCN_24);
  d.setTextSize(1.0f);
  d.setClipRect(x, y, w, h);
  const int block_y = y + h - 64;
  int line = 0;
  for (int yy = y + 8; yy + 32 <= block_y; yy += 32, ++line) {
    d.setCursor(x + 8, yy);
    d.printf("%s %d  ABC abc 0123456789 春日有信", epd_mode_name(mode),
             line + 1);
  }
  const int bx = x + 8 + (iteration * 53) % std::max(1, w - 96);
  d.fillRect(bx, block_y, 80, 56, TFT_BLACK);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setCursor(bx + 6, block_y + 14);
  d.print("AB");
  d.clearClipRect();
}

void request_display_probe(int mode, int area, int count) {
  probe_mode = mode;
  probe_area = area;
  probe_count = count;
  probe_pending.store(true);
}

bool run_display_probe() {
  if (!probe_pending.exchange(false))
    return false;
  invalidate_navigation();
  auto &d = M5.Display;
  d.display();
  d.waitDisplay();
  struct Area {
    const char *name;
    int x, y, w, h;
  } areas[] = {{"partial", 32, 120, 476, 240},
                {"full", 0, 0, d.width(), d.height()}};
  for (int mode = 1; mode <= 4; ++mode) {
    if (probe_mode && probe_mode != mode)
      continue;
    for (int a = 0; a < 2; ++a) {
      if (probe_area == 1 && a != 0)
        continue;
      if (probe_area == 2 && a != 1)
        continue;
      const auto &area = areas[a];
      for (int i = 0; i < probe_count; ++i) {
        draw_probe(area.x, area.y, area.w, area.h, mode, i);
        const int64_t started = esp_timer_get_time();
        d.display(area.x, area.y, area.w, area.h);
        d.waitDisplay();
        const int64_t elapsed =
            (esp_timer_get_time() - started) / 1000;
        ESP_LOGI("readocs",
                 "epd probe mode=%s area=%s iter=%d time_ms=%d",
                 epd_mode_name(mode), area.name, i + 1,
                 static_cast<int>(elapsed));
        vTaskDelay(pdMS_TO_TICKS(250));
      }
    }
  }
  invalidate_navigation();
  partial_updates.store(0);
  force_quality.store(false);
  quality_full_draw = false;
  last_input = now_ms();
  return true;
}
} // namespace rd
