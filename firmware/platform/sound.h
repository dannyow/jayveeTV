// Sound — what a channel asks for this frame. The platform mixes it over the
// set's own bed (hiss, hum, whistle, power thunk, knock thud), scales
// everything by the master volume, and fades a channel's own tone as the
// signal drifts — see docs/CHANNELS.md "Sound".
#pragma once
#include <stdint.h>

struct Sound {
  // Platform -> channel, set before sound() is called: the tuner's current
  // detune this frame, 0 = locked .. 1 = signal gone. A channel with its own
  // "belongs to the signal" tone (the test card's 1 kHz PM5544 tone) honours
  // this directly instead of the platform special-casing that channel.
  float detune;

  // Channel -> platform, this frame's request. Left at tone = false (the
  // default after a plain `Sound s = {};`): nothing asked, silence.
  bool  tone;
  float hz;      // Hz
  float level;   // 0..1, before the platform's own volume/warm-up scaling
};

// Ask for a tone this frame at full level. `ms` is informational — the
// platform does not time it; the caller does, by calling soundBeep() again
// next frame (or not). See channels/pong/pong.cpp for the pattern: remember
// an "until" timestamp in tick(), consult it in sound().
void soundBeep(Sound* s, float hz, uint32_t ms);

// PCM soundtrack (films, emulators) — a stub; no release-1 channel uses it.
void soundPcm(Sound* s, const int8_t* samples, int n);
