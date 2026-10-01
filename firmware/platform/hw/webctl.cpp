// Web controller: esp_http_server (ESP-IDF, ships with the Arduino core, WS
// support is on in the C6 sdkconfig) serving the gzipped phone page, the
// station list, and one WebSocket per phone. See webctl.h for the protocol
// and the threading rule.
#include "platform/hw/webctl.h"
#include "platform/hw/webctl_page.h"
#include "platform/hw/net.h"
#include "platform/input.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <unistd.h>

// ---- state shared between the httpd task and the main loop ------------------
struct Slot {
  int      fd;            // -1 = free
  uint32_t held;          // bit per InputKey currently down (IN_KEY_COUNT = 28 < 32)
  int16_t  axis;          // latest knob value from the phone
  bool     axisDirty;     // a knob value is waiting to be pushed
  uint32_t lastSeenMs;    // last frame from this phone (millis)
  uint32_t lastAxisMs;    // last IN_KNOB pushed (rate limit)
};
static Slot s_slot[INPUT_PLAYERS];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

struct Staged { uint8_t key, down, player; int16_t value; };   // key edges (+ value for IN_CHAR/IN_TUNE), httpd task -> webctlTick
static const int SQ = 64;
static Staged s_sq[SQ];
static volatile int s_sqHead = 0, s_sqTail = 0;
static volatile int s_dropped = 0;

static httpd_handle_t s_srv = nullptr;
static volatile int s_countDirty = 0;                // broadcast "s n" pending
static volatile int s_chDirty = 0;                   // broadcast "ch n" pending
static volatile int s_netDirty = 1;                  // broadcast "st net=.." pending (fires once right after begin)
static volatile NetMode s_lastNetMode = NET_AP;
static volatile int s_curStation = 0;
static volatile bool s_forgetRequested = false;      // POST /api/wifi/forget -> the platform arms the on-screen confirm
static const uint32_t AXIS_MIN_MS = 33;              // ~30 Hz per player
static const uint32_t STALE_MS    = 20000;           // heartbeat is every 5 s

bool webctlForgetRequested() { bool r = s_forgetRequested; s_forgetRequested = false; return r; }

// "tm <epoch> <offsetMin>" staged for the main loop (mux-protected: two values that must
// land together). See webctl.h and docs/NETWORK.md "Time".
static volatile bool     s_timeRequested = false;
static volatile uint32_t s_timeEpoch = 0;
static volatile int16_t  s_timeOffset = 0;

bool webctlTimeRequested(uint32_t* epochUtc, int* offsetMin) {
  portENTER_CRITICAL(&s_mux);
  bool have = s_timeRequested;
  if (have) { *epochUtc = s_timeEpoch; *offsetMin = s_timeOffset; s_timeRequested = false; }
  portEXIT_CRITICAL(&s_mux);
  return have;
}

// ---- station list, for /channels.json and "ch n" -------------------------------
static const Channel* const* s_chs = nullptr;
static int s_chCount = 0;

void webctlSetChannels(const Channel* const* chs, int n) { s_chs = chs; s_chCount = n; }
void webctlSetStation(int n) { if (n != s_curStation) { s_curStation = n; s_chDirty = 1; } }
static volatile int s_vol = 20, s_mute = 0, s_audioDirty = 0;
void webctlSetAudio(int volume, bool muted) { if (volume != s_vol || (int)muted != s_mute) { s_vol = volume; s_mute = muted; s_audioDirty = 1; } }

static void stage(uint8_t key, uint8_t down, uint8_t player, int16_t value = 0) {   // caller holds s_mux
  int next = (s_sqTail + 1) % SQ;
  if (next == s_sqHead) { s_dropped++; return; }
  s_sq[s_sqTail] = { key, down, player, value }; s_sqTail = next;
}

