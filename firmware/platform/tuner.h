// Tuner — the TV's "state of reception". Cycles: locked → losing the signal →
// lost (snow with the picture barely ghosting through, rolling) → hunting back
// in with the knob → locked. Pure: takes time, gives Glitch params + brightness.
#pragma once
#include <stdint.h>
#include "platform/glitch.h"

enum TunerState { TUNER_LOCKED, TUNER_LOSING, TUNER_LOST, TUNER_TUNING };

struct TunerOut {
  Glitch  g;
  uint8_t brightness;   // panel brightness 0..255 (sag + flicker)
  TunerState state;
  float   detune;       // 0 = clean, 1 = gone
};

void tunerInit(uint32_t nowMs);
void tunerStart(uint32_t nowMs, TunerState st);   // begin in a given state (e.g. TUNING after power-on)
void tunerKick(uint32_t nowMs);                    // "tune" knob pressed: fall off-tune and hunt back in
void tunerSetAuto(bool on);                        // auto-cycle through losing/lost (default off)
void tunerDrop(uint32_t nowMs);                    // programme ended: drift off tune (LOSING → LOST, stays LOST)
void tunerTick(uint32_t nowMs, TunerOut* out);
void tunerJolt(uint32_t nowMs, float k);          // a knock hit the cabinet: k 0..1 -> the raster jumps, shivers, snows and dims for ~0.6 s
