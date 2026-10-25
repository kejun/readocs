#include "article_list.hpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <cJSON.h>
#include "refresh_types.hpp"
using namespace rd;
namespace m5gfx {
enum class epd_mode_t { epd_quality = 1, epd_text = 2, epd_fast = 3 };
namespace textdatum_t { constexpr int top_left = 0; }
}
namespace fonts { static int efontCN_16, efontCN_24, efontCN_24_b; }
static constexpr int TFT_WHITE = 255, TFT_BLACK = 0;
struct Rect { int x, y, w, h; };
static bool same(Rect a, Rect b) {
  return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}
static Rect unite(Rect a, Rect b) {
  if (!a.w || !a.h) return b;
  int x = std::min(a.x, b.x), y = std::min(a.y, b.y);
  return {x, y, std::max(a.x+a.w,b.x+b.w)-x, std::max(a.y+a.h,b.y+b.h)-y};
}
struct Frame { m5gfx::epd_mode_t mode; Rect dirty, requested; };
static int64_t clock_us = 0;
struct Display {
  int x = 0, y = 0, color = 0, fills = 0;
  Rect clip{0,0,540,960}, dirty{0,0,0,0};
  m5gfx::epd_mode_t mode = m5gfx::epd_mode_t::epd_text;
  std::vector<Frame> frames;
  std::vector<unsigned char> pixels = std::vector<unsigned char>(540*960,255);
  int width() { return 540; }
  int height() { return 960; }
  void setEpdMode(m5gfx::epd_mode_t v) { mode = v; }
  void setFont(const int *) {}
  void setTextSize(float) {}
  void setTextWrap(bool) {}
  void setTextDatum(int) {}
  void setTextColor(int fg, int) { color = fg; }
  void setCursor(int xx, int yy) { x=xx; y=yy; }
  int textWidth(const char *s) { return static_cast<int>(std::strlen(s))*8; }
  void setClipRect(int xx,int yy,int w,int h) { clip={xx,yy,w,h}; }
  void clearClipRect() { clip={0,0,540,960}; }
  void fillRect(int xx,int yy,int w,int h,int c) {
    int l=std::max(xx,clip.x),t=std::max(yy,clip.y);
    int r=std::min(xx+w,clip.x+clip.w),b=std::min(yy+h,clip.y+clip.h);
    if (r<=l || b<=t) return;
    dirty=unite(dirty,{l,t,r-l,b-t});
    for(int py=t;py<b;++py) for(int px=l;px<r;++px) pixels[py*540+px]=c;
  }
  void fillScreen(int c) { ++fills; fillRect(0,0,540,960,c); }
  void drawFastHLine(int xx,int yy,int w,int c) { fillRect(xx,yy,w,1,c); }
  void print(const char *s) { fillRect(x,y,textWidth(s),16,color); }
  void printf(const char *fmt, ...) {
    char buf[512]; va_list args; va_start(args,fmt);
    vsnprintf(buf,sizeof(buf),fmt,args); va_end(args); print(buf);
  }
  void display(int xx=0,int yy=0,int w=0,int h=0) {
    Rect request{xx,yy,w,h};
    if(w && h) dirty=unite(dirty,request);
    frames.push_back({mode,dirty,request}); dirty={0,0,0,0};
  }
  void waitDisplay() { clock_us += 1000; }
};
static struct { Display Display; } M5;
static struct { std::mutex mutex; struct { unsigned font_size=24; } config; int page=0; } model;
static Position position;
static int reads=0, saves=0;
static std::string read_file(const std::string &) {
  ++reads; return R"({"content_updated_at":"2026-10-25T00:00:00Z"})";
}
static void display_marker(bool, const std::string & = "") {}
static void save_position(const Position &) { ++saves; }
static int64_t esp_timer_get_time() { return clock_us; }
static void log_message(const char *, const char *, ...) {}
#define ESP_LOGI(...) log_message(__VA_ARGS__)
#include "refresh_under_test.hpp"
static void expect_last(m5gfx::epd_mode_t mode, Rect dirty) {
  const auto &frame=M5.Display.frames.back();
  assert(frame.mode==mode && same(frame.dirty,dirty));
}
int main() {
  auto &d=M5.Display;
  const Rect whole{0,0,540,960};
  const auto full=m5gfx::epd_mode_t::epd_text, fast=m5gfx::epd_mode_t::epd_fast;
  std::vector<std::string> menu{"One","Two","Three"};
  draw_menu(menu,0,"Menu"); expect_last(full,whole);
  auto n=d.frames.size(); int fills=d.fills;
  draw_menu(menu,0,"Menu"); assert(d.frames.size()==n);
  draw_menu(menu,1,"Menu"); expect_last(fast,{24,112,492,122});
  assert(d.fills==fills);
  std::vector<Source> sources{{"feishu","Docs",449},{"feed","RSS",50}};
  draw_sources(sources,0); expect_last(full,whole);
  fills=d.fills;
  draw_sources(sources,1); expect_last(fast,{24,112,492,168});
  assert(d.fills==fills);
  n=d.frames.size(); draw_sources(sources,1); assert(d.frames.size()==n);
  std::vector<Entry> entries;
  for(int i=0;i<16;++i) entries.push_back({"feed",std::to_string(i),"Article "+std::to_string(i),i==0?"body":""});
  draw_articles(entries,0,"RSS",16); expect_last(full,whole);
  fills=d.fills;
  draw_articles(entries,1,"RSS",16); expect_last(fast,{24,112,492,168});
  assert(d.fills==fills);
  n=d.frames.size(); entries[12].title="Offscreen update"; entries[12].snapshot_id="new";
  draw_articles(entries,1,"RSS",16); assert(d.frames.size()==n);
  entries[0].snapshot_id="new-version-same-visible-marker";
  draw_articles(entries,1,"RSS",16); assert(d.frames.size()==n);
  entries[1].snapshot_id="now-cached";
  draw_articles(entries,1,"RSS",16); expect_last(full,whole);
  draw_articles(entries,8,"RSS",16); expect_last(full,whole);
  draw_menu(menu,1,"Menu"); expect_last(full,whole);
  Entry entry{"feed","doc","Title","snapshot"};
  std::vector<Page> pages{{{{"First",94,false}},"p0",0},
                          {{{"Second",94,false}},"p1",10},
                          {{{"Third",94,false}},"p2",20}};
  draw_page(entry,pages,0); expect_last(full,whole); assert(reads==1);
  auto header=d.pixels;
  fills=d.fills;
  draw_page(entry,pages,1); expect_last(fast,{0,63,540,897});
  assert(d.fills==fills && reads==1);
  assert(std::equal(header.begin(),header.begin()+540*63,d.pixels.begin()));
  n=d.frames.size(); int saved=saves;
  draw_page(entry,pages,1); assert(d.frames.size()==n && saves==saved);
  entry.source_id="other";
  draw_page(entry,pages,2); expect_last(full,whole);
  model.config.font_size=32;
  draw_page(entry,pages,1); expect_last(full,whole);
  set_display_partial_limit(3);
  draw_page(entry,pages,0); draw_page(entry,pages,1); draw_page(entry,pages,2);
  assert(display_partial_count()==3 && quality_cleanup_due());
  request_quality_cleanup(); draw_page(entry,pages,2);
  expect_last(m5gfx::epd_mode_t::epd_quality,whole);
  assert(display_partial_count()==0 && !quality_cleanup_due());
  draw_page(entry,pages,1); expect_last(fast,{0,63,540,897});
}