// ---- key names ----------------------------------------------------------------
struct KeyName { const char* name; uint8_t key; };
static const KeyName KEYS[] = {
  { "LEFT", IN_LEFT }, { "RIGHT", IN_RIGHT }, { "UP", IN_UP }, { "DOWN", IN_DOWN },
  { "A", IN_A }, { "B", IN_B }, { "START", IN_START }, { "SELECT", IN_SELECT },
  { "TUNE", IN_TUNE }, { "POWER", IN_POWER }, { "MUTE", IN_MUTE },
};
static uint8_t keyFromName(const char* s) {
  for (const KeyName& k : KEYS) if (strcasecmp(s, k.name) == 0) return k.key;
  if (strncasecmp(s, "HEX", 3) == 0) s += 3;            // HEX0..HEXF (what the page sends; A/B alone are the buttons)
  if (s[0] && s[1] == 0) {                               // a bare digit 0-9, C-F is accepted too
    char c = s[0]; if (c >= 'a') c -= 32;
    if (c >= '0' && c <= '9') return (uint8_t)(IN_HEX0 + (c - '0'));
    if (c >= 'A' && c <= 'F') return (uint8_t)(IN_HEX0 + 10 + (c - 'A'));
  }
  return IN_NONE;
}

// ---- slots ----------------------------------------------------------------------
static int slotOf(int fd) { for (int i = 0; i < INPUT_PLAYERS; i++) if (s_slot[i].fd == fd) return i; return -1; }

static int slotAssign(int fd) {                       // httpd task
  int p = -1;
  portENTER_CRITICAL(&s_mux);
  for (int i = 0; i < INPUT_PLAYERS && p < 0; i++) if (s_slot[i].fd < 0) {
    s_slot[i] = { fd, 0, 0, false, millis(), 0 }; p = i;
  }
  if (p >= 0) s_countDirty = 1;
  portEXIT_CRITICAL(&s_mux);
  return p;
}

static void slotRelease(int fd) {                    // httpd task (close_fn)
  portENTER_CRITICAL(&s_mux);
  int p = slotOf(fd);
  if (p >= 0) {
    Slot& s = s_slot[p];
    for (int k = 1; k < IN_KEY_COUNT; k++) if (s.held & (1u << k)) stage((uint8_t)k, 0, (uint8_t)p);
    s.fd = -1; s.held = 0; s.axisDirty = false;
    s_countDirty = 1;
  }
  portEXIT_CRITICAL(&s_mux);
  if (p >= 0) Serial.printf("webctl: player %d left (fd %d)\n", p, fd);
}

int webctlClients() { int n = 0; for (int i = 0; i < INPUT_PLAYERS; i++) if (s_slot[i].fd >= 0) n++; return n; }
const char* webctlUrl() {
  static char buf[40];
  if (!s_srv) return "";
  snprintf(buf, sizeof buf, "http://%s/", hwNetAddress());
  return buf;
}

// ---- WebSocket ------------------------------------------------------------------
static esp_err_t wsSendText(httpd_req_t* req, const char* s) {
  httpd_ws_frame_t f = {}; f.type = HTTPD_WS_TYPE_TEXT; f.payload = (uint8_t*)s; f.len = strlen(s); f.final = true;
  return httpd_ws_send_frame(req, &f);
}
static void wsSendTextAsync(int fd, const char* s) {  // httpd task only (queued work)
  httpd_ws_frame_t f = {}; f.type = HTTPD_WS_TYPE_TEXT; f.payload = (uint8_t*)s; f.len = strlen(s); f.final = true;
  httpd_ws_send_frame_async(s_srv, fd, &f);
}
static void countWork(void*) {                        // runs in the httpd task
  char msg[8]; snprintf(msg, sizeof msg, "s %d", webctlClients());
  for (int i = 0; i < INPUT_PLAYERS; i++) if (s_slot[i].fd >= 0) wsSendTextAsync(s_slot[i].fd, msg);
}
static void chWork(void*) {                           // runs in the httpd task
  char msg[16]; snprintf(msg, sizeof msg, "ch %d", s_curStation);
  for (int i = 0; i < INPUT_PLAYERS; i++) if (s_slot[i].fd >= 0) wsSendTextAsync(s_slot[i].fd, msg);
}
static void audioWork(void*) {                         // runs in the httpd task
  char msg[24]; snprintf(msg, sizeof msg, "st mute=%d vol=%d", s_mute, s_vol);
  for (int i = 0; i < INPUT_PLAYERS; i++) if (s_slot[i].fd >= 0) wsSendTextAsync(s_slot[i].fd, msg);
}
static void netWork(void*) {                           // runs in the httpd task
  char msg[16]; snprintf(msg, sizeof msg, "st net=%s", s_lastNetMode == NET_AP ? "ap" : "home");
  for (int i = 0; i < INPUT_PLAYERS; i++) if (s_slot[i].fd >= 0) wsSendTextAsync(s_slot[i].fd, msg);
}

