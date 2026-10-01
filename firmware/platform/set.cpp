// The set — power, station switching, tuner/CRT composition, keys,
// orientation, knock, the phone controller, serial dev commands. Everything
// a channel never sees. Pipeline: channel (clean picture) -> glitch layer
// (analog degradation, at blit time) -> panel. See docs/ARCHITECTURE.md.
#include "platform/set.h"
#include <Arduino.h>
#include <math.h>
#include <time.h>
#include <Wire.h>
#include <Preferences.h>
#include "platform/board.h"
#include "platform/hw/power.h"
#include "platform/hw/display.h"
#include "platform/hw/audio.h"
#include "platform/hw/imu.h"
#include "platform/hw/net.h"
#include "platform/hw/webctl.h"
#include "platform/hw/rtc.h"
#include "platform/remotecard.h"
#include "platform/input.h"
#include "platform/glitch.h"
#include "platform/tuner.h"
#include "platform/crt.h"
#include "platform/sound.h"
#include "channels/channels.h"
#include "channels/testcard/testcard.h"   // testcardSetTime(): the platform owns the wall clock, not I2C-from-a-channel

// Station numbering is CHANNELS[] (channels/channels.cpp); index 0 is the
// boot station. Tune-knob order: empty = every station in list order; serial
// 'O' can set a subset/order at runtime.
static const int N_STATIONS = CHANNEL_COUNT;
static int s_order[16] = {};
static int s_orderN = 0;
static int s_station = 0;
static bool s_remoteCard = false;         // showing the REMOTE card (not a numbered station)
static TunerOut s_tune;
static CrtOut   s_crt;
static Glitch   s_g;                 // composed: tuner signal + tube geometry
static bool     s_tunerOn = true;
static bool     s_show = false;      // auto show: power cycles around the tuner cycle (serial 'a')
// 1 = boot muted (handy on a bench). BOOT key / serial `u` toggles; `v` sets the
// level used when unmuted.
#define TV_DEV_MUTE 0
static bool     s_muted = TV_DEV_MUTE;
static int      s_volume = 20;   // bench default; 85 + doubled gains browned the board out at loud moments
static bool     s_imuOk = false;
static bool     s_faceDown = false;
static uint32_t s_faceSince = 0;
static uint8_t  s_brightMax = 255;
static int      s_locksSincePowerOn = 0;
static uint32_t s_lockedAt = 0, s_offAt = 0;
// Misbehave (docs/CHANNELS.md CH_MISBEHAVE): the flagged station wakes up in
// SNOW; a KNOCK (IMU tap) tunes it in at once, otherwise it finds the picture
// by itself after 2 s. A hard knock on the picture knocks it back into snow,
// which then waits for the next knock. Serial 'M' toggles, 'n' fakes a knock.
static bool     s_misbehave = true;
static const uint32_t AUTO_TUNE_MS = 2000;
static uint32_t s_autoTuneAt = 0;               // power-on snow finds the picture by itself at this time (0 = not armed)
static bool misbehaveHere() { return s_misbehave && !s_remoteCard && (CHANNELS[s_station]->info.flags & CH_MISBEHAVE) != 0; }
static bool stationIsGame() { return !s_remoteCard && (CHANNELS[s_station]->info.flags & CH_GAME) != 0; }

// RULE: everything is shown rotated 90° CCW from the panel's vendor default
// (MADCTL 0x30) because the module is mounted buttons-sideways. 0x90 = that.
#define TV_MADCTL 0x90

static void die(const char* what) {
  Serial.printf("FAIL: %s\n", what);
  while (true) { delay(1000); Serial.printf("FAIL: %s\n", what); }
}

// pressed() = press edge (debounced). held(ms) = still down for at least ms since
// the press, fires once per press. On game stations a short IO10 press is the game's
// "A" and a long press (800 ms) is the tune knob, so a game never traps the set.
struct Key { int pin; bool activeHigh; bool was = false; uint32_t t = 0; bool longFired = false;
  bool isDown() const { return (digitalRead(pin) == HIGH) == activeHigh; }
  bool pressed() {
    bool now = isDown();
    bool edge = now && !was && (millis() - t) > 40;
    if (edge) { t = millis(); longFired = false; }
    was = now; return edge; }
  bool held(uint32_t ms) {
    bool now = isDown();
    if (now && !longFired && millis() - t >= ms) { longFired = true; return true; }
    return false; }
  bool released() { bool now = isDown(); return !now && was; }
  // Short press = release before the long-press threshold fired; call every loop after pressed().
  bool releasedShort() { bool now = isDown(); bool r = !now && wasHeld && !longFired; wasHeld = now; return r; }
  bool wasHeld = false; };
static Key kIo10 { PIN_KEY_IO10, false }, kBoot { PIN_KEY_BOOT, false }, kPwr { PIN_KEY_PWR, true };

