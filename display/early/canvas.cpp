#include "early/canvas.h"
#include "early/common.h"

#include <ctype.h>
#include <string.h>

namespace bootscope {

const char* const kFontChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789./:-_=+@>%?";
const char* const kFontGlyphs[] = {
    "0E11111F111111", "1E11111E11111E", "0E11101010110E", "1E11111111111E",
    "1F10101E10101F", "1F10101E101010", "0E11101711110F", "1111111F111111",
    "0E04040404040E", "0101010111110E", "11121418141211", "1010101010101F",
    "111B1515111111", "11191513111111", "0E11111111110E", "1E11111E101010",
    "0E11111115120D", "1E11111E141211", "0F10100E01011E", "1F040404040404",
    "1111111111110E", "11111111110A04", "11111115151B11", "11110A040A1111",
    "11110A04040404", "1F01020408101F",
    "0E11131519110E", "040C040404040E", "0E11010204081F", "1E01010E01011E",
    "02060A121F0202", "1F10101E01011E", "0E10101E11110E", "1F010204080808",
    "0E11110E11110E", "0E11110F01010E",
    "00000000000C0C", "01020204080810", "000C0C000C0C00", "0000001F000000",
    "0000000000001F", "00001F001F0000", "0004041F040400", "0E11171517100E",
    "10080402040810", "18190204081303", "0E110102040004"
};

static int g_buf_w = kPortraitWidth;
static int g_buf_h = kPortraitHeight;

void set_canvas_bounds(int w, int h) {
    g_buf_w = w;
    g_buf_h = h;
}

int canvas_width() {
    return g_buf_w;
}

int canvas_height() {
    return g_buf_h;
}

void fill_rect(uint32_t* px, int stride, int left, int top, int right, int bottom, uint32_t color) {
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > g_buf_w) right = g_buf_w;
    if (bottom > g_buf_h) bottom = g_buf_h;
    for (int y = top; y < bottom; y++) {
        uint32_t* row = px + y * stride;
        for (int x = left; x < right; x++) {
            row[x] = color;
        }
    }
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}

void draw_text(uint32_t* px, int stride, const char* text, int x, int y, int scale, uint32_t color) {
    if (!text) return;
    const size_t n_chars = strlen(kFontChars);
    const size_t fallback = n_chars - 1;
    for (size_t i = 0; text[i] != '\0'; i++, x += scale * 6) {
        char ch = (char)toupper((unsigned char)text[i]);
        if (ch == ' ') continue;
        const char* found = strchr(kFontChars, ch);
        size_t idx = found ? (size_t)(found - kFontChars) : fallback;
        const char* glyph = kFontGlyphs[idx];
        for (int row = 0; row < 7; row++) {
            int bits = hex_val(glyph[row * 2]) * 16 + hex_val(glyph[row * 2 + 1]);
            for (int col = 0; col < 5; col++) {
                if ((bits & (16 >> col)) == 0) continue;
                int left = x + col * scale;
                int top = y + row * scale;
                fill_rect(px, stride, left, top, left + scale, top + scale, color);
            }
        }
    }
}

}  // namespace bootscope
