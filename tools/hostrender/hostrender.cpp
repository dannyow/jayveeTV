// hostrender — renders JayVeeTV channels on a Mac exactly the way the
// platform composes a frame: tunerTick -> Glitch, crtTick -> tube geometry,
// channel tick(now), then glitchRender stripe by stripe (STRIPE_H rows, panel
// byte order) into a 480×480 RGB565 frame, which is un-swapped and written as
// PNG. Time is fake and deterministic: frame i is at t0 + i*1000/fps.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include "channels_host.h"
#include "platform/remotecard.h"
#include "channels/testcard/testcard.h"
#include "platform/glitch.h"
#include "platform/tuner.h"
#include "platform/crt.h"
#include "platform/input.h"
#include "platform/font5x7.h"
#include "png.h"

uint32_t   hostNowMs = 0;        // the stub Arduino.h's millis()
#include "Arduino.h"
HostSerial Serial;

// ---- CLI -------------------------------------------------------------------
static void usage() {
  fprintf(stderr,
    "hostrender --channel NAME [options]\n"
    "  --frames N            frames to render (default 24)\n"
    "  --fps N               frame rate of the fake clock (default 24)\n"
    "  --t0 MS               fake millis() at frame 0 (default 0)\n"
    "  --out DIR             output dir, files fNNN.png (default out/)\n"
    "  --ppm                 write PPM instead of PNG\n"
    "  --tuner SPEC          locked|losing|lost|tuning|auto, optionally a timeline:\n"
    "                        \"locked,kick@2000,lost@6000,drop@8000\" (state@ms = tunerStart,\n"
    "                        kick@ms = tunerKick, drop@ms = tunerDrop). Default locked.\n"
    "  --crt SPEC            on|warmup|off, optionally a timeline: \"on,off@1500,on@4000\"\n"
    "                        (off@ms = crtPowerOff → collapse/afterglow, on@ms = crtPowerOn → warm-up).\n"
    "                        Default on.\n"
    "  --boot                the board's boot: --crt warmup --tuner tuning\n"
    "  --chroma 0..16        force gChroma every frame (default: what the tuner says)\n"
    "  --input \"t=MS key=NAME down=0|1 [player=N] [value=V]\"   repeatable; NAME = LEFT, A,\n"
    "                        KNOB, HEX7, TUNE ... (IN_ prefix optional). Pushed before tick().\n"
    "  --sheet FILE.png      contact sheet: one tile per second, 240x240 each, labelled\n"
    "  --no-dim              don't multiply the picture by panel brightness (tuner sag, warm-up)\n"
    "  --clock HH:MM|none    the wall clock the platform hands the test card (default 10:10, as on the set)\n"
    "  --list                list channels and exit\n"
    "  --url URL             what the REMOTE card encodes in its QR, e.g. http://192.168.4.1/\n"
    "  --check ID             docs/CHANNELS.md's \"definition of done\" check: 5 s at 24 fps\n"
    "                        (no --input: presses START and sweeps the slider if the panel has them),\n"
    "                        fails (exit 1) if the mean frame is over 1 ms on the host, writes\n"
    "                        out/<id>/sheet.png. Writes nothing into the repo.\n"
    "  --drive               play the channel through its panel like --check does (START, slider)\n"
    "  --quiet               no per-frame log\n");
}

static const char* KEY_NAMES[IN_KEY_COUNT] = {
  "NONE", "LEFT", "RIGHT", "UP", "DOWN", "A", "B", "START", "SELECT", "KNOB",
  "HEX0", "HEX1", "HEX2", "HEX3", "HEX4", "HEX5", "HEX6", "HEX7",
  "HEX8", "HEX9", "HEXA", "HEXB", "HEXC", "HEXD", "HEXE", "HEXF",
  "TUNE", "POWER", "MUTE", "CHAR", "VOLUME" };
static int keyByName(const char* s) {
  if (strncasecmp(s, "IN_", 3) == 0) s += 3;
  for (int i = 0; i < IN_KEY_COUNT; i++) if (strcasecmp(s, KEY_NAMES[i]) == 0) return i;
  return -1;
}