static void powerToggle(uint32_t now, const char* why);
static float s_knockK = 0; static uint32_t s_knockAt = 0;   // last knock strength (0..1) and time, for the sound
static void knock(uint32_t now, const char* how, float g = 1.5f) {
  static uint32_t lastKnock = 0;
  if (now - lastKnock < 1500) return;                 // one knock per 1.5 s: hw tap + sw jerk + double tap must count once
  lastKnock = now;
  if (!crtIsOn()) { Serial.printf("knock (%s) ignored: set is off\n", how); return; }
  float k = (g - 0.4f) / 2.0f; if (k < 0.15f) k = 0.15f; if (k > 1) k = 1;      // 0.5 g -> light, 2.4 g+ -> full
  s_knockK = k; s_knockAt = now;
  tunerJolt(now, k);
  Serial.printf("knock (%s) %.2f g -> k %.2f, state %s\n", how, g, k, s_tune.state == TUNER_LOCKED ? "locked" : "not locked");
  s_autoTuneAt = 0;
  if (s_tune.state == TUNER_LOCKED) { if (k >= 0.6f) tunerDrop(now); return; }   // a HARD knock on a good picture knocks it off; a light one only shakes it
  tunerKick(now);                                                                 // hunt back in
}
static void pushInput(uint8_t src, uint8_t key, bool down, uint8_t player = 0, int16_t value = 0) {
  InputEvent e = { src, key, player, (uint8_t)down, value, millis() };
  if (!inputPush(e)) Serial.println("input: queue full");
}
// Controls: PWR = power on/off · IO10 = tune (or the game's A) · BOOT = mute
// (or the game's B; long BOOT = REMOTE) · face down = off, face up = on.
// Volume and mute are remembered (NVS "jvtv-audio"), written 2 s after the last change so
// dragging the phone's slider doesn't wear the flash, and shown on every phone.
static uint32_t s_audioSaveAt = 0;
static void audioChanged() { s_audioSaveAt = millis() + 2000; if (!s_audioSaveAt) s_audioSaveAt = 1; webctlSetAudio(s_volume, s_muted); }
static void audioSaveIfDue(uint32_t now) {
  if (!s_audioSaveAt || (int32_t)(now - s_audioSaveAt) < 0) return;
  s_audioSaveAt = 0;
  Preferences p; p.begin("jvtv-audio", false); p.putInt("vol", s_volume); p.putBool("mute", s_muted); p.end();
}
static void setMute(bool m) { s_muted = m; audioVolume(m ? 0 : s_volume); audioChanged(); Serial.printf("mute -> %s\n", m ? "on" : "off"); }

// Tune to a station index: the outgoing channel lets go (arena, sockets), the
// tuner hunts, the channel's start() fires when it locks (in loop()).
static bool tuneTo(int idx, uint32_t now) {
  if (idx < 0 || idx >= N_STATIONS) return false;
  if (idx != s_station && CHANNELS[s_station]->stop) CHANNELS[s_station]->stop();
  s_station = idx;
  inputClear();
  tunerKick(now);
  webctlSetStation(s_station);
  return true;
}
// Tune knob: hunt to the next station in the configured order (default: list order).
static void tuneKnob(uint32_t now) {
  if (!crtIsOn()) return;
  int pick;
  if (s_orderN > 0) {
    int at = -1; for (int i = 0; i < s_orderN; i++) if (s_order[i] == s_station) { at = i; break; }
    pick = s_order[(at + 1) % s_orderN];
  } else pick = (s_station + 1) % N_STATIONS;
  Serial.printf("tune knob -> station %d (%s)\n", pick, CHANNELS[pick]->info.title);
  tuneTo(pick, now);
}
// REMOTE card: platform-level, not a numbered station (docs/REMOTE.md).
static bool s_remoteSwallow = false;
static uint32_t s_remoteShownAt = 0, s_remoteDismissAt = 0;
static int s_remoteClients = 0;           // phones connected when the card went up
static bool s_remoteFromSnow = false;     // the card opened over snow: closing it goes back to snow, not a picture      // the BOOT press that opened the REMOTE card is still down: ignore it until released
static void showRemoteCard(uint32_t now) {
  if (s_remoteCard) return;
  s_remoteSwallow = kBoot.isDown();
  s_remoteShownAt = now; s_remoteDismissAt = 0; s_remoteClients = webctlClients();
  s_remoteFromSnow = s_tune.state == TUNER_LOST || s_tune.state == TUNER_LOSING;
  if (CHANNELS[s_station]->stop) CHANNELS[s_station]->stop();
  s_remoteCard = true;
  inputClear();
  tunerKick(now);
}
static void hideRemoteCard(uint32_t now) {
  if (!s_remoteCard) return;
  s_remoteCard = false;
  if (!s_remoteFromSnow) { tuneTo(s_station, now); return; }   // re-lock into the same station we left
  tunerStart(now, TUNER_LOST);                                 // it was snow: snow again, the channel starts on the next lock
  if (s_autoTuneAt) s_autoTuneAt = now + AUTO_TUNE_MS;         // power-on snow still finds the picture by itself
}

