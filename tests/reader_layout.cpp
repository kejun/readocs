#include "layout_width.hpp"
#include "lgfx_fonts.hpp"
#include <cJSON.h>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

static size_t yields = 0;
#include "font_test_support.hpp"
struct Display {
  const lgfx::IFont *font = rd::reader_font(24, false);
  lgfx::TextStyle _text_style;
  enum utf8_decode_state_t : uint8_t { utf8_state0, utf8_state1, utf8_state2, utf8_state3 };
  utf8_decode_state_t _decoderState = utf8_decode_state_t::utf8_state0;
  uint32_t _unicode_buffer = 0;
  bool _emoji_draw_cb = false;
  void setFont(const lgfx::IFont *value) { font = value; }
  const lgfx::IFont *getFont() const { return font; }
  void setTextSize(float value) { _text_style.size_x = _text_style.size_y = value; }
  int textWidth(const char *text) {
    lgfx::FontMetrics metrics{};
    font->getDefaultMetric(&metrics);
    return text_width(text, font, &metrics);
  }
  using IFont = lgfx::IFont;
  using FontMetrics = lgfx::FontMetrics;
#include "width_reference.hpp"
};
struct { Display Display; } M5;
static int64_t esp_timer_get_time() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
static void vTaskDelay(int) { ++yields; }
template <typename... Args> static void log_message(Args...) {}
#define ESP_LOGI(...) log_message(__VA_ARGS__)
#define ESP_LOGW(...) log_message(__VA_ARGS__)

namespace rd {
int text_width(const std::string &s) { return M5.Display.textWidth(display_text(s).c_str()); }
struct JsonDelete { void operator()(cJSON *j) const { cJSON_Delete(j); } };
using Json = std::unique_ptr<cJSON, JsonDelete>;
static std::string str(cJSON *j, const char *key) {
  auto *v = cJSON_GetObjectItemCaseSensitive(j, key);
  return cJSON_IsString(v) ? v->valuestring : "";
}
static int num(cJSON *j, const char *key) {
  auto *v = cJSON_GetObjectItemCaseSensitive(j, key);
  return cJSON_IsNumber(v) ? v->valueint : 0;
}
struct Line { std::string text; int y; bool heading; };
struct Page { std::vector<Line> lines; std::string block; unsigned offset = 0; };
static std::string document;
static std::string read_file(const std::string &) { return document; }
#include "layout_under_test.hpp"

static void compare(int size, bool require_speedup) {
  font_metric_calls = yields = 0;
  const auto started = esp_timer_get_time();
  auto pages = layout("fixture", size);
  const auto optimized_calls = font_metric_calls;
  const auto optimized_ms = (esp_timer_get_time() - started) / 1000;
  assert(!pages.empty() && yields > 0);
  font_metric_calls = 0;
  auto reference = layout_reference("fixture", size);
  const auto reference_calls = font_metric_calls;
  assert(pages.size() == reference.size());
  for (size_t p = 0; p < pages.size(); ++p) {
    assert(pages[p].block == reference[p].block && pages[p].offset == reference[p].offset);
    assert(pages[p].lines.size() == reference[p].lines.size());
    for (size_t l = 0; l < pages[p].lines.size(); ++l) {
      const auto &a = pages[p].lines[l], &b = reference[p].lines[l];
      assert(a.text == b.text && a.y == b.y && a.heading == b.heading);
    }
  }
  if (require_speedup) assert(reference_calls > optimized_calls * 10);
  std::printf("layout bytes=%zu size=%d pages=%zu metric_calls=%zu/%zu host_ms=%lld\n",
              document.size(), size, pages.size(), optimized_calls, reference_calls,
              static_cast<long long>(optimized_ms));
}
}
int main(int argc, char **argv) {
  using namespace rd;
  assert(argc >= 2);
  load_test_fonts(argv[1]);
  Json doc(cJSON_CreateObject());
  auto *blocks = cJSON_AddArrayToObject(doc.get(), "blocks");
  const std::string mixed = "文章更新中文测试，English ffi jW 0123!? e\u0301 ❤\ufe0f 👩‍💻\t\r\n";
  const char *types[] = {"paragraph", "h1", "h2", "bullet", "ordered", "quote", "separator"};
  for (unsigned b = 0; b < 240; ++b) {
    auto *block = cJSON_CreateObject();
    cJSON_AddItemToArray(blocks, block);
    cJSON_AddStringToObject(block, "block_id", std::to_string(b).c_str());
    cJSON_AddStringToObject(block, "type", types[b % 7]);
    cJSON_AddNumberToObject(block, "depth", b % 5);
    cJSON_AddStringToObject(block, "text", (mixed + mixed + mixed + mixed).c_str());
  }
  char *serialized = cJSON_PrintUnformatted(doc.get());
  document = serialized;
  cJSON_free(serialized);
  for (const int size : {16, 24, 32}) compare(size, true);
  if (argc > 2) {
    std::ifstream input(argv[2]);
    document.assign(std::istreambuf_iterator<char>(input), {});
    assert(document.size() > 60000);
    for (const int size : {16, 24, 32}) compare(size, true);
  }
}
