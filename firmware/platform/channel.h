// A channel is a plugin that paints a clean, "as broadcast" logical 240×240
// picture. It is a pure function of (x, y) so the glitch layer below can sample
// it anywhere: rolled, bent, shifted. Analog degradation is NOT the channel's
// job. Channels draw "logical up"; rotation lives in the panel.
//
// This is the contract described in full in docs/CHANNELS.md — read that
// first if you are writing a channel; this header is what it compiles to.
#pragma once
#include <stdint.h>
#include "platform/hw/display.h"

// Logical picture is 240×240: 240 scan lines like a real (PAL-ish) tube, each
// drawn as two physical rows (the second darker = scanline structure), each
// logical pixel two physical wide (analog horizontal softness). Also 4× cheaper.
constexpr int LOG_W = 240;
constexpr int LOG_H = 240;

// A channel paints one logical ROW at a time: LOG_W (240) RGB565 pixels, or
// LCD_W (480) when its ChannelInfo::flags has CH_WIDE — see below.
typedef void (*RowFn)(int y, uint16_t* dst);

struct Sound;   // platform/sound.h — the argument to Channel::sound

enum ChannelFlags : uint8_t {
  CH_GAME      = 1 << 0,   // the set's physical keys belong to you: IO10 -> IN_A, BOOT -> IN_B
  CH_WIDE      = 1 << 1,   // row() fills 480 panel pixels instead of 240 (sources wider than 240)
  CH_ENDLESS   = 1 << 2,   // never drift off by yourself; length() is ignored
  CH_MISBEHAVE = 1 << 3,   // wakes the set in snow, waits for a knock, drifts off on its own
};

struct ChannelInfo {
  const char* id;        // "pong" — folder name, URL, CLI name
  const char* title;     // "Pong"
  const char* blurb;     // one line for the channel list: "Your phone is the paddle."
  uint8_t     flags;     // ChannelFlags
  const char* panel;     // JSON: which blocks the phone shows (see docs/REMOTE.md)
};

struct Channel {
  ChannelInfo info;
  void     (*begin)();                         // once at boot: build tables, no hardware
  void     (*start)(uint32_t nowMs);           // the set locked onto you: reset, claim buffers
  void     (*stop)();                          // tuned away / powered off: release buffers
  void     (*tick)(uint32_t nowMs);            // once per frame, before painting: input + state
  void     (*row)(int y, uint16_t* dst);       // paint logical row y (0..239) — or 0..479 wide, see CH_WIDE
  void     (*sound)(uint32_t nowMs, Sound* s); // optional (nullptr): tones for this frame
  uint32_t (*length)();                        // optional (nullptr): programme length in ms; 0 = endless
};

// Chroma gain 0..16 the glitch/tuner layer asks for; channels honour it when
// rebuilding their palettes in tick() (16 = full colour, 0 = monochrome).
extern int gChroma;
