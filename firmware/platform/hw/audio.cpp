// I2S + ES8311 bring-up lifted from claude-desktop-buddy-esp32/src/hw/audio.cpp
// (legacy i2s driver: allocates DMA in internal RAM). Synth is ours.
#include "platform/hw/audio.h"
#include "platform/board.h"
#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/i2c.h>
#include <driver/i2s.h>
#include <math.h>
extern "C" {
#include "es8311.h"
}

static constexpr int SR = 32000;
static volatile int s_master = 32767;              // software master gain, Q15 (see audioVolume)             // 15.6 kHz whistle needs > 16 kHz Nyquist
static constexpr int CHUNK = 256;

static es8311_handle_t s_codec = nullptr;
static volatile AudioMix s_target = { 0, 1000, 0, 0, 0, 0 };

// Clip ring: 8192 samples = 0.5 s at 16 kHz. Single producer (main), single consumer (synth).
static constexpr int RING = 8192;
static int8_t  s_ring[RING];
static volatile uint32_t s_ringW = 0, s_ringR = 0;    // monotonically increasing indices
int  audioClipFree() { return RING - (int)(s_ringW - s_ringR); }
int  audioClipWrite(const int8_t* src, int n) {
  int room = audioClipFree(); if (n > room) n = room;
  for (int i = 0; i < n; i++) s_ring[(s_ringW + i) & (RING - 1)] = src[i];
  s_ringW += n; return n;
}
void audioClipReset() { s_ringR = s_ringW; }
static volatile int s_pendingEvent = -1;

static int16_t s_sine[1024];
static inline int16_t sineAt(uint32_t ph) { return s_sine[ph >> 22]; }       // 32-bit phase → 1024 table
static uint32_t s_rng = 0xA5A5F00D;
static inline int32_t noise() { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return (int32_t)(s_rng >> 16) - 32768; }
static inline uint32_t phaseInc(float hz) { return (uint32_t)(hz * 4294967296.0f / SR); }

static bool i2sInit() {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = SR;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 4;
  cfg.dma_buf_len = CHUNK;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;
  cfg.fixed_mclk = SR * 256;
  cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr) != ESP_OK) return false;
  i2s_pin_config_t pins = {};
  pins.mck_io_num = PIN_I2S_MCLK; pins.bck_io_num = PIN_I2S_BCLK; pins.ws_io_num = PIN_I2S_WS;
  pins.data_out_num = PIN_I2S_DO; pins.data_in_num = I2S_PIN_NO_CHANGE;
  return i2s_set_pin(I2S_NUM_0, &pins) == ESP_OK;
}