// "k <KEY> <0|1>" | "a <value>" | "c <code> <0|1>" | "v <0..100>" | "t <n>" | "t +" | "m" | "pw" | "tm <epoch> <offsetMin>" | "h"
static void wsParse(int p, char* line) {
  Slot& s = s_slot[p];
  s.lastSeenMs = millis();
  if (line[0] == 'h' && line[1] == 0) return;
  if (line[0] == 'm' && line[1] == 0) {                    // "m": mute toggle
    portENTER_CRITICAL(&s_mux); stage(IN_MUTE, 1, (uint8_t)p); portEXIT_CRITICAL(&s_mux);
    return;
  }
  if (line[0] == 'p' && line[1] == 'w' && line[2] == 0) {  // "pw": power toggle
    portENTER_CRITICAL(&s_mux); stage(IN_POWER, 1, (uint8_t)p); portEXIT_CRITICAL(&s_mux);
    return;
  }
  if (line[0] == 't' && line[1] == 'm' && (line[2] == ' ' || line[2] == 0)) {   // "tm <epoch> <offsetMin>"
    char* end = nullptr;
    long epoch = strtol(line + 2, &end, 10);
    long off = 0; bool haveOff = false;
    if (end) { while (*end == ' ') end++; if (*end) { off = strtol(end, nullptr, 10); haveOff = true; } }
    if (epoch > 1600000000L && haveOff && off >= -720 && off <= 840) {
      portENTER_CRITICAL(&s_mux);
      s_timeEpoch = (uint32_t)epoch; s_timeOffset = (int16_t)off; s_timeRequested = true;
      portEXIT_CRITICAL(&s_mux);
      Serial.printf("webctl: tm %ld %+ld (staged for the main loop)\n", epoch, off);
    } else {
      Serial.printf("webctl: tm rejected (epoch=%ld offset=%ld%s)\n", epoch, off, haveOff ? "" : " missing");
    }
    return;
  }
  if (line[0] == 't' && (line[1] == ' ' || line[1] == 0)) {   // "t <n>" or "t +"
    char* q = line + 1; while (*q == ' ') q++;
    int16_t v = -1;                                          // -1 = next station
    if (*q && *q != '+') v = (int16_t)strtol(q, nullptr, 10);
    portENTER_CRITICAL(&s_mux); stage(IN_TUNE, 1, (uint8_t)p, v); portEXIT_CRITICAL(&s_mux);
    return;
  }
  if (line[0] == 'v' && line[1] == ' ') {                  // "v <0..100>": volume
    long v = strtol(line + 2, nullptr, 10); if (v < 0) v = 0; if (v > 100) v = 100;
    portENTER_CRITICAL(&s_mux); stage(IN_VOLUME, 1, (uint8_t)p, (int16_t)v); portEXIT_CRITICAL(&s_mux);
    return;
  }
  if (line[0] == 'c' && line[1] == ' ') {                  // "c <code> <0|1>": typed key for future channels -> IN_CHAR, value = code
    char* end = nullptr; long code = strtol(line + 2, &end, 10);
    if (!end || code < 0 || code > 32767) return;
    uint8_t down = (*end == ' ' && end[1] == '1');
    portENTER_CRITICAL(&s_mux); stage(IN_CHAR, down, (uint8_t)p, (int16_t)code); portEXIT_CRITICAL(&s_mux);
    return;
  }
  if (line[0] == 'a' && line[1] == ' ') {
    long v = strtol(line + 2, nullptr, 10);
    if (v < -32768) v = -32768;
    if (v > 32767)  v = 32767;
    portENTER_CRITICAL(&s_mux); s.axis = (int16_t)v; s.axisDirty = true; portEXIT_CRITICAL(&s_mux);
    return;
  }
  if (line[0] == 'k' && line[1] == ' ') {
    char* name = line + 2; char* sp = strchr(name, ' ');
    if (!sp) return;
    *sp = 0;
    uint8_t key = keyFromName(name);
    uint8_t down = sp[1] == '1';
    if (key == IN_NONE) return;
    portENTER_CRITICAL(&s_mux);
    uint32_t bit = 1u << key;
    if (down) s.held |= bit; else s.held &= ~bit;
    stage(key, down, (uint8_t)p, key == IN_TUNE ? -1 : 0);   // TUNE button = next station, not station 0
    portEXIT_CRITICAL(&s_mux);
  }
}

