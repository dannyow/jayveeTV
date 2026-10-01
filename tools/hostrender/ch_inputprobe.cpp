// inputprobe — a HOST-ONLY channel that shows what input.h is delivering: a
// 4×7 grid of the 28 keys (lit while held, player 0 left half of each cell,
// player 1 right half), the knob axis as a bar, and a counter of events polled.
// It is the reference for how a game channel consumes input in tick():
// drain inputPoll() for edges, read inputDown()/inputAxis() for held state.
// row() is integer-only, as the platform rule demands.
#include "platform/channel.h"
#include "platform/input.h"

extern const Channel CH_INPUTPROBE;   // a namespace-scope const has internal linkage without this

static int      s_events = 0;
static int16_t  s_knob[INPUT_PLAYERS];
static uint8_t  s_down[INPUT_PLAYERS][IN_KEY_COUNT];
static uint16_t s_on, s_off, s_bar, s_bg;

static void ipBegin() { s_events = 0; }
static void ipTick(uint32_t) {
  InputEvent e;
  while (inputPoll(&e)) s_events++;                       // edges: a game would act on these
  for (int p = 0; p < INPUT_PLAYERS; p++) {
    for (int k = 0; k < IN_KEY_COUNT; k++) s_down[p][k] = inputDown((uint8_t)k, (uint8_t)p);
    s_knob[p] = inputAxis(IN_KNOB, (uint8_t)p);
  }
  int c = gChroma;                                        // honour chroma like every channel
  s_on  = (uint16_t)(((0x1F * c / 16) << 11) | (0x3F << 5) | (0x1F * (16 - c) / 16));   // yellow → white as chroma drains
  s_off = 0x2104; s_bg = 0x0000;
  s_bar = (uint16_t)((0x1F << 11) | ((0x3F * (16 - c) / 16) << 5) | (0x1F * (16 - c) / 16));  // red → white
}
static void ipRow(int y, uint16_t* dst) {
  for (int x = 0; x < LOG_W; x++) dst[x] = s_bg;
  // Key grid: 7 columns × 4 rows of 30×30 cells starting at (15, 20).
  if (y >= 20 && y < 140) {
    int r = (y - 20) / 30, iy = (y - 20) % 30;
    if (iy >= 2 && iy < 28) for (int col = 0; col < 7; col++) {
      int k = r * 7 + col + 1; if (k >= IN_KEY_COUNT) break;
      int x0 = 15 + col * 30;
      for (int ix = 2; ix < 28; ix++) {
        int p = ix < 15 ? 0 : 1;
        dst[x0 + ix] = s_down[p][k] ? s_on : s_off;
      }
    }
  }
  // Knob bars: one per player, centre = 0, full scale ±32767 → ±100 px.
  for (int p = 0; p < INPUT_PLAYERS; p++) {
    int by = 160 + p * 20;
    if (y >= by && y < by + 12) {
      int len = s_knob[p] / 328;                          // -100..100
      int a = 120, b = 120 + len; if (a > b) { int t = a; a = b; b = t; }
      for (int x = a; x <= b && x < LOG_W; x++) if (x >= 0) dst[x] = s_bar;
      dst[120] = 0xFFFF;
    }
  }
  // Event counter: a 2 px tick per polled event along the bottom (wraps at 100).
  if (y >= 220 && y < 226) for (int i = 0; i < (s_events % 100); i++) dst[20 + i * 2] = 0xFFFF;
}
const Channel CH_INPUTPROBE = { { "inputprobe", "Input Probe", "host-only input debug", 0, "{\"blocks\":[]}" },
                                 ipBegin, nullptr, nullptr, ipTick, ipRow, nullptr, nullptr };