static bool codecInit() {
  s_codec = es8311_create((i2c_port_t)0, ES8311_ADDRRES_0);
  if (!s_codec) { Serial.println("audio: es8311_create failed"); return false; }
  const es8311_clock_config_t clk = { false, false, true, SR * 256, SR };
  if (es8311_init(s_codec, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) return false;
  if (es8311_sample_frequency_config(s_codec, clk.mclk_frequency, clk.sample_frequency) != ESP_OK) return false;
  es8311_microphone_config(s_codec, false);
  es8311_voice_volume_set(s_codec, 85, nullptr);
  return true;
}

// Fixed point throughout the sample loop: the C6 has no FPU and 32 k samples/s
// of software floats cost ~4 fps of picture. Levels are Q15 (0..32767).
static inline int32_t q15(float v) { if (v < 0) v = 0; if (v > 1) v = 1; return (int32_t)(v * 32767.0f); }

static void synthTask(void*) {
  int16_t buf[CHUNK];
  uint32_t phTone = 0, phHum = 0, phHum2 = 0, phWhine = 0, phThunk = 0;
  int32_t tone = 0, hiss = 0, hum = 0, whine = 0, clipL = 0; // Q15, smoothed
  int16_t clipS = 0; bool clipOdd = false;                   // 16→32 kHz: each sample twice
  float toneHz = 1000;
  int32_t thunk = 0, click = 0;                              // Q15 envelopes
  int32_t whineGlideQ = 32767, whineDropQ = 0;               // Q15
  int32_t lp = 0;                                            // hiss lowpass state
  while (true) {
    // Per-chunk (8 ms) housekeeping may use floats; the inner loop may not.
    int32_t tT = q15(s_target.tone), tH = q15(s_target.hiss), tM = q15(s_target.hum), tW = q15(s_target.whine), tC = q15(s_target.clip);
    tone += (tT - tone) * 3 / 10; hiss += (tH - hiss) * 3 / 10; hum += (tM - hum) * 3 / 10; whine += (tW - whine) * 3 / 10; clipL += (tC - clipL) * 3 / 10;
    toneHz += (s_target.toneHz - toneHz) * 0.5f;
    int ev = s_pendingEvent;
    if (ev >= 0) {
      s_pendingEvent = -1;
      if (ev == AUDIO_EV_POWER_ON)  { thunk = q15(0.9f); click = q15(0.5f); whineGlideQ = 32767; whineDropQ = 0; }
      if (ev == AUDIO_EV_POWER_OFF) { click = q15(0.7f); whineDropQ = 32767; }
    }
    if (whineDropQ > 0) { whineGlideQ = whineGlideQ * 985 / 1000; whineDropQ = whineDropQ * 97 / 100; if (whineDropQ < 600) { whineDropQ = 0; whineGlideQ = 32767; } }
    uint32_t iTone = phaseInc(toneHz), iHum = phaseInc(50), iHum2 = phaseInc(100), iThunk = phaseInc(55);
    uint32_t iWhine = phaseInc(15625.0f * (whineGlideQ / 32767.0f));
    int32_t whineNow = whineDropQ > 0 ? whineDropQ * 6 / 10 : whine;
    // Source gains (Q15 × level). Sum can exceed full scale; the soft clip below
    // handles that the way a small overdriven speaker would.
    int32_t gTone = tone, gHiss = hiss * 12 / 10, gHum = hum, gWhine = whineNow >> 1, gThunk = thunk * 12 / 10, gClick = click, gClip = clipL;
    for (int i = 0; i < CHUNK; i++) {
      int32_t s = 0;
      s += (sineAt(phTone) * gTone) >> 15;                                   phTone += iTone;
      int32_t n = noise(); lp += (n - lp) >> 2;
      s += (lp * gHiss) >> 15;
      s += (((sineAt(phHum) * 7 + sineAt(phHum2) * 3) / 10) * gHum) >> 15;    phHum += iHum; phHum2 += iHum2;
      s += (sineAt(phWhine) * gWhine) >> 15;                                 phWhine += iWhine;
      if (gClip > 30) {
        if (!clipOdd) { clipS = (s_ringR != s_ringW) ? (int16_t)(s_ring[s_ringR & (RING - 1)] << 8) : 0; if (s_ringR != s_ringW) s_ringR++; }
        clipOdd = !clipOdd;
        s += (clipS * gClip) >> 15;
      }
      if (gThunk > 30) { s += (sineAt(phThunk) * gThunk) >> 15; phThunk += iThunk; }
      if (gClick > 30) s += (noise() * gClick) >> 15;
      // Soft clip: linear to ±20000, then compress the rest 4:1, hard limit at the rails.
      if (s > 20000) s = 20000 + ((s - 20000) >> 2); else if (s < -20000) s = -20000 + ((s + 20000) >> 2);
      if (s > 32767) s = 32767; if (s < -32768) s = -32768;
      s = (s * s_master) >> 15;                       // master gain (see audioVolume)
      buf[i] = (int16_t)s;
    }
    thunk = thunk * 975 / 1000; click = click * 8 / 10;
    size_t written;
    i2s_write(I2S_NUM_0, buf, sizeof(buf), &written, portMAX_DELAY);
  }
}

bool hwAudioInit() {
  for (int i = 0; i < 1024; i++) s_sine[i] = (int16_t)(32767.0f * sinf(i * 6.2831853f / 1024));
  if (!i2sInit())   { Serial.println("audio: I2S init failed"); return false; }
  if (!codecInit()) { Serial.println("audio: ES8311 init failed"); return false; }
  xTaskCreate(synthTask, "synth", 4096, nullptr, 5, nullptr);
  return true;
}

void audioSetMix(const AudioMix& m) { s_target.tone = m.tone; s_target.toneHz = m.toneHz; s_target.hiss = m.hiss; s_target.hum = m.hum; s_target.whine = m.whine; s_target.clip = m.clip; }
void audioEvent(AudioEvent e) { s_pendingEvent = (int)e; }
// Volume: the codec's percent scale is not dB and 20 % was still loud on the bench
// (lab bring-up, 2026-09-27). The codec stays at a fixed 0 dB-ish level; a software
// master gain, quadratic in the percentage (20 % -> -28 dB, 50 % -> -12 dB), does the work.
void audioVolume(int pct) {
  if (pct < 0) pct = 0; if (pct > 100) pct = 100;
  // Loudness is heard in decibels: the slider is linear in dB over 36 dB (100 = full, 50 =
  // -18 dB, 1 = -36 dB), 0 = silent. Squaring the percentage (as before) left the top half
  // sounding the same and dropped off a cliff below 20.
  s_master = pct == 0 ? 0 : (int)(32767.0f * powf(10.0f, (pct - 100) * 0.36f / 20.0f));
  if (s_codec) es8311_voice_volume_set(s_codec, pct == 0 ? 0 : 70, nullptr);
}
