// QMI8658 6-axis IMU via SensorLib. Accelerometer only for now (orientation).
#pragma once
bool hwImuInit();
// Gravity in g. az > 0 when the screen faces UP (sign flipped vs the raw chip
// axis: raw Z is ≈ -0.94 screen-up on this board, as measured in the claude-desktop-buddy-esp32 port).
bool hwImuAccel(float* ax, float* ay, float* az);
// Hardware tap detection (QMI8658 motion engine). Poll once per loop: 0 none, 1 single tap, 2 double tap.
bool hwImuTapInit();
int  hwImuTapPoll();
// Software knock detector (250 Hz task). Start it after hwImuInit(); orientation then reads the task's sample.
void  hwImuTaskStart();
int   hwImuSoftTaps();            // knocks seen since the last call (cleared)
float hwImuJerkPeak();            // max |d|a|| between consecutive samples since the last call, in g (diagnostics)
void  hwImuSetTapThreshold(float g);
float hwImuLastTapG();            // strength (g of jerk) of the most recent software knock
