#include "platform/tuner.h"
#include <math.h>

int gChroma = 16;

static TunerState s_state = TUNER_LOCKED;
static uint32_t   s_stateT0 = 0, s_lastMs = 0;
static float      s_roll = 0;                      // rows, accumulates
static float      s_hum = -40;                     // hum bar top, rows
static uint32_t   s_rng = 0xC0FFEE;
static inline float frnd() { s_rng = s_rng * 1664525u + 1013904223u; return (s_rng >> 8) * (1.0f / 16777216.0f); }
static const uint32_t DUR[4] = { 9000, 5000, 4000, 9000 };   // ms per state

void tunerInit(uint32_t now) { s_state = TUNER_LOCKED; s_stateT0 = now; s_lastMs = now; }
void tunerStart(uint32_t now, TunerState st) { s_state = st; s_stateT0 = now; s_lastMs = now; s_roll = 0; }
static bool s_auto = false;
void tunerSetAuto(bool on) { s_auto = on; }
// Start hunting from partway through TUNING so the picture drops to ~0.75
// detune (heavy grain, rolling) rather than full snow, then comes back.
void tunerDrop(uint32_t now) { if (s_state == TUNER_LOCKED) { s_state = TUNER_LOSING; s_stateT0 = now; } }
static uint32_t s_joltT0 = 0; static float s_joltK = 0;
void tunerJolt(uint32_t now, float k) { if (k > 1) k = 1; if (k < 0) k = 0; if (now - s_joltT0 < 600 && s_joltK > k) return; s_joltT0 = now; s_joltK = k; }
void tunerKick(uint32_t now) { s_state = TUNER_TUNING; s_stateT0 = now - DUR[TUNER_TUNING] / 5; s_lastMs = now; }

static float smooth(float t) { return t * t * (3 - 2 * t); }

void tunerTick(uint32_t now, TunerOut* o) {
  float dt = (now - s_lastMs) * 0.001f; s_lastMs = now;
  uint32_t el = now - s_stateT0;
  if (el >= DUR[s_state]) {
    // Rule: LOCKED and LOST are resting states. They only move on by themselves
    // in the auto show. Otherwise LOCKED stays (gentle drift) until tunerDrop()
    // or the knob; LOST stays (snow, last picture ghosting) until the knob.
    bool resting = (s_state == TUNER_LOCKED || s_state == TUNER_LOST) && !s_auto;
    if (!resting) s_state = (TunerState)((s_state + 1) & 3);
    s_stateT0 = now; el = 0;
  }
  float p = (float)el / DUR[s_state];               // 0..1 through the state
  float ts = now * 0.001f;

  // Detune curve per state. Hunting adds a knob-turn wobble on the way back.
  float d;
  switch (s_state) {
    case TUNER_LOCKED: d = 0.04f + 0.03f * sinf(ts * 0.7f); break;
    case TUNER_LOSING: d = smooth(p); break;
    case TUNER_LOST:   d = 1.0f; break;
    default: {                                       // TUNING: come back in, overshooting
      float base = 1.0f - smooth(p);
      float hunt = 0.25f * sinf(ts * 2.3f) * (1.0f - p) * (1.0f - p);
      d = base + hunt; if (d < 0) d = 0; if (d > 1) d = 1;
    }
  }
  o->detune = d; o->state = s_state;

  Glitch& g = o->g;
  // Grain: barely there when locked, everything when lost. Picture still ghosts
  // through at 15/16 so "lost" isn't a dead screen.
  float sn = powf(d, 1.4f) * 15.5f;
  g.snow = (int)sn; if (g.snow > 15) g.snow = 15; if (d >= 0.999f) g.snow = 15;
  // Horizontal: wobble grows, then per-row jitter, then the top starts flagging.
  g.wobbleAmp   = 0.4f + 5.0f * d;
  g.wobblePhase = ts * 2.1f;
  g.jitter      = (int)(2.5f * d * d);
  g.bend        = 20.0f * d * sinf(ts * 1.7f);
  g.hshift      = (s_state == TUNER_TUNING) ? (int)(25.0f * d * sinf(ts * 2.3f)) : 0;
  // Vertical hold lets go past ~0.35 detune; roll speed rises with detune.
  float rollSpeed = d > 0.35f ? (d - 0.35f) * 130.0f : 0.0f;       // logical rows / s
  s_roll += rollSpeed * dt;
  if (rollSpeed == 0) { s_roll += (0 - s_roll) * fminf(1.0f, dt * 6.0f); if (fabsf(s_roll) < 0.5f) s_roll = 0; }
  while (s_roll >= LOG_H) s_roll -= LOG_H; while (s_roll < 0) s_roll += LOG_H;
  g.roll = (int)s_roll; g.blanking = true;
  // Hum bar drifts down slowly, always (mains hum is a fact of life).
  s_hum += (14.0f + 45.0f * d) * dt; if (s_hum > LOG_H) s_hum = -40;
  g.humBar = (int)s_hum;
  g.scanline = 1;
  g.vScale = 1; g.hScale = 1; g.boost = 0; g.raster = true; g.dot = 0; g.dotR = 0;
  // Colour drains before the picture does (chroma burst is the first to go).
  float chroma = 1.0f - d * 1.6f; if (chroma < 0) chroma = 0;
  gChroma = (int)(chroma * 16.0f + 0.5f);
  // Brightness sags and flickers with weak signal.
  float br = 255.0f - 50.0f * d - frnd() * 45.0f * d;
  o->brightness = (uint8_t)(br < 30 ? 30 : br);
  // Knock jolt: the raster jumps down and settles, shivers sideways, snows and dims,
  // all scaled by the knock strength and decaying over 0.6 s. A cabinet thump on a tube.
  if (now - s_joltT0 < 600 && s_joltK > 0) {
    float t = (now - s_joltT0) / 600.0f, e = (1 - t) * (1 - t) * s_joltK;
    o->g.roll    = (o->g.roll + (int)(e * 90 * (0.6f + 0.4f * sinf(t * 31.0f)))) % LOG_H; if (o->g.roll < 0) o->g.roll += LOG_H;
    o->g.hshift += (int)(e * 24 * sinf(t * 47.0f));
    o->g.jitter += (int)(e * 5);
    o->g.snow    = o->g.snow + (int)(e * 7) > 16 ? 16 : o->g.snow + (int)(e * 7);
    o->g.blanking = true;
    o->brightness = (uint8_t)(o->brightness * (1.0f - 0.45f * e));
  }
}