static esp_err_t wsHandler(httpd_req_t* req) {
  int fd = httpd_req_to_sockfd(req);
  if (req->method == HTTP_GET) {                     // handshake just completed
    int p = slotAssign(fd);
    char msg[8]; snprintf(msg, sizeof msg, "p %d", p);
    wsSendText(req, msg);
    if (p < 0) { wsSendText(req, "full"); Serial.printf("webctl: fd %d refused, both slots taken\n", fd); httpd_sess_trigger_close(s_srv, fd); return ESP_OK; }
    char ch[16]; snprintf(ch, sizeof ch, "ch %d", s_curStation);
    wsSendText(req, ch);
    char st[16]; snprintf(st, sizeof st, "st net=%s", hwNetMode() == NET_AP ? "ap" : "home");
    wsSendText(req, st);
    char au[24]; snprintf(au, sizeof au, "st mute=%d vol=%d", s_mute, s_vol);
    wsSendText(req, au);
    Serial.printf("webctl: player %d joined (fd %d), heap %u\n", p, fd, (unsigned)ESP.getFreeHeap());
    return ESP_OK;                                   // "s n" goes out from webctlTick (s_countDirty)
  }
  httpd_ws_frame_t f = {}; f.type = HTTPD_WS_TYPE_TEXT;
  esp_err_t r = httpd_ws_recv_frame(req, &f, 0);     // length only
  if (r != ESP_OK) return r;
  char buf[32];
  if (f.len >= sizeof buf) return ESP_FAIL;          // nothing we send is this long: drop the socket
  if (f.len) { f.payload = (uint8_t*)buf; r = httpd_ws_recv_frame(req, &f, sizeof buf - 1); if (r != ESP_OK) return r; }
  buf[f.len] = 0;
  if (f.type != HTTPD_WS_TYPE_TEXT) return ESP_OK;
  int p = slotOf(fd);
  if (p >= 0) wsParse(p, buf);
  return ESP_OK;
}

static esp_err_t pageHandler(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, (const char*)WEBCTL_PAGE_GZ, WEBCTL_PAGE_GZ_LEN);
}

// /channels.json: {"current":<n>,"channels":[{"id":..,"title":..,"blurb":..,"flags":N,"panel":<raw JSON>}, ...]}
// `panel` is embedded verbatim (ChannelInfo::panel is already a JSON value);
// id/title/blurb are our own literals, never user input, so no escaping.
static esp_err_t channelsHandler(httpd_req_t* req) {
  static char buf[2048];
  int n = snprintf(buf, sizeof buf, "{\"current\":%d,\"channels\":[", s_curStation);
  for (int i = 0; i < s_chCount && n < (int)sizeof buf; i++) {
    const ChannelInfo& ci = s_chs[i]->info;
    n += snprintf(buf + n, sizeof buf - n, "%s{\"id\":\"%s\",\"title\":\"%s\",\"blurb\":\"%s\",\"flags\":%u,\"panel\":%s}",
                  i ? "," : "", ci.id, ci.title, ci.blurb, (unsigned)ci.flags, ci.panel);
  }
  n += snprintf(buf + n, sizeof buf - n, "]}");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, buf, n);
}