// Physical Wi-Fi forget (docs/NETWORK.md): a second BOOT hold, on top of the
// REMOTE card, arms a confirmation that a phone alone cannot complete.
enum ForgetState { FORGET_IDLE, FORGET_ARMED };
static ForgetState s_forgetState = FORGET_IDLE;
static uint32_t    s_forgetArmedAt = 0;
static void forgetArm(uint32_t now, const char* why) {
  if (!s_remoteCard) showRemoteCard(now);
  s_forgetState = FORGET_ARMED; s_forgetArmedAt = now;
  remoteCardSetForgetArmed(true);
  Serial.printf("forget wifi: ARMED (%s), press BOOT within 5 s to confirm\n", why);
}
// BOOT on the REMOTE card: one physical button, two meanings, told apart by
// duration and by state rather than by two independent timers on one press
// (see set.cpp's key-reading comment in setLoop() for why held(ms) alone
// can't do this). released before 5 s -> back to the previous station,
// mirroring the 800 ms long-press that got us here but firing on release so
// a hold that goes on to arm forget never also fires it; held 5 s -> arm;
// once armed, a fresh BOOT press within 5 s confirms (erase + restart into
// AP), the window elapsing cancels back to the plain REMOTE card.
static void bootRemoteAndForget(uint32_t now) {
  static bool wasDown = false;
  bool down = kBoot.isDown();
  bool edge = down && !wasDown;
  bool releaseEdge = !down && wasDown;
  wasDown = down;
  // The long press that brought us here is not a press ON the card: its release must not
  // exit, and its duration must not count toward the 5 s forget hold. Only fresh presses do.
  if (s_remoteSwallow) { if (!down) s_remoteSwallow = false; return; }

  if (s_forgetState == FORGET_ARMED) {
    if (edge) {
      Serial.println("forget wifi: CONFIRMED, erasing saved credentials");
      hwNetForget();
      remoteCardSetForgetArmed(false);
      ESP.restart();
    } else if ((int32_t)(now - s_forgetArmedAt) > 5000) {
      s_forgetState = FORGET_IDLE;
      remoteCardSetForgetArmed(false);
      Serial.println("forget wifi: timed out, cancelled");
    }
    return;
  }
  if (down && (int32_t)(now - kBoot.t) >= 5000) { forgetArm(now, "BOOT held 5 s"); return; }
  if (releaseEdge) { Serial.println("BOOT held -> back on air"); hideRemoteCard(now); }
}

// Pushes the platform's Wi-Fi state into the REMOTE card whenever it changes:
// AP mode -> the Wi-Fi join QR + SSID/password; home mode -> the page's URL
// + jayveetv.local. Cheap (string compares), called every loop.
static void updateRemoteNet() {
  static RemoteNet lastMode = (RemoteNet)-1;
  static char lastA[40] = "\x01", lastB[24] = "\x01";
  if (!hwNetUp()) {
    if (lastMode != REMOTE_NET_NONE) { remoteCardSetNet(REMOTE_NET_NONE, "", ""); lastMode = REMOTE_NET_NONE; lastA[0] = 0; lastB[0] = 0; }
    return;
  }
  char a[40], b[24];
  RemoteNet mode;
  if (hwNetMode() == NET_AP) {
    mode = REMOTE_NET_AP;
    strncpy(a, hwNetApSsid(), sizeof a - 1); a[sizeof a - 1] = 0;
    strncpy(b, hwNetApPass(), sizeof b - 1); b[sizeof b - 1] = 0;
  } else {
    mode = REMOTE_NET_HOME;
    snprintf(a, sizeof a, "http://%s/", hwNetAddress());
    strncpy(b, "jayveetv.local", sizeof b - 1); b[sizeof b - 1] = 0;
  }
  if (mode != lastMode || strcmp(a, lastA) != 0 || strcmp(b, lastB) != 0) {
    remoteCardSetNet(mode, a, b);
    lastMode = mode;
    strncpy(lastA, a, sizeof lastA - 1); lastA[sizeof lastA - 1] = 0;
    strncpy(lastB, b, sizeof lastB - 1); lastB[sizeof lastB - 1] = 0;
  }
}

