// PCF85063 RTC via SensorLib (I2C 0x51, shared bus). Local time is stored in
// the chip; the platform pushes it to the test card once a second. Fails
// soft: every call returns false until init succeeded and the oscillator has
// run since the last set (OS flag clear).
#pragma once
#include <stdint.h>
bool hwRtcInit();
bool hwRtcGet(int* h, int* m, int* s, int* wday);          // wday 0 = Sunday
bool hwRtcSet(uint32_t epochUtc, int tzOffsetMin);          // tzOffsetMin e.g. 120 for Poland in September (CEST)

// Timezone offset the phone last supplied (docs/NETWORK.md "Time"), persisted in NVS so
// NTP can set the RTC without ever guessing a timezone. hwRtcSavedTz() returns false (and
// leaves *offsetMin untouched) until a phone has visited at least once.
bool hwRtcSavedTz(int* offsetMin);
void hwRtcSaveTz(int offsetMin);
