// Input — one event queue for everything that can poke the set: the 3 physical
// keys, touch, serial (dev), and phones over WebSocket. Channels that are
// games consume it in tick(); the platform (set.cpp) produces it. Pure C, no
// Arduino, so it compiles in the host harness (tools/hostrender) too.
//
// Rules: producers call inputPush(); the ONE consumer (the current channel)
// calls inputPoll() until it returns false, every tick. inputDown()/inputAxis()
// give the held state for games that want state rather than edges.
#pragma once
#include <stdint.h>

enum InputKey : uint8_t {
  IN_NONE = 0,
  IN_LEFT, IN_RIGHT, IN_UP, IN_DOWN,     // d-pad
  IN_A, IN_B, IN_START, IN_SELECT,       // buttons
  IN_KNOB,                               // paddle knob: value = -32768..32767 (TVG-10 style)
  IN_HEX0, IN_HEX1, IN_HEX2, IN_HEX3, IN_HEX4, IN_HEX5, IN_HEX6, IN_HEX7,   // hex keypad (future channels)
  IN_HEX8, IN_HEX9, IN_HEXA, IN_HEXB, IN_HEXC, IN_HEXD, IN_HEXE, IN_HEXF,
  IN_TUNE, IN_POWER, IN_MUTE,            // the set's own controls, so a phone can be the remote
                                          // (IN_TUNE: value < 0 = next station, else = that station index)
  IN_CHAR,                               // a typed key for future emulators: value = key code
  IN_VOLUME,                             // the set's volume from a phone: value = 0..100
  IN_KEY_COUNT
};

enum InputSrc : uint8_t { SRC_KEYS, SRC_TOUCH, SRC_SERIAL, SRC_NET };

struct InputEvent {
  uint8_t  src;      // InputSrc
  uint8_t  key;      // InputKey
  uint8_t  player;   // 0 or 1 (two phones = two paddles)
  uint8_t  down;     // 1 = press / axis update, 0 = release
  int16_t  value;    // axis value for IN_KNOB, else 0
  uint32_t t;        // ms
};

static const int INPUT_PLAYERS = 2;

void    inputInit();
bool    inputPush(const InputEvent& e);           // false = queue full (event dropped)
bool    inputPoll(InputEvent* out);               // drain in tick(); false = empty
bool    inputDown(uint8_t key, uint8_t player = 0);
int16_t inputAxis(uint8_t key, uint8_t player = 0);   // last value for IN_KNOB
void    inputClear();                             // on channel change: nobody inherits held keys

// Physical-key mapping for single-player games when no phone is connected:
// IO10 = IN_A (also "rotate"/"serve"), BOOT = IN_B, PWR stays power. The
// platform decides when the keys are game keys vs the set's controls.
