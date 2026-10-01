// Glitch layer — analog TV degradation applied at blit time, between the
// channel's clean picture and the panel. Everything here is continuous:
// smooth bends, rolling, grain, sag. No block moves, no hard pixel specks.
#pragma once
#include <stdint.h>
#include "platform/channel.h"

struct Glitch {
  int   snow;        // 0..16  how much grain is mixed into the signal (16 = only snow)
  float wobbleAmp;   // px     slow sine h-wobble down the picture
  float wobblePhase; // rad
  int   jitter;      // px     per-row random h-jitter amplitude
  float bend;        // px     h-sync "flagging": the top of the picture leans sideways
  int   hshift;      // px     whole-picture h-offset (tuning knob hunting)
  int   roll;        // rows   v-hold loss: picture scrolled by this many rows (0..479)
  int   humBar;      // rows   top of a soft bright band drifting down (< 0 = none)
  int   scanline;    // 0..8   scanline darkness (eighths)
  bool  blanking;    // show the vertical blanking bar at the roll seam
  // Tube geometry (from the CRT model). 1/1 = normal raster.
  float vScale;      // vertical size about the centre; rows outside are black
  float hScale;      // horizontal size about the centre
  int   boost;       // 0..2 extra gain (compressed beam)
  bool  raster;      // false: no picture at all (only the dot, if any)
  float dot;         // 0..1 phosphor dot at the centre
  float dotR;        // logical px
};

void glitchInit();
// wide = the channel's ChannelInfo::flags has CH_WIDE: `row` fills LCD_W (480)
// panel pixels per call instead of LOG_W (240), no horizontal doubling.
void glitchRender(uint16_t* dst, int y0, int rows, RowFn row, bool wide, const Glitch& g);
