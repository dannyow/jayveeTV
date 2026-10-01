#include "platform/crt.h"
#include <math.h>

static CrtState s_state = CRT_OFF;
static uint32_t s_t0 = 0;
static uint32_t s_rng = 0xBEEF;
static inline float frnd() { s_rng = s_rng * 1664525u + 1013904223u; return (s_rng >> 8) * (1.0f / 16777216.0f); }
static float easeOut(float t) { return 1 - (1 - t) * (1 - t); }
static float easeIn(float t)  { return t * t; }
static float clamp01(float t) { return t < 0 ? 0 : (t > 1 ? 1 : t); }

void crtInit(bool on) { s_state = on ? CRT_ON : CRT_OFF; s_t0 = 0; }
void crtPowerOn(uint32_t now)  { if (s_state == CRT_OFF || s_state == CRT_AFTERGLOW) { s_state = CRT_WARMUP; s_t0 = now; } }
void crtPowerOff(uint32_t now) { if (s_state == CRT_ON || s_state == CRT_WARMUP) { s_state = CRT_COLLAPSE; s_t0 = now; } }
bool crtIsOn() { return s_state == CRT_ON || s_state == CRT_WARMUP; }

void crtTick(uint32_t now, CrtOut* o) {
  float t = (now - s_t0) * 0.001f;
  o->state = s_state; o->vScale = 1; o->hScale = 1; o->bright = 1; o->boost = 0; o->dot = 0; o->dotR = 0; o->raster = true;
  switch (s_state) {
    case CRT_OFF:
      o->raster = false; o->bright = 0; break;

    case CRT_WARMUP: {
      // 0–0.9 s: heater — nothing. 0.9–1.2: a dim line grows across the centre.
      // 1.2–2.2: the raster opens vertically, overshoots (bloom) and settles.
      // Brightness creeps up over 3 s with a nervous flicker. Done at 3.2 s.
      if (t < 0.9f) { o->raster = false; o->bright = 0; break; }
      float tl = clamp01((t - 0.9f) / 0.3f);
      float tv = clamp01((t - 1.2f) / 1.0f);
      o->hScale = 0.05f + 0.95f * easeOut(tl);
      float v = easeOut(tv);
      o->vScale = 0.02f + v * (1.0f + 0.12f * sinf(tv * 3.14159f));      // bloom past 1.0 then back
      if (o->vScale < 0.02f) o->vScale = 0.02f;
      o->boost = o->vScale < 0.3f ? 1 : 0;
      float b = 0.25f + 0.75f * clamp01((t - 0.9f) / 2.3f);
      b -= frnd() * 0.25f * (1.0f - clamp01((t - 1.0f) / 2.0f));         // flicker dies down
      o->bright = clamp01(b);
      if (t >= 3.2f) { s_state = CRT_ON; s_t0 = now; }
      break;
    }

    case CRT_ON: break;

    case CRT_COLLAPSE: {
      // 0–0.22 s: height dies, beam energy piles into a brightening line.
      // 0.22–0.42 s: the line shrinks to a dot. Then afterglow.
      float tv = clamp01(t / 0.22f);
      o->vScale = 1.0f - 0.99f * easeIn(tv); if (o->vScale < 0.01f) o->vScale = 0.01f;
      o->boost = o->vScale < 0.5f ? (o->vScale < 0.15f ? 2 : 1) : 0;
      if (t > 0.22f) { float th = clamp01((t - 0.22f) / 0.20f); o->hScale = 1.0f - 0.98f * easeIn(th); }
      if (t >= 0.42f) { s_state = CRT_AFTERGLOW; s_t0 = now; }
      break;
    }

    case CRT_AFTERGLOW: {
      // Phosphor dot: bright, shrinking, exponential fade over ~1.5 s.
      o->raster = false;
      o->dot = expf(-t * 2.2f) * (1.0f - clamp01((t - 1.2f) / 0.5f));
      o->dotR = 7.0f - 4.0f * clamp01(t / 1.2f);
      o->bright = 1;
      if (t >= 1.8f) { s_state = CRT_OFF; s_t0 = now; o->dot = 0; o->bright = 0; }
      break;
    }
  }
}
