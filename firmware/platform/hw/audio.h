// Audio: ES8311 codec over I2S, a synth task streaming continuously. The
// platform only sets a mix (levels 0..1) each frame and fires events; the
// task never stutters under a heavy frame because it runs at higher priority.
#pragma once
#include <stdint.h>

struct AudioMix {
  float tone;      // channel-requested tone level (PM5544-style test tone, game beeps, ...)
  float toneHz;    // tone frequency
  float hiss;      // white-ish noise level (no signal)
  float hum;       // 50/100 Hz mains hum level
  float whine;     // 15.6 kHz line-output whistle level (faint, "the set is on")
  float clip;      // PCM soundtrack level (films, emulators — unused in release 1)
};

enum AudioEvent { AUDIO_EV_POWER_ON, AUDIO_EV_POWER_OFF };

bool hwAudioInit();
void audioSetMix(const AudioMix& m);
void audioEvent(AudioEvent e);
void audioVolume(int pct);   // 0..100 codec volume
// PCM soundtrack ring (s8 mono @ 16 kHz, played at 32 kHz by repetition).
// The platform tops it up once per frame; the synth task drains it, never blocks.
int  audioClipFree();                          // samples that can be written now
int  audioClipWrite(const int8_t* s, int n);   // returns samples accepted
void audioClipReset();                         // drop buffered soundtrack (channel change)
