#pragma once
#include "platform/channel.h"
extern const Channel CH_TESTCARD;
// Wall clock shown on the card (HH:MM). Call once a second from the platform;
// out-of-range values hide the clock. The channel never touches I2C itself.
void testcardSetTime(int h, int m);