struct TunerEv { uint32_t t; int kind; };              // kind: 0..3 = state, 4 = kick, 5 = drop, 6 = auto
struct CrtEv   { uint32_t t; bool on; };
struct InputEv { uint32_t t; InputEvent e; };

static bool parseTuner(const char* spec, std::vector<TunerEv>& evs) {
  std::string s(spec); size_t p = 0;
  while (p <= s.size()) {
    size_t c = s.find(',', p); if (c == std::string::npos) c = s.size();
    std::string tok = s.substr(p, c - p); p = c + 1;
    if (tok.empty()) continue;
    uint32_t t = 0; size_t at = tok.find('@');
    if (at != std::string::npos) { t = (uint32_t)atol(tok.c_str() + at + 1); tok = tok.substr(0, at); }
    int kind;
    if      (tok == "locked") kind = TUNER_LOCKED; else if (tok == "losing") kind = TUNER_LOSING;
    else if (tok == "lost")   kind = TUNER_LOST;   else if (tok == "tuning") kind = TUNER_TUNING;
    else if (tok == "kick")   kind = 4;            else if (tok == "drop")   kind = 5;
    else if (tok == "auto")   kind = 6;
    else { fprintf(stderr, "bad --tuner token '%s'\n", tok.c_str()); return false; }
    evs.push_back({ t, kind });
  }
  return true;
}
static bool parseCrt(const char* spec, std::vector<CrtEv>& evs, int* initial) {   // initial: 0 off, 1 warmup, 2 on
  std::string s(spec); size_t p = 0; bool first = true;
  while (p <= s.size()) {
    size_t c = s.find(',', p); if (c == std::string::npos) c = s.size();
    std::string tok = s.substr(p, c - p); p = c + 1;
    if (tok.empty()) continue;
    size_t at = tok.find('@');
    if (first && at == std::string::npos) {
      if (tok == "off") *initial = 0; else if (tok == "warmup") *initial = 1; else if (tok == "on") *initial = 2;
      else { fprintf(stderr, "bad --crt token '%s'\n", tok.c_str()); return false; }
      first = false; continue;
    }
    if (at == std::string::npos) { fprintf(stderr, "--crt: '%s' needs @ms\n", tok.c_str()); return false; }
    uint32_t t = (uint32_t)atol(tok.c_str() + at + 1); tok = tok.substr(0, at);
    if (tok == "on") evs.push_back({ t, true }); else if (tok == "off") evs.push_back({ t, false });
    else { fprintf(stderr, "bad --crt token '%s'\n", tok.c_str()); return false; }
    first = false;
  }
  return true;
}
static bool parseInput(const char* spec, InputEv* out) {
  InputEv ev = {}; ev.e.src = SRC_SERIAL; ev.e.down = 1; bool haveKey = false, haveT = false;
  std::string s(spec); size_t p = 0;
  while (p < s.size()) {
    while (p < s.size() && s[p] == ' ') p++;
    size_t e = s.find(' ', p); if (e == std::string::npos) e = s.size();
    std::string kv = s.substr(p, e - p); p = e;
    size_t eq = kv.find('='); if (eq == std::string::npos) { fprintf(stderr, "--input: '%s' is not k=v\n", kv.c_str()); return false; }
    std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
    if (k == "t") { ev.t = (uint32_t)atol(v.c_str()); haveT = true; }
    else if (k == "key") { int ki = keyByName(v.c_str()); if (ki < 0) { fprintf(stderr, "--input: unknown key '%s'\n", v.c_str()); return false; } ev.e.key = (uint8_t)ki; haveKey = true; }
    else if (k == "down") ev.e.down = (uint8_t)atoi(v.c_str());
    else if (k == "player") ev.e.player = (uint8_t)atoi(v.c_str());
    else if (k == "value") ev.e.value = (int16_t)atoi(v.c_str());
    else { fprintf(stderr, "--input: unknown field '%s'\n", k.c_str()); return false; }
  }
  if (!haveKey || !haveT) { fprintf(stderr, "--input needs t= and key=\n"); return false; }
  *out = ev; return true;
}

