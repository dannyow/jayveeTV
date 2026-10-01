// Paddle — a 1979 TV-game-console tennis (Ameprod TVG-10 / AY-3-8500 flavour):
// white blocks on black, dashed net, blocky score digits, square ball.
// Two players via IN_KNOB (or IN_UP/IN_DOWN, IN_A/IN_B) per player; the right
// paddle is a CPU whenever player 1 has been silent for 5 s. First to 11.
#pragma once
#include "platform/channel.h"

extern const Channel CH_PONG;

// Debug/harness peek at the game state (logical px, top-left of the sprites).
// Any pointer may be null.
void pongDebug(int* ballX, int* ballY, int* padLeftY, int* padRightY, int* scoreL, int* scoreR, int* cpuRight);