// NTP (home network only): syncs the instant (UTC) via SNTP, non-blocking — configTime()
// starts esp-idf's SNTP client in the background, we just poll time(nullptr) each loop.
// The phone (webctl "tm", docs/NETWORK.md "Time") is the only source of the timezone
// offset: no offset ever stored yet -> leave the RTC alone rather than guess one. Applies
// once as soon as a plausible time and a stored offset are both in hand, then again every
// few hours in case the RTC's oscillator has drifted.
static bool     s_ntpStarted = false;
static uint32_t s_ntpAppliedAt = 0;           // millis() of the last RTC set from NTP (0 = never)
static const uint32_t NTP_RESYNC_MS = 6UL * 3600UL * 1000UL;
static void pollNtp(uint32_t now) {
  if (!hwNetUp() || hwNetMode() != NET_HOME) { s_ntpStarted = false; return; }
  if (!s_ntpStarted) {
    configTime(0, 0, "pool.ntp.org");
    s_ntpStarted = true;
    Serial.println("ntp: syncing (home network)");
  }
  if (s_ntpAppliedAt && (int32_t)(now - s_ntpAppliedAt) < (int32_t)NTP_RESYNC_MS) return;
  time_t t = time(nullptr);
  if (t < 1600000000) return;                 // SNTP hasn't landed a reply yet
  int offsetMin;
  if (!hwRtcSavedTz(&offsetMin)) {
    static bool warned = false;
    if (!warned) { Serial.println("ntp: have the time but no timezone yet - a phone visit will supply one"); warned = true; }
    return;
  }
  bool ok = hwRtcSet((uint32_t)t, offsetMin);
  s_ntpAppliedAt = now;
  Serial.printf("ntp: synced epoch %u, stored offset %+d min -> rtc %s\n", (unsigned)t, offsetMin, ok ? "ok" : "FAILED");
}

// Face-down detector. Angle is measured from the FACE-DOWN position (0° = lying
// on its face, 180° = screen up). Off when within 30° of face down, on once
// lifted past 40° (hysteresis), 100 ms dwell to ignore bumps.
static const float OFF_DEG = 30.0f, ON_DEG = 40.0f;
static void pollOrientation(uint32_t now) {
  static uint32_t last = 0; if (now - last < 50) return; last = now;
  float ax, ay, az; if (!hwImuAccel(&ax, &ay, &az)) return;
  float g = sqrtf(ax * ax + ay * ay + az * az); if (g < 0.5f) return;
  float c = -az / g; if (c > 1) c = 1; if (c < -1) c = -1;
  float deg = acosf(c) * 57.2958f;                              // 0 = face down
  bool down = s_faceDown ? (deg < ON_DEG) : (deg < OFF_DEG);
  if (down != s_faceDown) { if (!s_faceSince) s_faceSince = now; if (now - s_faceSince < 400) return; }   // 400 ms dwell: a knock's ring-down must not flip the set
  else { s_faceSince = 0; return; }
  s_faceDown = down; s_faceSince = 0;
  Serial.printf("orientation: face %s (%.0f deg from face-down)\n", down ? "DOWN" : "UP", deg);
  if (down && crtIsOn()) {
    s_show = false; powerToggle(now, "face down");
    // Face down = the set goes back on the shelf: reset to station 0, in
    // snow, waiting for a knock, no matter what was playing or the REMOTE card.
    if (s_remoteCard || s_station != 0) {
      if (!s_remoteCard && CHANNELS[s_station]->stop) CHANNELS[s_station]->stop();
      s_remoteCard = false; s_station = 0; inputClear();
    }
    s_autoTuneAt = 0;
    Serial.println("face down -> reset to station 0");
  }
  if (!down && !crtIsOn()) { s_show = false; powerToggle(now, "face up"); }
}

static void renderStripe(void*, uint16_t* dst, int y0, int rows) {
  bool wide = !s_remoteCard && (CHANNELS[s_station]->info.flags & CH_WIDE) != 0;
  RowFn row = s_remoteCard ? remoteCardRow : CHANNELS[s_station]->row;
  glitchRender(dst, y0, rows, row, wide, s_g);
}

static void powerToggle(uint32_t now, const char* why) {
  if (crtIsOn()) { crtPowerOff(now); audioEvent(AUDIO_EV_POWER_OFF); Serial.printf("power off (%s)\n", why); }
  else { crtPowerOn(now); tunerStart(now, misbehaveHere() ? TUNER_LOST : TUNER_TUNING); s_locksSincePowerOn = 0; s_autoTuneAt = misbehaveHere() ? now + AUTO_TUNE_MS : 0; audioEvent(AUDIO_EV_POWER_ON); Serial.printf("power on (%s)%s\n", why, misbehaveHere() ? " - snow, knock or wait 2 s" : ""); }
}

// Soundtrack follows the picture: the platform's own bed (hiss to a lost
// signal, hum to a struggling set, the whistle to any set that's on, the
// knock thud) plus whatever the current channel asked for via sound().
static void mixSound(uint32_t now) {
  AudioMix m = { 0, 1000, 0, 0, 0, 0 };
  float d = s_tune.detune;
  bool on = s_crt.state == CRT_ON || s_crt.state == CRT_WARMUP;
  if (on) {
    float warm = s_crt.state == CRT_WARMUP ? s_crt.bright : 1.0f;      // sound comes up with the tube
    Sound snd = {}; snd.detune = d;
    if (!s_remoteCard && CHANNELS[s_station]->sound) CHANNELS[s_station]->sound(now, &snd);
    m.tone   = snd.tone ? snd.level * warm : 0.0f;
    m.toneHz = snd.tone ? snd.hz : 1000.0f;
    m.hiss   = 0.7f * powf(d, 1.3f) * warm;
    m.hum    = (0.05f + 0.30f * d) * warm;
    if (now - s_knockAt < 180) { float e = (1.0f - (now - s_knockAt) / 180.0f) * s_knockK; m.hum += 0.9f * e; m.hiss += 0.5f * e; }   // the cabinet thud
    m.whine  = 0.02f * warm;                                           // faint: "the set is on"
  }
  audioSetMix(m);
}

