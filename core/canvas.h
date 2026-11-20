#pragma once

#include <stdint.h>

namespace bootscope {

// Little-endian RGBA_8888 (0xAABBGGRR)
constexpr uint32_t kColorBg = 0xff241b10;       // #101b24
constexpr uint32_t kColorAccent = 0xffcae07b;   // #7be0ca
constexpr uint32_t kColorPrimary = 0xfff7f4ed;  // #edf4f7
constexpr uint32_t kColorMuted = 0xffd6c39e;    // #9ec3d6
constexpr uint32_t kColorTrack = 0xff42321e;    // #1e3242
constexpr uint32_t kColorError = 0xff7b8cff;    // #ff8c7b

extern const char* const kFontChars;
extern const char* const kFontGlyphs[];

void set_canvas_bounds(int w, int h);
int canvas_width();
int canvas_height();

void fill_rect(uint32_t* px, int stride, int left, int top, int right, int bottom, uint32_t color);
void draw_text(uint32_t* px, int stride, const char* text, int x, int y, int scale, uint32_t color);

}  // namespace bootscope
