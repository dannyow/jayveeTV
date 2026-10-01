#include "platform/sound.h"

void soundBeep(Sound* s, float hz, uint32_t ms) {
  s->tone = true; s->hz = hz; s->level = 1.0f; (void)ms;
}

void soundPcm(Sound*, const int8_t*, int) {
  // stub: PCM soundtrack (films, emulators) — no release-1 channel uses it.
}
