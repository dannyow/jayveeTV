// Title in ×3, a QR code (Nayuki qrcodegen, version ≤ 5, ECC low) — AP mode:
// "WIFI:T:WPA;S:..;P:..;;", so the phone's own camera joins the set's own
// network; home mode: the controller URL — two lines of text under it, and
// "N CONNECTED" in ×2. White on black, only the status line carries a
// colour so gChroma still has something to drain. Two overlays borrow the
// same card when the network is mid-change: a join in flight ("TUNING TO
// <ssid>") and the physical forget confirmation ("FORGET WI-FI? PRESS
// BOOT"), both pushed from set.cpp's key/net handling. The QR is rebuilt in
// remoteCardTick() when the payload changes; remoteCardRow() is a table lookup per
// pixel: s_rowMod[y] / s_colMod[x] map picture pixels to module indices.
#include "platform/remotecard.h"
#include "platform/channel.h"
#include "platform/hw/webctl.h"
#include "platform/font5x7.h"
#include "platform/qrcodegen.h"
#include <string.h>
#include <stdio.h>

#define RGB(r,g,b) ((uint16_t)((((r)&0xF8)<<8) | (((g)&0xFC)<<3) | ((b)>>3)))

// Host harness and any build without hw/webctl.cpp: nobody is connected.
__attribute__((weak)) int webctlClients() { return 0; }

static const uint16_t C_BLACK = 0x0000, C_WHITE = 0xFFFF;
static const uint8_t  ON_RGB[3]  = { 60, 235, 90 };    // status when a phone is on
static const uint8_t  OFF_RGB[3] = { 150, 150, 150 };  // status when nobody is
static uint16_t C_STATUS;
static int s_chromaBuilt = -1, s_clientsBuilt = -1;

// Layout (logical px): title 6..27, QR region 30..174, line1 180..194,
// line2 197..211, status 219..233. Sized so the widest realistic payload
// (byte-mode Wi-Fi QR, version 3) sits at module scale 4 with room to spare.
static const int QR_Y0 = 30, QR_H = 144, QR_MAX = 45;      // 45 = v5 (37) + 2×4 quiet, the widest we index
static const int QR_QUIET = 4;

static char    s_payload[64] = "", s_builtPayload[64] = "\x01";  // "\x01" != "" so the first tick builds
static char    s_line1[24] = "", s_line2[24] = "";               // what is printed under the QR, uppercase
static char    s_status[16] = "0 CONNECTED";
static bool    s_haveQr = false;
static RemoteNet s_net = REMOTE_NET_NONE;
static uint8_t s_mods[QR_MAX * QR_MAX];                  // 1 = white module
static uint8_t s_colMod[LOG_W], s_rowMod[LOG_H];         // 0xFF = outside the QR
static int     s_qx0 = 0, s_qx1 = 0;                     // x span of the QR, for remoteCardRow()

static bool    s_forgetArmed = false;

struct Text { int x, y, scale; const char* s; int w; const uint16_t* col; };
static Text s_title  = { 120 - (6*6*3 - 3)/2, 6,   3, "REMOTE",     6*6*3 - 3, &C_WHITE };
static Text s_line1T = { 0,                    180, 2, s_line1,     0,        &C_WHITE };
static Text s_line2T = { 0,                    197, 2, s_line2,     0,        &C_WHITE };
static Text s_stat    = { 0,                   219, 2, s_status,    0,        &C_STATUS };
static Text s_noNet   = { 120 - (10*6*2 - 2)/2, 106, 2, "NO NETWORK", 10*6*2 - 2, &C_WHITE };
static char s_joinLine[24] = "";
static Text s_joinT1  = { 120 - (9*6*2 - 2)/2,  100, 2, "TUNING TO",     9*6*2 - 2,  &C_WHITE };
static Text s_joinT2  = { 0,                    128, 2, s_joinLine,     0,           &C_WHITE };
static Text s_forgT1  = { 120 - (13*6*2 - 2)/2, 96,  2, "FORGET WI-FI?", 13*6*2 - 2, &C_WHITE };
static Text s_forgT2  = { 120 - (10*6*2 - 2)/2, 132, 2, "PRESS BOOT",    10*6*2 - 2, &C_WHITE };

static void centre(Text& t) { int n = (int)strlen(t.s); t.w = n ? n * 6 * t.scale - t.scale : 0; t.x = 120 - t.w / 2; }

static uint16_t shade(const uint8_t* c) {
  int r = c[0], g = c[1], b = c[2];
  int l = (r * 77 + g * 150 + b * 29) >> 8;
  r = l + ((r - l) * gChroma) / 16; g = l + ((g - l) * gChroma) / 16; b = l + ((b - l) * gChroma) / 16;
  return RGB(r, g, b);
}
static void buildPalette() { C_STATUS = shade(s_clientsBuilt > 0 ? ON_RGB : OFF_RGB); s_chromaBuilt = gChroma; }