// ---- Picture helpers -----------------------------------------------------------
static inline void rgb565to888(uint16_t c, uint8_t* o) {
  int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
  o[0] = (uint8_t)((r << 3) | (r >> 2)); o[1] = (uint8_t)((g << 2) | (g >> 4)); o[2] = (uint8_t)((b << 3) | (b >> 2));
}
static void drawText(uint8_t* rgb, int W, int H, int x0, int y0, int scale, const char* s) {
  for (int i = 0; s[i]; i++) {
    const uint8_t* g = font5x7Glyph(s[i]);
    for (int col = 0; col < 5; col++) for (int row = 0; row < 7; row++) if ((g[col] >> row) & 1)
      for (int sy = 0; sy < scale; sy++) for (int sx = 0; sx < scale; sx++) {
        int x = x0 + (i * 6 + col) * scale + sx, y = y0 + row * scale + sy;
        if (x >= 0 && x < W && y >= 0 && y < H) { uint8_t* p = rgb + (size_t)(y * W + x) * 3; p[0] = p[1] = p[2] = 255; }
      }
  }
}
static bool writeFrame(const std::string& path, bool ppm, const uint8_t* rgb, int w, int h) {
  if (!ppm) return pngWrite(path.c_str(), rgb, w, h);
  FILE* f = fopen(path.c_str(), "wb"); if (!f) return false;
  fprintf(f, "P6\n%d %d\n255\n", w, h); fwrite(rgb, 1, (size_t)w * h * 3, f); return fclose(f) == 0;
}
static void mkdirs(const std::string& dir) {          // mkdir -p
  for (size_t p = 1; p <= dir.size(); p++) if (p == dir.size() || dir[p] == '/') mkdir(dir.substr(0, p).c_str(), 0755);
}
static const char* tunerName(TunerState s) { static const char* N[4] = { "locked", "losing", "lost", "tuning" }; return N[s]; }
static const char* crtName(CrtState s) { static const char* N[5] = { "off", "warmup", "on", "collapse", "afterglow" }; return N[s]; }