// ---- Wi-Fi setup (docs/NETWORK.md) -----------------------------------------------
// No ArduinoJson in this build: bodies are our own page's {"s":"..","p":".."},
// so a permissive scan for "key":"value" (with \" \\ unescaped) is enough.
static bool jsonStr(const char* body, const char* key, char* out, size_t outSize) {
  char pat[24]; snprintf(pat, sizeof pat, "\"%s\"", key);
  const char* p = strstr(body, pat);
  if (!p) { out[0] = 0; return false; }
  p = strchr(p + strlen(pat), ':');
  if (!p) { out[0] = 0; return false; }
  p++;
  while (*p == ' ') p++;
  if (*p != '"') { out[0] = 0; return false; }
  p++;
  size_t w = 0;
  while (*p && *p != '"' && w < outSize - 1) {
    if (*p == '\\' && p[1]) p++;
    out[w++] = *p++;
  }
  out[w] = 0;
  return true;
}
static bool readBody(httpd_req_t* req, char* buf, size_t bufSize) {
  int total = req->content_len;
  if (total <= 0 || total >= (int)bufSize) return false;
  int got = 0;
  while (got < total) {
    int r = httpd_req_recv(req, buf + got, total - got);
    if (r <= 0) return false;
    got += r;
  }
  buf[got] = 0;
  return true;
}
static esp_err_t sendJson(httpd_req_t* req, const char* status, const char* json) {
  if (status) httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}
static bool apModeOnly(httpd_req_t* req) {                        // true = ok to proceed
  if (hwNetMode() == NET_AP) return true;
  sendJson(req, "403 Forbidden", "{\"ok\":false,\"reason\":\"AP mode only\"}");
  return false;
}

// GET /api/wifi/scan -> {"aps":[{"s":<ssid>,"rssi":N,"secure":bool}, ...]} (short "s"
// key: see the note on POST /api/wifi) or {"aps":[],"scanning":true} while a
// just-started scan is still running. AP mode only: the page reads a 200 here as "show the
// setup screen" and a 403 as "this is the home network, show the remote"; it retries an
// empty list a few times, 2 s apart.
// Answers from the scan cache; see the comment in the handler.
// SSIDs are whatever the neighbours typed: escape them for JSON (the page never puts them in HTML).
static int jsonEscaped(char* dst, int cap, const char* src) {
  int n = 0;
  for (; *src && n < cap - 7; src++) {
    unsigned char c = *src;
    if (c == '"' || c == '\\') { dst[n++] = '\\'; dst[n++] = c; }
    else if (c < 0x20) n += snprintf(dst + n, cap - n, "\\u%04x", c);
    else dst[n++] = c;
  }
  dst[n] = 0; return n;
}

static esp_err_t wifiScanHandler(httpd_req_t* req) {
  if (!apModeOnly(req)) return ESP_OK;
  // Always answer from the cache (filled by the boot-time scan, before the AP came up).
  // Only an empty cache starts a background refresh: a refresh runs in AP_STA and can
  // make the phone blink off the set's network, so never on every panel open.
  NetApInfo aps[24];
  int n = hwNetScanResults(aps, 24);
  if (n == 0 && hwNetScanDone()) hwNetScanStart();
  static char buf[1536];
  int len = snprintf(buf, sizeof buf, "{\"aps\":[");
  for (int i = 0; i < n && len < (int)sizeof buf - 160; i++) {
    char esc[200]; jsonEscaped(esc, sizeof esc, aps[i].ssid);
    len += snprintf(buf + len, sizeof buf - len, "%s{\"s\":\"%s\",\"rssi\":%d,\"secure\":%s}",
                    i ? "," : "", esc, (int)aps[i].rssi, aps[i].secure ? "true" : "false");
  }
  len += snprintf(buf + len, sizeof buf - len, "]}");
  return sendJson(req, nullptr, buf);
}

