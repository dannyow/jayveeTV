// Paddle — TVG-10 / AY-3-8500 style tennis. See pong.h.
//
// All positions in 8.8 fixed point (1/256 px), velocities in 1/256 px per
// second; tick() integrates with the wall-clock dt so the game speed does not
// depend on the frame rate. row() only reads a handful of ints set up by tick()
// and paints white runs into a black row — no divisions, no floats.
#include "channels/pong/pong.h"
#include "platform/input.h"
#include "platform/font5x7.h"
#include "platform/sound.h"
#include <string.h>

static const uint16_t W = 0xFFFF;                 // pure white, like the original (no colour, gChroma is moot)

// ---- Court geometry (logical 240×240) ----
static const int SCORE_Y  = 6;                    // score digit band 6..35
static const int DIG_S    = 6;                    // 3×5 cells × 6 px = 18×30 digits
static const int WALL_T0  = 40, WALL_T1 = 44;     // top wall rows [40,44)
static const int WALL_B0  = 228, WALL_B1 = 232;   // bottom wall rows [228,232)
static const int COURT_Y0 = WALL_T1, COURT_Y1 = WALL_B0;   // ball/paddle space [44,228)
static const int NET_X    = 118, NET_W = 4;       // dashed centre line
static const int PW = 5, PH = 28;                 // paddle size
static const int PX_L = 14, PX_R = 221;           // paddle x (left edge)
static const int BW = 5, BH = 5;                  // ball size
static const int DIG_XL[2] = { 64, 88 };          // left score: tens, units
static const int DIG_XR[2] = { 134, 158 };        // right score: tens, units

// ---- Rules ----
static const int WIN_SCORE   = 11;
static const int BASE_VX     = 150 << 8;          // px/s ×256 at serve
static const int MAX_VX      = 520 << 8;
static const int KEY_SPEED   = 200 << 8;          // paddle speed from keys
static const int CPU_SPEED   = 130 << 8;          // the machine is beatable
static const int SERVE_MS    = 1200;
static const int WIN_MS      = 3000;
static const int HUMAN_HOLD_MS = 5000;            // player 1 silent this long -> CPU

enum State : uint8_t { ST_SERVE, ST_PLAY, ST_WIN };

static State    s_state;
static uint32_t s_stateAt, s_lastTick, s_now;
static int32_t  s_bx, s_by, s_vx, s_vy;           // ball, 8.8
static int32_t  s_py[2];                          // paddle top y, 8.8
static int      s_score[2];
static int      s_serveDir;                       // +1 = towards the right player
static int      s_cpuErr;                         // px, CPU aiming error for this ball
static uint32_t s_lastKnob[2], s_lastKey[2], s_lastAny[2];
static uint32_t s_rnd = 0x2545F491;
static int      s_winner;

// Sound: the current beep, expressed as "until" so sound() (called every
// frame) can just check the clock — see docs/CHANNELS.md "Sound".
static uint32_t s_beepUntil = 0;
static float    s_beepHz = 440;
static void beep(int kind) {                       // 1 = paddle/wall hit, 2 = point scored
  if (kind == 1) { s_beepUntil = s_now + 40;  s_beepHz = 440; }
  else            { s_beepUntil = s_now + 250; s_beepHz = 220; }
}

// Per-frame render description (plain ints for row()).
static int  r_bx, r_by, r_pl, r_pr;
static bool r_ball, r_text;
static uint8_t r_dig[4];                          // 3×5 digit rows for [Ltens, Lunits, Rtens, Runits] (0xFF = blank)
static int  r_textX;

static const uint8_t DIGIT3x5[10][5] = {
  {7,5,5,5,7}, {1,1,1,1,1}, {7,1,7,4,7}, {7,1,7,1,7}, {5,5,7,1,1},
  {7,4,7,1,7}, {7,4,7,5,7}, {7,1,1,1,1}, {7,5,7,5,7}, {7,5,7,1,7},
};