static void upperInto(char* dst, size_t n, const char* src) {
  strncpy(dst, src ? src : "", n - 1); dst[n - 1] = 0;
  for (char* c = dst; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
}

void remoteCardSetNet(RemoteNet mode, const char* a, const char* b) {
  s_net = mode;
  if (mode == REMOTE_NET_NONE) { s_payload[0] = 0; s_line1[0] = 0; s_line2[0] = 0; return; }
  if (mode == REMOTE_NET_AP) {
    snprintf(s_payload, sizeof s_payload, "WIFI:T:WPA;S:%s;P:%s;;", a, b);
    upperInto(s_line1, sizeof s_line1, a);
    upperInto(s_line2, sizeof s_line2, b);
  } else {                                                // REMOTE_NET_HOME
    strncpy(s_payload, a, sizeof s_payload - 1); s_payload[sizeof s_payload - 1] = 0;
    // The line under the code: the full URL if it fits ×2 (≤ 20 chars), else what a human types.
    char t[48]; strncpy(t, a, sizeof t - 1); t[sizeof t - 1] = 0;
    if (strlen(t) > 20) {
      if (strncmp(t, "http://", 7) == 0) memmove(t, t + 7, strlen(t + 7) + 1);
      size_t m = strlen(t); if (m > 1 && t[m-1] == '/') t[m-1] = 0;
    }
    upperInto(s_line1, sizeof s_line1, t);
    upperInto(s_line2, sizeof s_line2, b);
  }
}

void remoteCardSetJoining(const char* ssid) {
  if (!ssid || !ssid[0]) { s_joinLine[0] = 0; return; }
  upperInto(s_joinLine, sizeof s_joinLine, ssid);
}

void remoteCardSetForgetArmed(bool armed) { s_forgetArmed = armed; }

static void buildQr() {
  strcpy(s_builtPayload, s_payload);
  memset(s_colMod, 0xFF, sizeof s_colMod); memset(s_rowMod, 0xFF, sizeof s_rowMod);
  s_haveQr = false; s_qx0 = s_qx1 = 0;
  if (!s_payload[0]) return;

  centre(s_line1T); centre(s_line2T);

  uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(5)], qr[qrcodegen_BUFFER_LEN_FOR_VERSION(5)];
  if (!qrcodegen_encodeText(s_payload, tmp, qr, qrcodegen_Ecc_LOW, 1, 5, qrcodegen_Mask_AUTO, true)) return;
  int size = qrcodegen_getSize(qr);                       // 21..37
  int s = QR_H / (size + 2 * QR_QUIET);                   // module = s×s logical px, ≥ 4 (spec), ≤ 6
  if (s > 6) s = 6;
  if (s < 4) s = 4;                                        // v5 at 4 px leaves 10 px = 2.5 modules of quiet: still black all round
  int box = size * s;
  s_qx0 = (LOG_W - box) / 2; s_qx1 = s_qx0 + box;
  int qy0 = QR_Y0 + (QR_H - box) / 2;
  for (int y = 0; y < size; y++) for (int x = 0; x < size; x++) s_mods[y * QR_MAX + x] = qrcodegen_getModule(qr, x, y) ? 1 : 0;
  for (int x = s_qx0; x < s_qx1; x++) s_colMod[x] = (uint8_t)((x - s_qx0) / s);
  for (int y = qy0;   y < qy0 + box; y++) s_rowMod[y] = (uint8_t)((y - qy0) / s);
  s_haveQr = true;
}

void remoteCardBegin() { s_clientsBuilt = 0; buildPalette(); buildQr(); centre(s_stat); }

void remoteCardTick(uint32_t) {
  if (strcmp(s_payload, s_builtPayload) != 0) buildQr();
  int n = webctlClients();
  if (n != s_clientsBuilt) { s_clientsBuilt = n; snprintf(s_status, sizeof(s_status), "%d CONNECTED", n); centre(s_stat); buildPalette(); }
  if (gChroma != s_chromaBuilt) buildPalette();
  centre(s_joinT2);
}

static inline void textRow(const Text& t, int y, uint16_t* dst) {
  if (y < t.y || y >= t.y + 7 * t.scale || t.w <= 0) return;
  int gy = (y - t.y) / t.scale;
  for (int x = t.x; x < t.x + t.w; x++) {
    int gx = (x - t.x) / t.scale, col = gx % 6;
    if (col == 5) continue;
    if ((font5x7Glyph(t.s[gx / 6])[col] >> gy) & 1) dst[x] = *t.col;
  }
}

void remoteCardRow(int y, uint16_t* dst) {
  for (int x = 0; x < LOG_W; x++) dst[x] = C_BLACK;
  if (s_forgetArmed) {                                    // takes over the whole card except the title
    textRow(s_title, y, dst);
    textRow(s_forgT1, y, dst);
    textRow(s_forgT2, y, dst);
    return;
  }
  if (s_joinLine[0]) {
    textRow(s_title, y, dst);
    textRow(s_joinT1, y, dst);
    textRow(s_joinT2, y, dst);
    return;
  }
  if (s_haveQr) {
    uint8_t my = s_rowMod[y];
    if (my != 0xFF) {
      const uint8_t* mr = &s_mods[my * QR_MAX];
      for (int x = s_qx0; x < s_qx1; x++) if (mr[s_colMod[x]]) dst[x] = C_WHITE;
    }
    textRow(s_line1T, y, dst);
    textRow(s_line2T, y, dst);
  } else if (!s_payload[0]) {
    textRow(s_noNet, y, dst);
  }
  textRow(s_title, y, dst);
  textRow(s_stat, y, dst);
}