// POST /api/wifi {"s":<ssid>,"p":<pass>} — AP mode only (short keys: every byte of
// tools/remote/page.html counts against build_page.py's budget). Blocks this httpd
// worker (not the main loop, not the display) for up to ~20 s while hw/net.h's join
// state machine, pumped by hwNetTick() from setLoop(), tries the network.
static esp_err_t wifiJoinHandler(httpd_req_t* req) {
  if (!apModeOnly(req)) return ESP_OK;
  char body[256];
  if (!readBody(req, body, sizeof body)) return sendJson(req, "400 Bad Request", "{\"ok\":false,\"reason\":\"bad request\"}");
  char ssid[33] = "", pass[65] = "";
  jsonStr(body, "s", ssid, sizeof ssid);
  jsonStr(body, "p", pass, sizeof pass);
  if (!ssid[0]) return sendJson(req, "400 Bad Request", "{\"ok\":false,\"reason\":\"missing ssid\"}");
  if (!hwNetJoinStart(ssid, pass)) return sendJson(req, "409 Conflict", "{\"ok\":false,\"reason\":\"busy\"}");
  while (hwNetJoinPending()) vTaskDelay(pdMS_TO_TICKS(150));
  char buf[160];
  if (hwNetJoinOk()) snprintf(buf, sizeof buf, "{\"ok\":true,\"address\":\"http://%s/\"}", hwNetAddress());
  else               snprintf(buf, sizeof buf, "{\"ok\":false,\"reason\":\"%s\"}", hwNetJoinReason());
  return sendJson(req, nullptr, buf);
}

// POST /api/wifi/forget — both modes. Only arms the on-screen confirmation
// (webctlForgetRequested(), polled by the platform); never erases by itself.
static esp_err_t wifiForgetHandler(httpd_req_t* req) {
  s_forgetRequested = true;
  Serial.println("webctl: /api/wifi/forget -> armed the on-screen confirmation");
  return sendJson(req, nullptr, "{\"ok\":true}");
}

// Captive-portal probes (iOS/Android/Windows): redirect to / so the page
// opens without the phone owner typing an address. AP mode only — a normal
// 404 in home mode is a normal 404.
static esp_err_t redirectHandler(httpd_req_t* req) {
  char loc[40]; snprintf(loc, sizeof loc, "http://%s/", hwNetAddress());
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", loc);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, "", 0);
}
static esp_err_t notFoundHandler(httpd_req_t* req, httpd_err_code_t) {
  if (hwNetMode() == NET_AP) return redirectHandler(req);
  return httpd_resp_send_404(req);
}

static void onClose(httpd_handle_t, int fd) { slotRelease(fd); close(fd); }

// ---- public ---------------------------------------------------------------------
bool webctlBegin() {
  if (s_srv) return true;
  for (int i = 0; i < INPUT_PLAYERS; i++) s_slot[i] = { -1, 0, 0, false, 0, 0 };
  s_sqHead = s_sqTail = 0;
  s_lastNetMode = hwNetMode(); s_netDirty = 0;   // nobody's connected yet to broadcast to; new sockets get "st" on join
  unsigned heap0 = ESP.getFreeHeap();

  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port      = 80;
  cfg.max_open_sockets = 4;          // 2 players + the page fetch + one to say "full"
  cfg.max_uri_handlers = 16;         // page, channels.json, ws, 3 wifi API, 4 captive probes
  cfg.max_resp_headers = 4;
  cfg.stack_size       = 6144;       // the /api/wifi handler blocks here for up to ~20 s
  cfg.lru_purge_enable = true;
  cfg.keep_alive_enable = true; cfg.keep_alive_idle = 5; cfg.keep_alive_interval = 3; cfg.keep_alive_count = 3;
  cfg.close_fn         = onClose;
  if (httpd_start(&s_srv, &cfg) != ESP_OK) { s_srv = nullptr; Serial.println("webctl: httpd_start failed"); return false; }

  httpd_uri_t page  = {}; page.uri  = "/";              page.method  = HTTP_GET;  page.handler  = pageHandler;
  httpd_uri_t chs   = {}; chs.uri   = "/channels.json"; chs.method   = HTTP_GET;  chs.handler   = channelsHandler;
  httpd_uri_t ws    = {}; ws.uri    = "/ws";            ws.method    = HTTP_GET;  ws.handler    = wsHandler; ws.is_websocket = true;
  httpd_uri_t scan  = {}; scan.uri  = "/api/wifi/scan";   scan.method = HTTP_GET;  scan.handler  = wifiScanHandler;
  httpd_uri_t join  = {}; join.uri  = "/api/wifi";        join.method = HTTP_POST; join.handler  = wifiJoinHandler;
  httpd_uri_t forget= {}; forget.uri= "/api/wifi/forget"; forget.method=HTTP_POST; forget.handler= wifiForgetHandler;
  httpd_register_uri_handler(s_srv, &page);
  httpd_register_uri_handler(s_srv, &chs);
  httpd_register_uri_handler(s_srv, &ws);
  httpd_register_uri_handler(s_srv, &scan);
  httpd_register_uri_handler(s_srv, &join);
  httpd_register_uri_handler(s_srv, &forget);
  static const char* CAPTIVE[] = { "/hotspot-detect.html", "/generate_204", "/ncsi.txt", "/connecttest.txt" };
  static httpd_uri_t captive[4];
  for (int i = 0; i < 4; i++) { captive[i] = {}; captive[i].uri = CAPTIVE[i]; captive[i].method = HTTP_GET; captive[i].handler = redirectHandler; httpd_register_uri_handler(s_srv, &captive[i]); }
  httpd_register_err_handler(s_srv, HTTPD_404_NOT_FOUND, notFoundHandler);

  Serial.printf("webctl: %s  page %u B gz, heap %u -> %u (%d B)\n", webctlUrl(), (unsigned)WEBCTL_PAGE_GZ_LEN,
                heap0, (unsigned)ESP.getFreeHeap(), (int)heap0 - (int)ESP.getFreeHeap());
  return true;
}