static inline uint32_t rnd() { s_rnd = s_rnd * 1103515245u + 12345u; return s_rnd >> 8; }
static inline int32_t clampPad(int32_t y) {
  const int32_t lo = COURT_Y0 << 8, hi = (COURT_Y1 - PH) << 8;
  return y < lo ? lo : (y > hi ? hi : y);
}

static void serve(uint32_t now) {
  s_state = ST_SERVE; s_stateAt = now;
  s_bx = (120 - BW / 2) << 8;
  s_by = (COURT_Y0 + 20 + (int)(rnd() % (COURT_Y1 - COURT_Y0 - 40 - BH))) << 8;
  s_vx = s_serveDir * BASE_VX;
  s_vy = (rnd() & 1) ? (BASE_VX * 5 / 12) : -(BASE_VX * 5 / 12);   // never a flat serve
  s_cpuErr = (int)(rnd() % 17) - 8;
}

static void pgStart(uint32_t now);
static void pgBegin() { pgStart(0); }     // sane even if the owner never calls start()

static void pgStart(uint32_t now) {
  s_score[0] = s_score[1] = 0;
  s_py[0] = s_py[1] = ((COURT_Y0 + COURT_Y1) / 2 - PH / 2) << 8;
  s_serveDir = (rnd() & 1) ? 1 : -1;
  s_lastTick = now; s_now = now; s_winner = -1; s_beepUntil = 0;
  for (int p = 0; p < 2; p++) { s_lastKnob[p] = s_lastKey[p] = 0; s_lastAny[p] = now - HUMAN_HOLD_MS - 1; }
  serve(now);
}

static bool cpuDrives() { return (uint32_t)(s_now - s_lastAny[1]) > (uint32_t)HUMAN_HOLD_MS; }

// Ball angle after a paddle hit: five zones like the original, by where it
// struck. The centre zone keeps the ball's current slope (it is never flat, so
// the rally always ends up at the walls and somebody eventually misses).
static int32_t zoneVy(int32_t ballCy, int32_t padCy, int32_t vxMag, int32_t curVy) {
  int off = (int)((ballCy - padCy) >> 8);                 // px, about ±(PH+BH)/2
  int zone = off * 5 / (PH + BH); if (zone > 2) zone = 2; if (zone < -2) zone = -2;
  if (zone == 0) { int32_t mag = vxMag * 5 / 24; return curVy < 0 ? -mag : mag; }
  return zone * (vxMag * 5 / 12);
}

static void movePaddles(uint32_t dt) {
  for (int p = 0; p < 2; p++) {
    if (p == 1 && cpuDrives()) {
      int32_t target;
      if (s_state == ST_PLAY && s_vx > 0) target = s_by + ((BH / 2 - PH / 2 + s_cpuErr) << 8);
      else target = ((COURT_Y0 + COURT_Y1) / 2 - PH / 2) << 8;
      int32_t d = target - s_py[1], step = (int32_t)(CPU_SPEED / 1000 * dt);
      if (d > (2 << 8)) s_py[1] += d < step ? d : step;
      else if (d < -(2 << 8)) s_py[1] -= -d < step ? -d : step;
      s_py[1] = clampPad(s_py[1]);
      continue;
    }
    bool knob = s_lastKnob[p] && (s_lastKnob[p] >= s_lastKey[p]);
    if (knob) {
      int32_t a = (int32_t)inputAxis(IN_KNOB, p) + 32768;         // 0..65535
      s_py[p] = (COURT_Y0 << 8) + (int32_t)(((int64_t)a * ((COURT_Y1 - COURT_Y0 - PH) << 8)) / 65535);
    } else {
      int dir = 0;
      if (inputDown(IN_UP, p) || inputDown(IN_A, p)) dir -= 1;
      if (inputDown(IN_DOWN, p) || inputDown(IN_B, p)) dir += 1;
      if (dir) s_py[p] = clampPad(s_py[p] + dir * (int32_t)(KEY_SPEED / 1000 * dt));
    }
  }
}