static const char* stateName(TunerState s) {
  static const char* N[4] = { "locked", "losing", "lost", "tuning" }; return N[s];
}

// Serial dev commands (one per line): "b <0-255>" max brightness, "m <hex>"
// MADCTL, "t" toggle tuner (off = clean locked picture), "p" power toggle
// (stops the auto show), "a" auto show back on, "?" status.
static void pollSerial() {
  static char buf[32]; static int n = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      buf[n] = 0; n = 0;
      // Strict shape: one command letter, then end or a space. Anything else is
      // ignored — the host tty can echo our own output back at us on open, and
      // a loose parser turned "fps 13.0 ..." into an `f` command.
      if (buf[0] == 0 || !(buf[1] == 0 || buf[1] == ' ')) continue;
      if (buf[0] == 'b') { s_brightMax = atoi(buf + 1); Serial.printf("brightness max -> %d\n", s_brightMax); }
      else if (buf[0] == 'm') { int v = strtol(buf + 1, nullptr, 16); hwDisplayMadctl(v); Serial.printf("MADCTL -> 0x%02X\n", v); }
      else if (buf[0] == 't') { s_tunerOn = !s_tunerOn; Serial.printf("tuner -> %s\n", s_tunerOn ? "on" : "off"); }
      else if (buf[0] == 'p') { s_show = false; powerToggle(millis(), "serial"); }
      else if (buf[0] == 'a') { s_show = !s_show; tunerSetAuto(s_show); Serial.printf("auto show -> %s\n", s_show ? "on" : "off"); }
      else if (buf[0] == 'v') { s_volume = atoi(buf + 1); if (!s_muted) audioVolume(s_volume); audioChanged(); Serial.printf("volume -> %d\n", s_volume); }
      else if (buf[0] == 'k') tuneKnob(millis());
      else if (buf[0] == 'c') { int i = atoi(buf + 1); if (i >= 0 && i < N_STATIONS) tuneTo(i, millis()); }
      else if (buf[0] == 'u') setMute(!s_muted);
      else if (buf[0] == 'O') {                        // O 0,1 = tune order; O = every station
        s_orderN = 0; const char* q = buf + 1;
        while (*q && s_orderN < 16) { while (*q == ' ' || *q == ',') q++; if (!*q) break; int v = atoi(q); if (v >= 0 && v < N_STATIONS) s_order[s_orderN++] = v; while (*q && *q != ',' && *q != ' ') q++; }
        Serial.printf("tune order ->"); if (!s_orderN) Serial.printf(" all"); for (int i = 0; i < s_orderN; i++) Serial.printf(" %d", s_order[i]); Serial.println();
      }
      else if (buf[0] == 'q') { uint32_t nw = millis(); if (s_remoteCard) hideRemoteCard(nw); else showRemoteCard(nw); Serial.printf("(q) remote -> %s\n", s_remoteCard ? "on" : "off"); }
      else if (buf[0] == 'n') { float g = atof(buf + 1); knock(millis(), "serial", g > 0 ? g : 1.5f); }
      else if (buf[0] == 'J') { float g = atoi(buf + 1) / 100.0f; hwImuSetTapThreshold(g); Serial.printf("knock threshold -> %.2f g\n", g); }
      else if (buf[0] == 'I') Serial.printf("imu tap poll -> %d\n", hwImuTapPoll());
      else if (buf[0] == 'M') { s_misbehave = !s_misbehave; Serial.printf("misbehave -> %s\n", s_misbehave ? "on" : "off"); }
      else if (buf[0] == 'T') {                        // T <epochUtc> [offsetMin]: dev clock set (host: date +%s)
        char* end = nullptr;
        uint32_t e = (uint32_t)strtoul(buf + 1, &end, 10);
        if (e > 1600000000u) {
          int off; bool haveOff = false;
          if (end) { while (*end == ' ') end++; if (*end) { off = (int)strtol(end, nullptr, 10); haveOff = true; } }
          if (!haveOff && !hwRtcSavedTz(&off)) off = 120;   // no explicit arg, nothing stored yet: the old CEST default
          bool ok = hwRtcSet(e, off);
          Serial.printf("rtc set: epoch %u offset %+d min -> %s\n", (unsigned)e, off, ok ? "ok" : "FAILED");
        } else Serial.println("T <epochUtc> [offsetMin]   (host: date +%s)");
      }
      else if (buf[0] == 'x') {                        // x <key> <down> [player] [value]: inject an input event (dev / harness parity)
        int key = 0, down = 1, player = 0, value = 0;
        sscanf(buf + 1, "%d %d %d %d", &key, &down, &player, &value);
        if (key > 0 && key < IN_KEY_COUNT) { pushInput(SRC_SERIAL, (uint8_t)key, down != 0, (uint8_t)player, (int16_t)value); Serial.printf("input <- key %d down %d p%d v%d\n", key, down, player, value); }
      }
      else if (buf[0] == 'i') { float ax, ay, az; if (hwImuAccel(&ax, &ay, &az)) Serial.printf("accel x=%.2f y=%.2f z=%.2f (z>0 = face up)\n", ax, ay, az); else Serial.println("imu: no data"); }
      else if (buf[0] == '?') { int hh, mm, ss, wd; bool have = hwRtcGet(&hh, &mm, &ss, &wd);
        char rtcs[12]; if (have) snprintf(rtcs, sizeof rtcs, "%02d:%02d:%02d", hh, mm, ss); else strcpy(rtcs, "none");
        Serial.printf("jvc-tv station=%d/%s remote=%d crt=%d state=%s detune=%.2f show=%d mute=%d face=%s heap=%u rtc=%s\n",
          s_station, CHANNELS[s_station]->info.id, (int)s_remoteCard, (int)s_crt.state, stateName(s_tune.state), s_tune.detune,
          (int)s_show, (int)s_muted, s_faceDown ? "down" : "up", (unsigned)ESP.getFreeHeap(), rtcs); }
    } else if (n < (int)sizeof(buf) - 1) buf[n++] = c;
  }
}