void webctlTick(uint32_t now) {
  if (!s_srv) return;
  { NetMode m = hwNetMode(); if (m != s_lastNetMode) { s_lastNetMode = m; s_netDirty = 1; } }
  // key edges, in order
  for (;;) {
    Staged e;
    portENTER_CRITICAL(&s_mux);
    bool have = s_sqHead != s_sqTail;
    if (have) { e = s_sq[s_sqHead]; s_sqHead = (s_sqHead + 1) % SQ; }
    portEXIT_CRITICAL(&s_mux);
    if (!have) break;
    InputEvent ev = { SRC_NET, e.key, e.player, e.down, e.value, now };
    if (!inputPush(ev)) Serial.println("webctl: input queue full");
  }
  if (s_dropped) { Serial.printf("webctl: %d staged events dropped\n", s_dropped); s_dropped = 0; }
  // knob: latest value, ≤ 30 Hz per player
  for (int p = 0; p < INPUT_PLAYERS; p++) {
    Slot& s = s_slot[p];
    if (!s.axisDirty || now - s.lastAxisMs < AXIS_MIN_MS) continue;
    int16_t v;
    portENTER_CRITICAL(&s_mux); v = s.axis; s.axisDirty = false; portEXIT_CRITICAL(&s_mux);
    s.lastAxisMs = now;
    InputEvent ev = { SRC_NET, IN_KNOB, (uint8_t)p, 1, v, now };
    inputPush(ev);
  }
  // a phone that vanished without a FIN
  for (int p = 0; p < INPUT_PLAYERS; p++) {
    Slot& s = s_slot[p];
    if (s.fd >= 0 && (int32_t)(now - s.lastSeenMs) > (int32_t)STALE_MS) /* signed: lastSeen can be a few ms ahead of the loop's now */ { Serial.printf("webctl: player %d silent %u ms, dropping\n", p, (unsigned)(now - s.lastSeenMs)); s.lastSeenMs = now; httpd_sess_trigger_close(s_srv, s.fd); }
  }
  if (s_countDirty) { s_countDirty = 0; httpd_queue_work(s_srv, countWork, nullptr); }
  if (s_chDirty)    { s_chDirty = 0;    httpd_queue_work(s_srv, chWork, nullptr); }
  if (s_netDirty)   { s_netDirty = 0;   httpd_queue_work(s_srv, netWork, nullptr); }
  if (s_audioDirty) { s_audioDirty = 0; httpd_queue_work(s_srv, audioWork, nullptr); }
}
