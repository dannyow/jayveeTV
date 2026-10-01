// Test card — PM5544-flavoured: grid, big circle, castellations, colour bars,
// greyscale, frequency gratings, station ident. Pure function of (x, y).
#include "channels/testcard/testcard.h"
#include "platform/font5x7.h"
#include "platform/sound.h"
#include <math.h>

#define RGB(r,g,b) ((uint16_t)((((r)&0xF8)<<8) | (((g)&0xFC)<<3) | ((b)>>3)))

static const uint16_t C_BLACK = 0x0000, C_WHITE = 0xFFFF;
static const uint16_t C_GRID_BG = RGB(64,64,64);
static const uint8_t  BARS_RGB[8][3] = { {235,235,235}, {235,235,0}, {0,235,235}, {0,235,0},
                                         {235,0,235},   {235,0,0},   {0,0,235},   {16,16,16} };
static uint16_t BARS[8];                          // live palette (desaturated by gChroma)
static int      s_chromaBuilt = -1;

static const int CX = 120, CY = 120, R_OUT = 113, R_IN = 110;
static int16_t  s_hwOut[LOG_H], s_hwIn[LOG_H];   // circle half-widths per row

static int16_t halfWidth(int dy, int r) {
  if (dy < -r || dy > r) return -1;
  return (int16_t)sqrtf((float)(r*r - dy*dy));
}

static void buildPalette() {
  for (int i = 0; i < 8; i++) {
    int r = BARS_RGB[i][0], g = BARS_RGB[i][1], b = BARS_RGB[i][2];
    int l = (r * 77 + g * 150 + b * 29) >> 8;
    r = l + ((r - l) * gChroma) / 16; g = l + ((g - l) * gChroma) / 16; b = l + ((b - l) * gChroma) / 16;
    BARS[i] = RGB(r, g, b);
  }
  s_chromaBuilt = gChroma;
}

static void tcBegin() {
  for (int y = 0; y < LOG_H; y++) { s_hwOut[y] = halfWidth(y - CY, R_OUT); s_hwIn[y] = halfWidth(y - CY, R_IN); }
  buildPalette();
}

static void tcTick(uint32_t) { if (gChroma != s_chromaBuilt) buildPalette(); }

struct Text { int x, y, scale; const char* s; uint16_t col; int w; };
static const Text TEXTS[] = {
  { 120 - (4*6*2)/2, 11, 2, "CH 1", C_WHITE, 4*6*2 },        // top ident = orientation marker
  { 120 - (8*6*2)/2, 154, 2, "@dannyow", C_WHITE, 8*6*2 },   // ident band: Daniel's X handle
  { 120 - (9*6*1)/2, 224, 1, "TEST CARD", RGB(210,210,210), 9*6*1 },
};

// Clock: the PM5544's second black box, below the ident band. HH:MM in ×2
// font, white, in a black box that replaces the centre cross while a time is
// known. The platform pushes the time once a second — no I2C from here.
static char s_clock[6] = "";                                  // "HH:MM" or "" = no clock
static const int CLK_X0 = 84, CLK_X1 = 156;                       // box spans the cross row (200..220)
static const Text CLOCK_TEXT = { 120 - (5*6*2 - 2)/2, 203, 2, s_clock, C_WHITE, 5*6*2 - 2 };
void testcardSetTime(int h, int m) {
  if (h < 0 || h > 23 || m < 0 || m > 59) { s_clock[0] = 0; return; }
  s_clock[0] = '0' + h / 10; s_clock[1] = '0' + h % 10; s_clock[2] = ':';
  s_clock[3] = '0' + m / 10; s_clock[4] = '0' + m % 10; s_clock[5] = 0;
}

static inline bool glyphPixel(const Text& t, int x, int y, uint16_t* out) {
  if (y < t.y || y >= t.y + 7 * t.scale || x < t.x || x >= t.x + t.w) return false;
  int gx = (x - t.x) / t.scale, gy = (y - t.y) / t.scale;
  int ci = gx / 6, col = gx % 6;
  if (col == 5) return false;
  if ((font5x7Glyph(t.s[ci])[col] >> gy) & 1) { *out = t.col; return true; }
  return false;
}
static inline bool textPixel(int x, int y, uint16_t* out) {
  for (const Text& t : TEXTS) {
    if (y < t.y || y >= t.y + 7 * t.scale || x < t.x || x >= t.x + t.w) continue;
    return glyphPixel(t, x, y, out);
  }
  return false;
}

static inline uint16_t tcPixel(int x, int y) {
  uint16_t t;
  if (textPixel(x, y, &t)) return t;
  int dx = x - CX;
  int adx = dx < 0 ? -dx : dx;
  int16_t hwo = s_hwOut[y], hwi = s_hwIn[y];
  if (hwo < 0 || adx > hwo) {                 // outside the circle: grid
    return ((x % 20) < 1 || (y % 20) < 1) ? C_WHITE : C_GRID_BG;
  }
  if (adx > hwi) return C_WHITE;               // the ring
  if (y < 30)  return (y < 4 || ((x / 20) & 1)) ? RGB(90,90,90) : RGB(40,40,40);
  if (y < 50)  return ((x / 10) & 1) ? C_WHITE : C_BLACK;             // castellation
  if (y < 110) { int i = (x - 20) / 25; if (i < 0) i = 0; if (i > 7) i = 7; return BARS[i]; }
  if (y < 150) { int i = (x - 20) / 25; if (i < 0) i = 0; if (i > 7) i = 7; int v = i * 36; return RGB(v,v,v); }
  if (y < 170) return C_BLACK;                                         // ident band
  if (y < 200) {                                                       // gratings
    static const uint8_t PITCH[5] = { 1, 2, 3, 4, 6 };
    int z = (x - 20) / 40; if (z < 0) z = 0; if (z > 4) z = 4;
    return ((x / PITCH[z]) & 1) ? C_WHITE : C_BLACK;
  }
  if (y < 220) {                                                       // centre cross + grid
    if (s_clock[0] && x >= CLK_X0 && x < CLK_X1) {                     // clock box (PM5544's lower box)
      uint16_t t; return glyphPixel(CLOCK_TEXT, x, y, &t) ? t : C_BLACK;
    }
    if (adx < 1) return C_WHITE;
    return ((x % 20) < 1 || (y % 20) < 1) ? C_WHITE : C_GRID_BG;
  }
  return RGB(40,40,40);
}

static void tcRow(int y, uint16_t* dst) { for (int x = 0; x < LOG_W; x++) dst[x] = tcPixel(x, y); }

// The 1 kHz test tone belongs to a locked card; it warbles off-tune and fades
// as the signal drifts. Platform-supplied detune replaces the old special
// case in main.cpp's soundFollow() (see docs/CHANNELS.md "Sound").
static void tcSound(uint32_t now, Sound* s) {
  float d = s->detune;
  float lock = (1.0f - d) * (1.0f - d);
  s->tone  = true;
  s->hz    = 1000.0f + 60.0f * d * sinf(now * 0.011f);
  s->level = 0.45f * lock;
}

const Channel CH_TESTCARD = {
  { "testcard", "Test Card", "Colour bars, grid, the station ident.", CH_ENDLESS | CH_MISBEHAVE, "{\"blocks\":[]}" },
  tcBegin, nullptr, nullptr, tcTick, tcRow, tcSound, nullptr
};