void setSetup() {
  Serial.begin(115200);
  delay(800);
  Serial.println("\n=== jayveetv boot ===");
  Serial.printf("free heap: %u\n", (unsigned)ESP.getFreeHeap());

  pinMode(PIN_KEY_IO10, INPUT_PULLUP);
  pinMode(PIN_KEY_BOOT, INPUT_PULLUP);
  pinMode(PIN_KEY_PWR,  INPUT_PULLUP);   // BSS138 inverter output needs the pull-up (the claude-desktop-buddy-esp32 port does the same)
  pinMode(PIN_TP_RESET, OUTPUT);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(400000);
  if (!hwPowerInit()) die("power");
  hwPowerPanelReset();                 // panel reset = ALDO3 power-cycle
  digitalWrite(PIN_TP_RESET, LOW); delay(20); digitalWrite(PIN_TP_RESET, HIGH); delay(20);
  if (!hwDisplayInit()) die("display");
  hwDisplayMadctl(TV_MADCTL);
  Serial.println("display up");
  if (!hwAudioInit()) Serial.println("audio: DISABLED (init failed)"); else Serial.println("audio up");
  { Preferences p; p.begin("jvtv-audio", false);                     // what the owner set last time
    if (p.isKey("vol"))  s_volume = p.getInt("vol", s_volume);
    if (p.isKey("mute")) s_muted  = p.getBool("mute", s_muted);
    p.end(); }
  webctlSetAudio(s_volume, s_muted);
  if (s_muted) { audioVolume(0); Serial.printf("audio: muted (volume %d)\n", s_volume); } else audioVolume(s_volume);   // the master gain only exists once this is called
  s_imuOk = hwImuInit(); if (!s_imuOk) Serial.println("imu: DISABLED (init failed)");
  if (s_imuOk) { hwImuTapInit(); hwImuTaskStart(); }
  if (!hwRtcInit()) Serial.println("rtc: DISABLED (init failed or time invalid - send T <epochUtc>)");
  Serial.printf("free heap after init: %u\n", (unsigned)ESP.getFreeHeap());

  inputInit();
  glitchInit();
  for (int i = 0; i < N_STATIONS; i++) CHANNELS[i]->begin();
  remoteCardBegin();
  webctlSetChannels(CHANNELS, N_STATIONS);
  Serial.printf("free heap after buffers: %u\n", (unsigned)ESP.getFreeHeap());
#ifdef TV_WIFI_AT_BOOT
  hwNetBegin();
#endif
  crtInit(false);
  tunerInit(millis());
  hwDisplayBrightness(0);
  // The set is "plugged in": switch it on, tube warms up, tuner hunts for lock.
  powerToggle(millis(), "boot");
  Serial.println("on air");
}

