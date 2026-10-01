// Lifted from claude-desktop-buddy-esp32/src/hw/imu.cpp.
#include "platform/hw/imu.h"
#include "platform/hw/i2c_lock.h"
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include "platform/board.h"
#include <Arduino.h>
#include <Wire.h>
#include <SensorQMI8658.hpp>

static SensorQMI8658 s_qmi;
static bool s_ok = false;

// sda/scl default to -1 (skip): set.cpp's Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL) already
// brought the bus up. Passing the pins again makes SensorLib's begin() call
// TwoWire::setPins() on an already-initialised bus, which logs an E line every boot.
bool hwImuInit() {
  if (!s_qmi.begin(Wire, QMI8658_L_SLAVE_ADDRESS)) {
    Serial.println("imu: QMI8658 begin failed");
    return false;
  }
  Serial.printf("imu: WHO_AM_I=0x%02X chipID=0x%02X\n", s_qmi.whoAmI(), s_qmi.getChipID());
  // Reset first — without it the 2.16 returns saturated X/Y (stuck at +2g).
  s_qmi.reset();
  s_qmi.configAccelerometer(SensorQMI8658::ACC_RANGE_4G, SensorQMI8658::ACC_ODR_500Hz, SensorQMI8658::LPF_OFF);   // no LPF: a 10 ms knock must reach the samples (the default 2.66 % LPF smeared it to 0.08 g)   // 500 Hz: the tap engine's windows below are counted in samples at this rate
  s_qmi.enableAccelerometer();
  s_ok = true;
  return true;
}

bool hwImuAccelRaw(float* ax, float* ay, float* az) {
  if (!s_ok) return false;
  IMUdata d;
  if (!s_qmi.getAccelerometer(d.x, d.y, d.z)) return false;
  *ax = d.x; *ay = d.y; *az = -d.z;
  return true;
}

// Tap = a knock on the enclosure. QMI8658 tap engine, windows in samples @500 Hz ODR:
// peak within 20 samples (40 ms), quiet 50 samples before a 2nd tap, double-tap window
// 250 samples (0.5 s). Thresholds in g: peak 0.8, quiet below 0.4. Tune on the bench.
static bool s_tap = false;
bool hwImuTapInit() {
  if (!s_qmi.configTap(0, 20, 50, 250, 0.0625f, 0.25f, 0.5f, 0.3f)) { Serial.println("imu: configTap failed"); return false; }
  s_qmi.enableAccelerometer();                      // configTap switches the accel off while it writes the engine config
  if (!s_qmi.enableTap(SensorQMI8658::INTERRUPT_PIN_1)) { Serial.println("imu: enableTap failed"); return false; }
  s_tap = true; Serial.println("imu: tap detection on");
  return true;
}
int hwImuTapPoll() {
  if (!s_tap) return 0;
  i2cLock(); int st = s_qmi.getStatusRegister(); i2cUnlock();
  if (st < 0 || !(st & 0x02)) return 0;              // raw STATUS1: bit 1 = tap (SensorLib's 0x0400 is its composed status word, not this register)
  i2cLock(); auto ev = s_qmi.getTapStatus(); i2cUnlock();
  return ev == SensorQMI8658::DOUBLE_TAP ? 2 : 1;
}

// ---- shared I2C lock ----
static SemaphoreHandle_t s_i2c = nullptr;
static void i2cLockInit() { if (!s_i2c) s_i2c = xSemaphoreCreateRecursiveMutex(); }
void i2cLock()   { i2cLockInit(); xSemaphoreTakeRecursive(s_i2c, portMAX_DELAY); }
void i2cUnlock() { xSemaphoreGiveRecursive(s_i2c); }

// ---- software knock detector: a 250 Hz sampling task ----
// A knock on the enclosure is a sharp change of |a| between two samples 4 ms apart.
// The main loop only runs at 12-25 Hz and cannot see a 10 ms impulse; this task can.
static volatile float s_ax = 0, s_ay = 0, s_az = 1;      // LOW-PASSED sample for the orientation code (a knock must not look like a flip)
static volatile float s_peak = 0;                         // max jerk since the last hwImuJerkPeak()
static volatile int   s_swTaps = 0;
static volatile float s_lastTapG = 0;                     // jerk of the last software knock (g)
static float s_tapThr = 0.3f;                             // g, per 4 ms sample; serial 'J <x100>' tunes it
static void imuTask(void*) {
  float prev = 0; uint32_t lastTap = 0;
  TickType_t t = xTaskGetTickCount();
  const uint32_t settleUntil = millis() + 500;   // the first samples after power-up are junk: no knocks from them
  for (;;) {
    vTaskDelayUntil(&t, pdMS_TO_TICKS(4));
    IMUdata d;
    i2cLock(); bool ok = s_qmi.getAccelerometer(d.x, d.y, d.z); i2cUnlock();
    if (!ok) continue;
    // Orientation: exponential average, tau ~0.3 s at 250 Hz. Sign flip: az > 0 = face up.
    s_ax += 0.015f * (d.x - s_ax); s_ay += 0.015f * (d.y - s_ay); s_az += 0.015f * (-d.z - s_az);
    float mag = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    if ((int32_t)(millis() - settleUntil) < 0) { prev = mag; continue; }
    float jerk = fabsf(mag - prev); prev = mag;
    if (jerk > s_peak) s_peak = jerk;
    uint32_t now = millis();
    if (jerk > s_tapThr && now - lastTap > 300) { lastTap = now; s_swTaps++; s_lastTapG = jerk; }
    else if (jerk > s_lastTapG && now - lastTap < 60) s_lastTapG = jerk;   // the ring-down right after may carry the true peak
  }
}
void hwImuTaskStart() { xTaskCreate(imuTask, "imu", 3072, nullptr, 2, nullptr); }
float hwImuJerkPeak() { float p = s_peak; s_peak = 0; return p; }
void  hwImuSetTapThreshold(float g) { s_tapThr = g; }
int   hwImuSoftTaps() { int n = s_swTaps; s_swTaps = 0; return n; }
bool hwImuAccel(float* ax, float* ay, float* az) { *ax = s_ax; *ay = s_ay; *az = s_az; return true; }
float hwImuLastTapG() { return s_lastTapG; }
