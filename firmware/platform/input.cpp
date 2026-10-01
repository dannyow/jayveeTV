#include "platform/input.h"
#include <string.h>

static const int Q = 32;
static InputEvent s_q[Q];
static int s_head = 0, s_tail = 0;            // head = next read, tail = next write
static uint8_t s_down[INPUT_PLAYERS][IN_KEY_COUNT];
static int16_t s_axis[INPUT_PLAYERS][IN_KEY_COUNT];

void inputInit() { s_head = s_tail = 0; memset(s_down, 0, sizeof(s_down)); memset(s_axis, 0, sizeof(s_axis)); }

bool inputPush(const InputEvent& e) {
  int next = (s_tail + 1) % Q;
  if (next == s_head) return false;
  if (e.player < INPUT_PLAYERS && e.key < IN_KEY_COUNT) {
    s_down[e.player][e.key] = e.down;
    if (e.key == IN_KNOB) s_axis[e.player][e.key] = e.value;
  }
  s_q[s_tail] = e; s_tail = next;
  return true;
}

bool inputPoll(InputEvent* out) {
  if (s_head == s_tail) return false;
  *out = s_q[s_head]; s_head = (s_head + 1) % Q;
  return true;
}

bool    inputDown(uint8_t key, uint8_t player) { return player < INPUT_PLAYERS && key < IN_KEY_COUNT && s_down[player][key]; }
int16_t inputAxis(uint8_t key, uint8_t player) { return (player < INPUT_PLAYERS && key < IN_KEY_COUNT) ? s_axis[player][key] : 0; }
void    inputClear() { s_head = s_tail = 0; memset(s_down, 0, sizeof(s_down)); memset(s_axis, 0, sizeof(s_axis)); }
