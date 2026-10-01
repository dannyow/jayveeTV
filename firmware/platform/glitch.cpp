#include "platform/glitch.h"
#include <math.h>

static uint16_t s_noise[4096];     // grainy grey snow, triangular distribution + flecks
static uint32_t s_rng = 0x9E3779B9u;
static inline uint32_t rnd() { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }

void glitchInit() {
  for (int i = 0; i < 4096; i++) {
    uint32_t r = rnd();
    int v = ((r & 0xFF) + ((r >> 8) & 0xFF)) >> 1;   // 0..255, peaked mid
    if ((r >> 16 & 0x3F) == 0) v = 255;                // occasional bright fleck
    v = 40 + v * 3 / 4;                                // bright grey hiss, never fully black
    s_noise[i] = (uint16_t)(((v & 0xF8) << 8) | ((v & 0xFC) << 3) | (v >> 3));
  }
}

static inline uint16_t mix(uint16_t a, uint16_t b, int k) {   // k 0..16 toward b
  uint32_t rb = ((a & 0xF81F) * (16 - k) + (b & 0xF81F) * k) >> 4;
  uint32_t g  = ((a & 0x07E0) * (16 - k) + (b & 0x07E0) * k) >> 4;
  return (uint16_t)((rb & 0xF81F) | (g & 0x07E0));
}

void glitchRender(uint16_t* dst, int y0, int rows, RowFn row, bool wide, const Glitch& g) {
  static uint16_t line[LCD_W], src[LCD_W];
  const int W = wide ? LCD_W : LOG_W, SH = wide ? 2 : 1;   // row width; Glitch displacements are logical px, a wide row is panel px = 2x
  const float cy = LOG_H * 0.5f, cx = W * 0.5f;
  for (int r = 0; r < rows; r += 2) {
    int y = (y0 + r) >> 1;                              // logical row
    uint16_t* rowA = dst + r * LCD_W;
    uint16_t* rowB = rowA + LCD_W;
    // Tube geometry: map this physical line back through the raster size.
    float fy = (y + 0.5f - cy) / g.vScale;
    bool outside = !g.raster || fy < -cy || fy >= cy || g.hScale <= 0.0f;
    if (outside) {
      for (int x = 0; x < LCD_W; x++) rowA[x] = rowB[x] = 0;
      if (g.dot > 0.001f) {                             // phosphor afterglow dot
        float dy = y + 0.5f - cy; float R2 = g.dotR * g.dotR;
        if (dy * dy < R2) for (int x = 0; x < LOG_W; x++) {
          float dx = x + 0.5f - cx; float d2 = dx * dx + dy * dy;
          if (d2 < R2) {
            float v = g.dot * (1.0f - d2 / R2); int iv = (int)(v * 255); if (iv > 255) iv = 255;
            uint16_t c = SWAP16((uint16_t)(((iv & 0xF8) << 8) | ((iv & 0xFC) << 3) | (iv >> 3)));
            rowA[2*x] = rowA[2*x+1] = rowB[2*x] = rowB[2*x+1] = c;   // the dot is drawn in logical units regardless of mode
          }
        }
      }
      continue;
    }
    int yg = (int)(fy + cy);                            // logical row in the unscaled raster
    // Source row after vertical roll; the seam carries the blanking bar.
    int ys = yg + g.roll; if (ys >= LOG_H) ys -= LOG_H;
    bool blank = g.blanking && g.roll != 0 && ys < 11;
    // Horizontal displacement: wobble + flagging at the top + knob + row jitter.
    float top = 1.0f - (float)ys / 70.0f; if (top < 0) top = 0;
    float sh = g.wobbleAmp * sinf(g.wobblePhase + ys * 0.05f) + g.bend * top * top + g.hshift;
    sh *= g.hScale * SH;                               // displacement in row units
    int jit = g.jitter ? ((int)(rnd() % (2 * g.jitter + 1)) - g.jitter) * SH : 0;
    int shift = (int)lrintf(sh) + jit;
    int nOff = rnd() & 4095;
    int k = g.snow;
    if (k < 16) row(ys, src);                            // the channel's clean row
    if (g.hScale >= 0.999f) {
      // Shifted copy of the row (black beyond the edges), then grain.
      int x0 = shift > 0 ? shift : 0, x1 = shift < 0 ? W + shift : W;
      if (k >= 16) for (int x = 0; x < W; x++) line[x] = s_noise[(x + nOff) & 4095];
      else {
        for (int x = 0; x < x0; x++) line[x] = 0;
        for (int x = x1; x < W; x++) line[x] = 0;
        if (k) for (int x = x0; x < x1; x++) line[x] = mix(src[x - shift], s_noise[(x + nOff) & 4095], k);
        else   for (int x = x0; x < x1; x++) line[x] = src[x - shift];
      }
    } else {                                            // horizontally compressed raster
      float inv = 1.0f / g.hScale;
      for (int x = 0; x < W; x++) {
        float fx = (x + 0.5f - cx) * inv;
        if (fx < -cx || fx >= cx) { line[x] = 0; continue; }
        int xg = (int)(fx + cx) - shift;
        uint16_t c = (k >= 16) ? s_noise[(xg + nOff) & 4095] : ((xg >= 0 && xg < W) ? src[xg] : 0);
        if (k && k < 16) c = mix(c, s_noise[(xg + nOff) & 4095], k);
        line[x] = c;
      }
    }
    // Per-row gain: soft hum bar, blanking; second physical row darker = scanline.
    int gain = g.boost;                                 // -3..+2
    if (g.humBar >= 0) { int d = yg - g.humBar; if (d >= 0 && d < 32) gain += (d < 6 || d >= 26) ? 1 : 2; }
    if (blank) gain = -3;
    if (gain > 2) gain = 2;
    for (int x = 0; x < W; x++) {
      uint16_t c = line[x];
      switch (gain) {
        case -3: c = (c >> 2) & 0x39E7; break;
        case -2: c = (c >> 1) & 0x7BEF; break;
        case -1: c -= (c >> 2) & 0x39E7; break;
        case  1: c += ((~c) >> 2) & 0x39E7; break;
        case  2: c += ((~c) >> 1) & 0x7BEF; break;
        default: break;
      }
      uint16_t cs = g.scanline ? (uint16_t)(c - ((c >> 2) & 0x39E7)) : c;   // -25% on the gap line
      c = SWAP16(c); cs = SWAP16(cs);                                          // panel byte order
      if (wide) { rowA[x] = c; rowB[x] = cs; }
      else { rowA[2*x] = rowA[2*x + 1] = c; rowB[2*x] = rowB[2*x + 1] = cs; }
    }
  }
}