static void moveBall(uint32_t dt) {
  int32_t ox = s_bx, oy = s_by;
  int32_t nx = ox + (int32_t)((int64_t)s_vx * dt / 1000);
  int32_t ny = oy + (int32_t)((int64_t)s_vy * dt / 1000);
  // walls
  const int32_t top = COURT_Y0 << 8, bot = (COURT_Y1 - BH) << 8;
  for (int i = 0; i < 4; i++) {
    if (ny < top) { ny = 2 * top - ny; s_vy = -s_vy; beep(1); }
    else if (ny > bot) { ny = 2 * bot - ny; s_vy = -s_vy; beep(1); }
    else break;
  }
  // paddles: swept test on the paddle's inner face, so a fast ball cannot tunnel
  if (s_vx < 0) {
    const int32_t face = (PX_L + PW) << 8;
    if (ox >= face && nx < face) {
      int32_t yc = oy + (int32_t)((int64_t)(ny - oy) * (ox - face) / (ox - nx));
      if (yc + (BH << 8) > s_py[0] && yc < s_py[0] + (PH << 8)) {
        nx = 2 * face - nx;
        int32_t mag = -s_vx; mag = mag * 27 / 25; if (mag > MAX_VX) mag = MAX_VX;
        s_vx = mag; s_vy = zoneVy(yc + ((BH / 2) << 8), s_py[0] + ((PH / 2) << 8), mag, s_vy);
        beep(1);
      }
    }
  } else {
    const int32_t face = (PX_R - BW) << 8;
    if (ox <= face && nx > face) {
      int32_t yc = oy + (int32_t)((int64_t)(ny - oy) * (face - ox) / (nx - ox));
      if (yc + (BH << 8) > s_py[1] && yc < s_py[1] + (PH << 8)) {
        nx = 2 * face - nx;
        int32_t mag = s_vx; mag = mag * 27 / 25; if (mag > MAX_VX) mag = MAX_VX;
        s_vx = -mag; s_vy = zoneVy(yc + ((BH / 2) << 8), s_py[1] + ((PH / 2) << 8), mag, s_vy);
        beep(1);
      }
    }
  }
  s_bx = nx; s_by = ny;
  // out: a point
  int scorer = -1;
  if (s_bx + (BW << 8) < 0) scorer = 1;
  else if (s_bx > (LOG_W << 8)) scorer = 0;
  if (scorer >= 0) {
    s_score[scorer]++; beep(2);
    s_serveDir = scorer == 0 ? 1 : -1;               // serve towards the player who lost the point
    if (s_score[scorer] >= WIN_SCORE) { s_state = ST_WIN; s_stateAt = s_now; s_winner = scorer; }
    else serve(s_now);
  }
}

static void pgTick(uint32_t now) {
  s_now = now;
  uint32_t dt = now - s_lastTick; if (dt > 50) dt = 50; s_lastTick = now;
  InputEvent e;
  while (inputPoll(&e)) {
    if (e.player >= 2) continue;
    if (e.key == IN_KNOB) { s_lastKnob[e.player] = now ? now : 1; s_lastAny[e.player] = now; }
    else if (e.key == IN_UP || e.key == IN_DOWN || e.key == IN_A || e.key == IN_B) { s_lastKey[e.player] = now ? now : 1; s_lastAny[e.player] = now; }
    else if (e.key == IN_START && e.down) pgStart(now);
  }
  movePaddles(dt);
  switch (s_state) {
    case ST_SERVE: if (now - s_stateAt >= (uint32_t)SERVE_MS) { s_state = ST_PLAY; s_stateAt = now; } break;
    case ST_PLAY:  moveBall(dt); break;
    case ST_WIN:   if (now - s_stateAt >= (uint32_t)WIN_MS) pgStart(now); break;
  }
  // render description
  r_bx = s_bx >> 8; r_by = s_by >> 8;
  r_pl = s_py[0] >> 8; r_pr = s_py[1] >> 8;
  r_ball = s_state == ST_PLAY || (s_state == ST_SERVE && (now - s_stateAt) > (uint32_t)(SERVE_MS / 2));
  r_text = s_state == ST_WIN && ((now - s_stateAt) / 250) % 2 == 0;
  r_textX = s_winner == 0 ? 6 : 126;
  int lt = s_score[0] / 10, lu = s_score[0] % 10, rt = s_score[1] / 10, ru = s_score[1] % 10;
  r_dig[0] = lt ? lt : 0xFF; r_dig[1] = lu; r_dig[2] = rt ? rt : 0xFF; r_dig[3] = ru;
}

