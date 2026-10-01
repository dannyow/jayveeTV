#include "platform/hw/rtc.h"
#include "platform/hw/i2c_lock.h"
struct I2cGuard { I2cGuard() { i2cLock(); } ~I2cGuard() { i2cUnlock(); } };
#include "platform/board.h"
#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <time.h>
#include <time/pcf85063/SensorPCF85063.hpp>

static SensorPCF85063 s_rtc;
static bool s_ok = false;
static bool s_warned = false;

static const char* TZ_PREF_NS = "jvtv-rtc";

static bool notReady(const char* what) {
  if (!s_warned) { Serial.printf("rtc: %s (no clock on the card)\n", what); s_warned = true; }
  return false;
}

// set.cpp's Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL) is the one place the bus is actually
// brought up; SensorLib's begin() defaults sda/scl to -1 here so it skips its own
// TwoWire::setPins() (which logs an E line if the bus is already initialised — it always
// is, by the time this runs) and just re-touches an already-open bus.
bool hwRtcInit() { I2cGuard g_;
  if (!s_rtc.begin(Wire)) return notReady("PCF85063 begin failed");
  s_ok = true;
  if (!s_rtc.isClockIntegrityGuaranteed()) Serial.println("rtc: oscillator stopped since last set — time invalid until `T <epoch>`");
  else { int h, m, s, w; if (hwRtcGet(&h, &m, &s, &w)) Serial.printf("rtc: %02d:%02d:%02d wday=%d\n", h, m, s, w); }
  return true;
}

bool hwRtcGet(int* h, int* m, int* s, int* wday) { I2cGuard g_;
  if (!s_ok) return notReady("not initialised");
  if (!s_rtc.isClockIntegrityGuaranteed()) return notReady("oscillator stop flag set");
  RTC_DateTime t = s_rtc.getDateTime();
  if (t.getHour() > 23 || t.getMinute() > 59 || t.getSecond() > 59) return false;
  *h = t.getHour(); *m = t.getMinute(); *s = t.getSecond(); *wday = t.getWeek();
  return true;
}

bool hwRtcSet(uint32_t epochUtc, int tzOffsetMin) { I2cGuard g_;
  if (!s_ok) return notReady("not initialised");
  time_t local = (time_t)epochUtc + (time_t)tzOffsetMin * 60;   // the chip keeps LOCAL time
  struct tm tm; gmtime_r(&local, &tm);
  s_rtc.setDateTime(RTC_DateTime(tm));                            // writes seconds with OS bit cleared
  s_rtc.start();
  s_warned = false;
  Serial.printf("rtc: set %04d-%02d-%02d %02d:%02d:%02d (UTC%+d min)\n",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, tzOffsetMin);
  return true;
}

// Read from NVS once and kept here: NTP asks every loop until a phone has visited.
// Opened read-write so a namespace that does not exist yet is created, not logged as an error.
static int s_tzState = -1;                   // -1 not read yet, 0 none stored, 1 s_tz valid
static int s_tz = 0;
bool hwRtcSavedTz(int* offsetMin) {
  if (s_tzState < 0) {
    Preferences p; p.begin(TZ_PREF_NS, false);
    s_tzState = p.isKey("tz") ? 1 : 0;
    if (s_tzState) s_tz = p.getInt("tz", 0);
    p.end();
  }
  if (s_tzState == 1 && offsetMin) *offsetMin = s_tz;
  return s_tzState == 1;
}

void hwRtcSaveTz(int offsetMin) {
  Preferences p; p.begin(TZ_PREF_NS, false);
  p.putInt("tz", offsetMin);
  p.end();
  s_tz = offsetMin; s_tzState = 1;
}
