// The set: power, station switching, tuner/CRT composition, pacing, keys,
// orientation, knock, serial dev commands — everything main.cpp used to do
// directly. main.cpp is now just setup()/loop() calling these two.
#pragma once
void setSetup();
void setLoop();