void setLoop() {
  static uint32_t frames = 0, t0 = millis();
  static TunerState lastState = TUNER_LOCKED;
  pollSerial();
  uint32_t now = millis();
  if (s_tunerOn) tunerTick(now, &s_tune);
  else { tunerInit(now); tunerTick(now, &s_tune); }
  if (s_tune.state != lastState) {
    Serial.printf("tuner: %s\n", stateName(s_tune.state)); lastState = s_tune.state;
    if (s_tune.state == TUNER_LOCKED) {
      s_locksSincePowerOn++; s_lockedAt = now;
      if (!s_remoteCard && CHANNELS[s_station]->start) CHANNELS[s_station]->start(now);
    }
  }
  // Programme over -> the set drifts off tune by itself and stays in snow.
  if (!s_remoteCard && s_tune.state == TUNER_LOCKED && !(CHANNELS[s_station]->info.flags & CH_ENDLESS) &&
      CHANNELS[s_station]->length && CHANNELS[s_station]->length() && now - s_lockedAt > CHANNELS[s_station]->length()) tunerDrop(now);
  if (s_autoTuneAt && !s_remoteCard && crtIsOn() && s_tune.state == TUNER_LOST && (int32_t)(now - s_autoTuneAt) >= 0) { s_autoTuneAt = 0; Serial.println("snow: no knock in 2 s, tuning in"); tunerKick(now); }
  { static uint32_t lastClk = 0;             // once a second: the test card's wall clock
    if (now - lastClk >= 1000) { lastClk = now;
      int h, m, sec, w; bool have = hwRtcGet(&h, &m, &sec, &w);
      testcardSetTime(have ? h : -1, m);
    } }
  crtTick(now, &s_crt);
  // Auto show: after the tuner has been through a full cycle and locked again,
  // enjoy it for a bit, switch the set off, wait in the dark, switch it on.
  if (s_show) {
    if (crtIsOn() && s_crt.state == CRT_ON && s_locksSincePowerOn >= 2 && now - s_lockedAt > 6000) powerToggle(now, "show");
    if (s_crt.state == CRT_OFF) { if (!s_offAt) s_offAt = now; else if (now - s_offAt > 3500) { s_offAt = 0; powerToggle(now, "show"); } }
  }
  // Compose: tuner's signal quality + tube geometry.
  s_g = s_tune.g;
  s_g.vScale = s_crt.vScale; s_g.hScale = s_crt.hScale; s_g.boost = s_crt.boost;
  s_g.raster = s_crt.raster; s_g.dot = s_crt.dot; s_g.dotR = s_crt.dotR;
  hwDisplayBrightness((uint8_t)(s_tune.brightness * s_crt.bright * s_brightMax / 255.0f));
  mixSound(now);
  if (s_remoteCard) remoteCardTick(now); else CHANNELS[s_station]->tick(now);
  hwDisplayPushFrame(renderStripe, nullptr);
  frames++;
  if (now - t0 >= 2000) {
    Serial.printf("fps %.1f  heap %u  crt %d  state %s d=%.2f  net %s\n",
                  frames * 1000.0f / (now - t0), (unsigned)ESP.getFreeHeap(),
                  (int)s_crt.state, stateName(s_tune.state), s_tune.detune, hwNetStatus());
    frames = 0; t0 = now;
  }
  if (stationIsGame()) {                     // keys belong to the game: press = down, release = up; long IO10 = tune away
    if (kIo10.pressed()) pushInput(SRC_KEYS, IN_A, true);
    if (kBoot.pressed()) pushInput(SRC_KEYS, IN_B, true);
    if (kIo10.held(800)) { pushInput(SRC_KEYS, IN_A, false); tuneKnob(now); }
    else {
      static bool aWas = false, bWas = false;
      bool aNow = (digitalRead(PIN_KEY_IO10) == HIGH) == false, bNow = (digitalRead(PIN_KEY_BOOT) == HIGH) == false;
      if (aWas && !aNow) pushInput(SRC_KEYS, IN_A, false);
      if (bWas && !bNow) pushInput(SRC_KEYS, IN_B, false);
      aWas = aNow; bWas = bNow;
    }
  } else {
    if (kIo10.pressed()) tuneKnob(now);
    kBoot.pressed();                                   // arm the edge/keep t & longFired fresh every frame
    // Mute toggles on a SHORT release so a long hold (REMOTE) does not flip it — but only
    // off the REMOTE card; on it, BOOT means something else (bootRemoteAndForget() below).
    // releasedShort() is still called unconditionally to keep its own wasHeld in sync.
    bool bootShortRelease = kBoot.releasedShort();
    if (!s_remoteCard && bootShortRelease) setMute(!s_muted);
  }
  if (kPwr.pressed()) { s_show = false; powerToggle(now, "PWR key"); }
  if (s_imuOk) pollOrientation(now);
  // A firm press on the set's own keys jolts the case like a knock. So an IMU knock waits
  // KNOCK_HOLD_MS (the jolt can reach the IMU before the key contact closes) and is dropped
  // if any key was down within KEY_QUIET_MS of it. Serial `n` knocks at once (tests).
  static const uint32_t KNOCK_HOLD_MS = 120, KEY_QUIET_MS = 400;
  static uint32_t keyDownAt = 0;                       // last loop any key was down (0 = never)
  static const char* pendHow = nullptr; static float pendG = 0; static uint32_t pendAt = 0;
  if (kIo10.isDown() || kBoot.isDown() || kPwr.isDown()) keyDownAt = now;
  if (s_imuOk && !pendHow) {
    int n = hwImuSoftTaps(), t = hwImuTapPoll();
    if (t)      { pendHow = t == 2 ? "hw double tap" : "hw tap"; pendG = hwImuLastTapG() > 0.4f ? hwImuLastTapG() : 0.9f; pendAt = now; }
    else if (n) { pendHow = "sw jerk"; pendG = hwImuLastTapG(); pendAt = now; }
  }
  if (pendHow && now - pendAt >= KNOCK_HOLD_MS) {
    if (keyDownAt && (int32_t)(keyDownAt - pendAt) > -(int32_t)KEY_QUIET_MS)
      Serial.printf("knock (%s) %.2f g ignored: a key was pressed\n", pendHow, pendG);
    else knock(now, pendHow, pendG);
    pendHow = nullptr;
  }
  hwNetTick(now);
  updateRemoteNet();
  remoteCardSetJoining(hwNetJoinPending() ? hwNetJoinSsid() : "");
  { // A join asked from the phone puts the card up (the join progress, then the new QR to
    // scan), even if it timed out while someone was typing the password.
    static bool wasJoining = false; bool joining = hwNetJoinPending();
    if (joining != wasJoining && crtIsOn() && (joining || hwNetJoinOk())) {
      if (!s_remoteCard) showRemoteCard(now);
      s_remoteShownAt = now; s_remoteDismissAt = 0;
      Serial.printf("remote: card up (%s)\n", joining ? "join started" : "joined, new QR");
    }
    wasJoining = joining;
  }
  if (webctlForgetRequested() && s_forgetState == FORGET_IDLE) forgetArm(now, "remote");
  { static bool wc = false;                  // phone controller: start once the LAN is up
    if (!wc && hwNetUp()) { wc = webctlBegin(); Serial.printf("webctl: %s\n", wc ? webctlUrl() : "FAILED"); }
    webctlTick(now); }
  { // The phone's clock and timezone (docs/NETWORK.md "Time"): staged by webctl.cpp's httpd
    // task, applied here because only the main loop touches I2C. The phone is authoritative
    // for the offset; NTP (below) is authoritative for the instant.
    uint32_t epoch; int offsetMin;
    if (webctlTimeRequested(&epoch, &offsetMin)) {
      bool ok = hwRtcSet(epoch, offsetMin);
      int savedTz; if (!hwRtcSavedTz(&savedTz) || savedTz != offsetMin) hwRtcSaveTz(offsetMin);   // every phone visit sends it: write NVS only on a change
      Serial.printf("time: phone set epoch %u offset %+d min -> rtc %s\n", (unsigned)epoch, offsetMin, ok ? "ok" : "FAILED");
    }
  }
  pollNtp(now);
  audioSaveIfDue(now);
  { // The set's own controls may arrive from a phone: act on them here, hand the rest back to the channel in order.
    InputEvent e, keep[32]; int n = 0;
    while (inputPoll(&e)) {
      if (e.down && e.key == IN_TUNE) { if (e.value < 0) tuneKnob(now); else tuneTo(e.value, now); }
      else if (e.down && e.key == IN_POWER) { s_show = false; powerToggle(now, "remote"); }
      else if (e.down && e.key == IN_MUTE)  setMute(!s_muted);
      else if (e.down && e.key == IN_VOLUME) { s_volume = e.value; if (!s_muted) audioVolume(s_volume); audioChanged(); Serial.printf("volume (phone) -> %d\n", s_volume); }
      else if (n < 32) keep[n++] = e;
    }
    for (int i = 0; i < n; i++) inputPush(keep[i]);
  }
  // Long BOOT: not on REMOTE, show it (fires the instant the 800 ms threshold crosses,
  // same as always); on REMOTE, bootRemoteAndForget() owns BOOT (mute/exit/forget).
  if (!s_remoteCard) {
    if (kBoot.held(800)) {
      if (stationIsGame()) pushInput(SRC_KEYS, IN_B, false);
      Serial.println("BOOT held -> REMOTE"); showRemoteCard(now);
    }
  } else {
    bootRemoteAndForget(now);
    // The card is for joining: once a NEW phone connects on the home network it steps aside
    // after 2 s and the set returns to what it was showing; unused, it steps aside after 60 s.
    // On the set's own AP a phone that joins is usually setting up Wi-Fi and still needs the
    // card (join progress, then the new QR), so there the 60 s restarts on every change
    // instead. Never while a Wi-Fi forget is waiting for its confirmation. Another phone = BOOT again.
    if (s_remoteCard && s_forgetState == FORGET_IDLE) {
      static NetMode lastMode = NET_AP;
      int c = webctlClients(); NetMode mode = hwNetMode();
      if (c != s_remoteClients || mode != lastMode || hwNetJoinPending()) s_remoteShownAt = now;
      if (mode == NET_HOME && c > s_remoteClients && !s_remoteDismissAt) { s_remoteDismissAt = now + 2000; Serial.println("remote: phone joined, stepping aside in 2 s"); }
      s_remoteClients = c; lastMode = mode;
      if ((s_remoteDismissAt && (int32_t)(now - s_remoteDismissAt) >= 0) || (int32_t)(now - s_remoteShownAt) >= 60000) {
        Serial.println(s_remoteDismissAt ? "remote: dismissed (phone joined)" : "remote: dismissed (60 s)"); hideRemoteCard(now);
      }
    }
  }
}
