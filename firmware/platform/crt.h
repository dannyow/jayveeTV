// CRT tube model — power on / off behaviour of the "picture tube".
// Off → warm-up (heater wait, dim line opens up with bloom) → on.
// On → collapse (vertical deflection dies: bright line, then a dot) → afterglow → off.
// Pure: takes time, gives geometry + brightness for the glitch layer.
#pragma once
#include <stdint.h>

enum CrtState { CRT_OFF, CRT_WARMUP, CRT_ON, CRT_COLLAPSE, CRT_AFTERGLOW };

struct CrtOut {
  CrtState state;
  float vScale;     // 1 = full height; → 0 collapses to a line at centre
  float hScale;     // 1 = full width;  → 0 collapses the line to a dot
  float bright;     // 0..1 panel brightness multiplier
  int   boost;      // 0..2 extra gain while the beam is compressed (energy piles up)
  float dot;        // 0..1 afterglow dot intensity (0 = none)
  float dotR;       // dot radius in logical px
  bool  raster;     // false = nothing but the dot / black
};

void crtInit(bool on);
void crtPowerOn(uint32_t nowMs);
void crtPowerOff(uint32_t nowMs);
bool crtIsOn();                    // true from power-on until collapse starts
void crtTick(uint32_t nowMs, CrtOut* out);