int main(int argc, char** argv) {
  const char* chName = nullptr; int frames = 24, fps = 24; uint32_t t0 = 0; std::string outDir = "out";
  bool ppm = false, dim = true, quiet = false, boot = false; int chroma = -1; std::string sheet;
  std::vector<TunerEv> tunerEvs; std::vector<CrtEv> crtEvs; int crtInitial = 2; std::vector<InputEv> inputs;
  bool tunerGiven = false, crtGiven = false, framesGiven = false, sheetGiven = false, drive = false;
  std::string checkId;
  testcardSetTime(10, 10);                 // the set always has a clock once a phone or NTP set it; --clock overrides
  for (int i = 1; i < argc; i++) {
    auto need = [&](const char* opt) -> const char* { if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", opt); exit(2); } return argv[++i]; };
    if      (!strcmp(argv[i], "--channel")) chName = need("--channel");
    else if (!strcmp(argv[i], "--frames"))  { frames = atoi(need("--frames")); framesGiven = true; }
    else if (!strcmp(argv[i], "--fps"))     fps = atoi(need("--fps"));
    else if (!strcmp(argv[i], "--t0"))      t0 = (uint32_t)atol(need("--t0"));
    else if (!strcmp(argv[i], "--out"))     outDir = need("--out");
    else if (!strcmp(argv[i], "--ppm"))     ppm = true;
    else if (!strcmp(argv[i], "--tuner"))   { if (!parseTuner(need("--tuner"), tunerEvs)) return 2; tunerGiven = true; }
    else if (!strcmp(argv[i], "--crt"))     { if (!parseCrt(need("--crt"), crtEvs, &crtInitial)) return 2; crtGiven = true; }
    else if (!strcmp(argv[i], "--boot"))    boot = true;
    else if (!strcmp(argv[i], "--chroma"))  chroma = atoi(need("--chroma"));
    else if (!strcmp(argv[i], "--input"))   { InputEv ev; if (!parseInput(need("--input"), &ev)) return 2; inputs.push_back(ev); }
    else if (!strcmp(argv[i], "--sheet"))   { sheet = need("--sheet"); sheetGiven = true; }
    else if (!strcmp(argv[i], "--no-dim"))  dim = false;
    else if (!strcmp(argv[i], "--clock")) { const char* c = need("--clock"); int h = -1, m = -1; if (strcmp(c, "none")) sscanf(c, "%d:%d", &h, &m); testcardSetTime(h, m); }
    else if (!strcmp(argv[i], "--url"))     remoteCardSetNet(REMOTE_NET_HOME, need("--url"), "jayveetv.local");
    else if (!strcmp(argv[i], "--check"))   checkId = need("--check");
    else if (!strcmp(argv[i], "--drive"))   drive = true;
    else if (!strcmp(argv[i], "--quiet"))   quiet = true;
    else if (!strcmp(argv[i], "--list"))    { for (int k = 0; k < hostChannelCount(); k++) printf("%s\n", hostChannelAt(k)->info.id); return 0; }
    else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
    else { fprintf(stderr, "unknown option %s\n", argv[i]); usage(); return 2; }
  }
  bool checkMode = !checkId.empty();
  if (checkMode) {
    chName = checkId.c_str();
    if (!framesGiven) frames = fps > 0 ? fps * 5 : 120;   // 5 s
    if (!sheetGiven) sheet = "out/" + checkId + "/sheet.png";
  }
  if (!chName) { usage(); return 2; }
  if (inputs.empty() && (checkMode || drive)) {
    // No --input given: drive the channel the way its own phone panel would, so the
    // sheet shows play rather than an idle screen. START (if the panel has it) at
    // 0.5 s; the slider (if any) swept top to bottom and back over the run.
    const Channel* c = hostChannel(chName);
    const char* panel = c && c->info.panel ? c->info.panel : "";
    if (strstr(panel, "\"START\"")) {
      InputEv dn = {}; dn.t = 500; dn.e = { SRC_SERIAL, IN_START, 0, 1, 0, 0 }; inputs.push_back(dn);
      InputEv up = dn; up.t = 600; up.e.down = 0;                               inputs.push_back(up);
    }
    if (strstr(panel, "\"slider\"")) {
      for (int k = 0; k < 20; k++) {
        uint32_t t = (uint32_t)((int64_t)k * frames * 1000 / fps / 20);
        int32_t sweep = -32768 + (int32_t)((int64_t)65535 * (k % 10) / 9);
        InputEv ev = {}; ev.t = t; ev.e = { SRC_SERIAL, IN_KNOB, 0, 1, (int16_t)sweep, 0 };
        inputs.push_back(ev);
      }
    }
    std::stable_sort(inputs.begin(), inputs.end(), [](const InputEv& a, const InputEv& b) { return a.t < b.t; });
  }
  if (fps <= 0 || frames <= 0) { fprintf(stderr, "fps/frames must be > 0\n"); return 2; }
  if (chroma > 16) chroma = 16;
  const Channel* ch = nullptr;
  ch = hostChannel(chName);
  if (!ch) { fprintf(stderr, "no channel '%s' (see --list; add it in channels_host.cpp)\n", chName); return 2; }
  if (boot) { if (!crtGiven) crtInitial = 1; if (!tunerGiven) tunerEvs.push_back({ 0, TUNER_TUNING }); }
  if (!checkMode) mkdirs(outDir);
  if (!sheet.empty()) { size_t sl = sheet.rfind('/'); if (sl != std::string::npos) mkdirs(sheet.substr(0, sl)); }

  // ---- setup(), as the platform does it ---------------------------------------
  hostNowMs = t0;
  glitchInit(); inputInit();
  ch->begin();
  crtInit(crtInitial == 2);
  if (crtInitial == 1) crtPowerOn(t0);
  tunerInit(t0);
  // Initial tuner state: the first event without a time (or at t0) is applied now.
  size_t nextTuner = 0, nextCrt = 0, nextInput = 0;
  auto applyTuner = [&](const TunerEv& ev, uint32_t now) {
    switch (ev.kind) {
      case 4: tunerKick(now); break;
      case 5: tunerDrop(now); break;
      case 6: tunerSetAuto(true); tunerStart(now, TUNER_LOCKED); break;
      default: tunerStart(now, (TunerState)ev.kind); break;
    }
  };
  while (nextTuner < tunerEvs.size() && tunerEvs[nextTuner].t <= t0) applyTuner(tunerEvs[nextTuner++], t0);
  while (nextCrt < crtEvs.size() && crtEvs[nextCrt].t <= t0) { if (crtEvs[nextCrt].on) crtPowerOn(t0); else crtPowerOff(t0); nextCrt++; }

  std::vector<uint16_t> fb((size_t)LCD_W * LCD_H);
  std::vector<uint8_t>  rgb((size_t)LCD_W * LCD_H * 3);
  // Contact sheet: one 240×240 tile per second.
  const int TILE = 240, PAD = 4;
  std::vector<std::vector<uint8_t>> tiles;

  TunerOut tune = {}; CrtOut crt = {}; Glitch g = {};
  bool first = true; TunerState lastState = TUNER_LOCKED; uint32_t lockedAt = t0;
  double renderTotalMs = 0, renderMaxMs = 0;

  for (int i = 0; i < frames; i++) {
    uint32_t now = t0 + (uint32_t)(((uint64_t)i * 1000 + fps / 2) / fps);
    hostNowMs = now;
    while (nextTuner < tunerEvs.size() && tunerEvs[nextTuner].t <= now) applyTuner(tunerEvs[nextTuner++], now);
    while (nextCrt < crtEvs.size() && crtEvs[nextCrt].t <= now) { if (crtEvs[nextCrt].on) crtPowerOn(now); else crtPowerOff(now); nextCrt++; }

    // loop(), as the platform does it
    tunerTick(now, &tune);
    if (first || tune.state != lastState) {
      // The platform calls start() when the tuner arrives at LOCKED. A harness
      // run that begins locked counts as "just locked" so game channels start.
      if (tune.state == TUNER_LOCKED) { lockedAt = now; if (ch->start) ch->start(now); }
      lastState = tune.state; first = false;
    }
    bool endless = (ch->info.flags & CH_ENDLESS) != 0;
    if (!endless && tune.state == TUNER_LOCKED && ch->length && ch->length() && now - lockedAt > ch->length()) tunerDrop(now);
    crtTick(now, &crt);
    g = tune.g;
    g.vScale = crt.vScale; g.hScale = crt.hScale; g.boost = crt.boost;
    g.raster = crt.raster; g.dot = crt.dot; g.dotR = crt.dotR;
    float brightness = tune.brightness * crt.bright / 255.0f;
    if (chroma >= 0) gChroma = chroma;
    while (nextInput < inputs.size() && inputs[nextInput].t <= now) {
      InputEvent e = inputs[nextInput].e; e.t = now;
      if (!inputPush(e)) fprintf(stderr, "f%03d: input queue full, event dropped\n", i);
      nextInput++;
    }
    auto tA = std::chrono::steady_clock::now();
    ch->tick(now);
    bool wide = (ch->info.flags & CH_WIDE) != 0;
    for (int y0 = 0; y0 < LCD_H; y0 += STRIPE_H)
      glitchRender(fb.data() + (size_t)y0 * LCD_W, y0, STRIPE_H, ch->row, wide, g);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tA).count();
    renderTotalMs += ms; if (ms > renderMaxMs) renderMaxMs = ms;

    // Panel byte order → RGB565 → RGB888, dimmed by the panel brightness.
    int k = dim ? (int)(brightness * 256.0f + 0.5f) : 256;
    for (size_t p = 0; p < fb.size(); p++) {
      uint8_t* o = &rgb[p * 3]; rgb565to888(SWAP16(fb[p]), o);
      if (k < 256) { o[0] = (uint8_t)(o[0] * k >> 8); o[1] = (uint8_t)(o[1] * k >> 8); o[2] = (uint8_t)(o[2] * k >> 8); }
    }
    if (!checkMode) {
      char name[64]; snprintf(name, sizeof name, "/f%03d.%s", i, ppm ? "ppm" : "png");
      std::string path = outDir + name;
      if (!writeFrame(path, ppm, rgb.data(), LCD_W, LCD_H)) { fprintf(stderr, "cannot write %s\n", path.c_str()); return 1; }
    }
    if (!quiet) fprintf(stderr, "f%03d t=%6ums tuner=%-6s d=%.2f crt=%-9s bright=%.2f chroma=%2d roll=%3d snow=%2d render=%.1fms\n",
                        i, now, tunerName(tune.state), tune.detune, crtName(crt.state), brightness, gChroma, g.roll, g.snow, ms);
    if (!sheet.empty() && i % fps == 0) {
      std::vector<uint8_t> t((size_t)TILE * TILE * 3);
      for (int y = 0; y < TILE; y++) for (int x = 0; x < TILE; x++) for (int c = 0; c < 3; c++) {
        const uint8_t* s = &rgb[((size_t)(2 * y) * LCD_W + 2 * x) * 3 + c];
        t[((size_t)y * TILE + x) * 3 + c] = (uint8_t)((s[0] + s[3] + s[LCD_W * 3] + s[LCD_W * 3 + 3]) >> 2);
      }
      char label[32]; snprintf(label, sizeof label, "%03d T=%uMS", i, now);
      for (int y = 2; y < 2 + 7 * 2 + 4; y++) for (int x = 2; x < 2 + (int)strlen(label) * 6 * 2 + 4; x++) { uint8_t* p = &t[((size_t)y * TILE + x) * 3]; p[0] = p[1] = p[2] = 0; }
      drawText(t.data(), TILE, TILE, 4, 4, 2, label);
      tiles.push_back(std::move(t));
    }
  }
  if (!sheet.empty() && !tiles.empty()) {
    int n = (int)tiles.size(), cols = n < 6 ? n : 6, rows = (n + cols - 1) / cols;
    int W = cols * TILE + (cols + 1) * PAD, H = rows * TILE + (rows + 1) * PAD;
    std::vector<uint8_t> s((size_t)W * H * 3, 24);
    for (int k = 0; k < n; k++) {
      int ox = PAD + (k % cols) * (TILE + PAD), oy = PAD + (k / cols) * (TILE + PAD);
      for (int y = 0; y < TILE; y++) memcpy(&s[((size_t)(oy + y) * W + ox) * 3], &tiles[k][(size_t)y * TILE * 3], TILE * 3);
    }
    if (!pngWrite(sheet.c_str(), s.data(), W, H)) { fprintf(stderr, "cannot write %s\n", sheet.c_str()); return 1; }
    fprintf(stderr, "sheet %s: %d tiles (%dx%d)\n", sheet.c_str(), n, cols, rows);
  }
  double meanMs = renderTotalMs / frames;
  fprintf(stderr, "%d frames of '%s' -> %s  render (tick+rows, host): mean %.2f ms, max %.2f ms\n",
          frames, ch->info.id, checkMode ? sheet.c_str() : (outDir + "/").c_str(), meanMs, renderMaxMs);
  if (checkMode && meanMs > 1.0) {
    fprintf(stderr, "CHECK FAILED: %s mean frame %.2f ms > 1 ms host budget (docs/CHANNELS.md)\n", checkId.c_str(), meanMs);
    return 1;
  }
  if (checkMode) fprintf(stderr, "CHECK ok: %s mean %.2f ms, max %.2f ms\n", checkId.c_str(), meanMs, renderMaxMs);
  return 0;
}
