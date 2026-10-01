// Host stub for <Arduino.h>. The pure render code never includes it; this
// exists so a channel that sneaks in millis()/random()/Serial still builds on
// the host. millis() is the harness's FAKE frame clock, not wall time.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern uint32_t hostNowMs;                       // set by the harness before each tick
inline uint32_t millis() { return hostNowMs; }
inline uint32_t micros() { return hostNowMs * 1000u; }
inline void delay(uint32_t) {}
inline void delayMicroseconds(uint32_t) {}
inline long random(long n) { return n > 0 ? (long)(rand() % n) : 0; }
inline long random(long a, long b) { return b > a ? a + (long)(rand() % (b - a)) : a; }
inline void randomSeed(unsigned long s) { srand((unsigned)s); }
template <class T> inline T constrain(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
#ifndef PROGMEM
#define PROGMEM
#define pgm_read_byte(p) (*(const uint8_t*)(p))
#define pgm_read_word(p) (*(const uint16_t*)(p))
#endif
inline uint32_t esp_random() { return ((uint32_t)rand() << 16) ^ (uint32_t)rand(); }

struct HostSerial {                              // prints go to stderr, prefixed
  template <class... A> void printf(const char* fmt, A... a) { fprintf(stderr, "[serial] "); fprintf(stderr, fmt, a...); }
  void println(const char* s = "") { fprintf(stderr, "[serial] %s\n", s); }
  void print(const char* s) { fprintf(stderr, "%s", s); }
  void println(int v) { fprintf(stderr, "[serial] %d\n", v); }
  void print(int v) { fprintf(stderr, "%d", v); }
  int available() { return 0; }
  int read() { return -1; }
  void begin(unsigned long) {}
};
extern HostSerial Serial;