static inline void run(uint16_t* dst, int x0, int w) {
  if (x0 < 0) { w += x0; x0 = 0; }
  if (x0 + w > LOG_W) w = LOG_W - x0;
  for (int i = 0; i < w; i++) dst[x0 + i] = W;
}

static void pgRow(int y, uint16_t* dst) {
  memset(dst, 0, LOG_W * sizeof(uint16_t));
  if ((y >= WALL_T0 && y < WALL_T1) || (y >= WALL_B0 && y < WALL_B1)) { run(dst, 0, LOG_W); return; }
  if (y >= SCORE_Y && y < SCORE_Y + 5 * DIG_S) {
    int r = (y - SCORE_Y) / DIG_S;
    for (int i = 0; i < 4; i++) {
      if (r_dig[i] == 0xFF) continue;
      uint8_t bits = DIGIT3x5[r_dig[i]][r];
      int x0 = i < 2 ? DIG_XL[i] : DIG_XR[i - 2];
      if (bits & 4) run(dst, x0, DIG_S);
      if (bits & 2) run(dst, x0 + DIG_S, DIG_S);
      if (bits & 1) run(dst, x0 + 2 * DIG_S, DIG_S);
    }
    return;
  }
  if (y < COURT_Y0 || y >= COURT_Y1) return;
  if (((y - COURT_Y0) & 7) < 4) run(dst, NET_X, NET_W);
  if (y >= r_pl && y < r_pl + PH) run(dst, PX_L, PW);
  if (y >= r_pr && y < r_pr + PH) run(dst, PX_R, PW);
  if (r_ball && y >= r_by && y < r_by + BH) run(dst, r_bx, BW);
  if (r_text) {
    static const int TY = 100, SC = 3;               // "WINNER" in font5x7 ×3, 21 rows tall
    if (y >= TY && y < TY + 7 * SC) {
      int gy = (y - TY) / SC;
      static const char* T = "WINNER";
      int x = r_textX;
      for (int ci = 0; T[ci]; ci++, x += 6 * SC) {
        const uint8_t* g = font5x7Glyph(T[ci]);
        for (int col = 0; col < 5; col++) if ((g[col] >> gy) & 1) run(dst, x + col * SC, SC);
      }
    }
  }
}

static uint32_t pgLength() { return 0; }

// The set's own bed carries hiss/hum/whistle; a hit or a point just rides a
// short tone over it (see docs/CHANNELS.md "Sound").
static void pgSound(uint32_t now, Sound* s) {
  if (now < s_beepUntil) { s->tone = true; s->hz = s_beepHz; s->level = 0.35f; }
}

void pongDebug(int* bx, int* by, int* pl, int* pr, int* sl, int* sr, int* cpu) {
  if (bx) *bx = s_bx >> 8; if (by) *by = s_by >> 8;
  if (pl) *pl = s_py[0] >> 8; if (pr) *pr = s_py[1] >> 8;
  if (sl) *sl = s_score[0]; if (sr) *sr = s_score[1];
  if (cpu) *cpu = cpuDrives() ? 1 : 0;
}

const Channel CH_PONG = {
  { "pong", "Pong", "Your phone is the paddle.", CH_GAME | CH_ENDLESS,
    "{\"blocks\":[{\"slider\":{}},{\"buttons\":[{\"key\":\"START\",\"label\":\"start\"}]}]}" },
  pgBegin, pgStart, nullptr, pgTick, pgRow, pgSound, pgLength
};
