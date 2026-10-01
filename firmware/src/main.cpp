// JayVeeTV — main.cpp is intentionally tiny: setup()/loop() delegate to the
// platform (firmware/platform/set.cpp). See docs/ARCHITECTURE.md.
#include <Arduino.h>
#include "platform/set.h"

void setup() { setSetup(); }
void loop()  { setLoop(); }
