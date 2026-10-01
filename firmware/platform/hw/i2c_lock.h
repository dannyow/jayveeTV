// One recursive mutex around the shared I2C bus (AXP2101, ES8311, ES7210, CST9217,
// QMI8658, PCF85063). The IMU task samples at 250 Hz while the main loop reads the
// RTC once a second; Wire is not thread-safe, so every I2C user takes this lock.
#pragma once
void i2cLock();
void i2cUnlock();
