// See net.h. State machine, one mode served at a time:
//   boot -> (saved NVS creds, else dev secrets.h) -> STA, 30 s deadline
//              \-> connected -> HOME (mDNS up)
//              \-> timeout   -> AP (saved creds kept for next boot)
//           -> nothing saved -> AP directly
//   AP:  WiFi.softAP(ssid, pass) + DNSServer on :53 answering "*" with the AP
//        IP (captive portal). webctl.cpp adds the HTTP redirect handlers.
//   join (from the remote, AP mode only): WIFI_AP_STA so the caller's socket
//        survives the trial, up to 20 s; success saves + switches to HOME +
//        drops the AP; failure reverts to plain AP, nothing saved.
#include "platform/hw/net.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <esp_wifi.h>
#if __has_include("../../secrets.h")
  #include "../../secrets.h"
  #define NET_HAVE_DEV_CREDS 1
#else
  #define NET_HAVE_DEV_CREDS 0
#endif

static const char* PREF_NS = "jvtv-net";
static const uint32_t BOOT_TIMEOUT_MS = 30000;
static const uint32_t JOIN_TIMEOUT_MS = 20000;

static NetMode  s_mode = NET_AP;
static bool     s_resolved = false;          // AP or HOME decided (vs. still trying the boot connect)
static char     s_apSsid[20] = "";
static char     s_apPass[9]  = "";
static char     s_status[48] = "off";
static char     s_address[16] = "";
static DNSServer s_dns;

static bool     s_bootConnecting = false;
static uint32_t s_bootT0 = 0;

static bool     s_joinPending = false, s_joinOk = false;
static char     s_joinReason[24] = "";
static char     s_joinSsid[33] = "", s_joinPass[65] = "";
static uint32_t s_joinT0 = 0;

static bool     s_scanRunning = false;

// ---- helpers ----------------------------------------------------------------
static void genApPass(char* out) {
  static const char CS[] = "abcdefghijklmnopqrstuvwxyz23456789";   // [a-z2-9], NETWORK.md
  for (int i = 0; i < 8; i++) out[i] = CS[esp_random() % (sizeof(CS) - 1)];
  out[8] = 0;
}

static void loadOrMakeApCreds() {
  uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);   // from eFuse: WiFi.macAddress() reads 00:..:00 before the driver starts
  snprintf(s_apSsid, sizeof s_apSsid, "JayVeeTV-%02X%02X", mac[4], mac[5]);

  Preferences p; p.begin(PREF_NS, false);
  String pass = p.isKey("appass") ? p.getString("appass", "") : String();
  if (pass.length() != 8) {
    char gen[9]; genApPass(gen);
    p.putString("appass", gen);
    strncpy(s_apPass, gen, sizeof s_apPass - 1);
  } else {
    strncpy(s_apPass, pass.c_str(), sizeof s_apPass - 1);
  }
  s_apPass[sizeof s_apPass - 1] = 0;
  p.end();
}

// Scan results are cached: a pure-AP radio cannot scan (the STA interface does the
// scanning), and the driver's list is freed once read. So: one scan before the AP comes
// up (nobody is connected yet; async, so the set is already on air in snow meanwhile),
// and refreshes run in AP_STA and land here.
static NetApInfo s_scanCache[20];
static int s_scanCacheN = 0;
static void scanToCache(int16_t n) {
  s_scanCacheN = 0;
  for (int16_t i = 0; i < n && s_scanCacheN < 20; i++) {
    if (WiFi.channel(i) > 14 || WiFi.SSID(i).length() == 0) continue;   // 2.4 GHz only, no hidden
    NetApInfo& o = s_scanCache[s_scanCacheN++];
    strncpy(o.ssid, WiFi.SSID(i).c_str(), sizeof o.ssid - 1); o.ssid[sizeof o.ssid - 1] = 0;
    o.rssi = (int8_t)WiFi.RSSI(i); o.secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  Serial.printf("net: scan cached %d networks\n", s_scanCacheN);
}

static bool s_apScanFirst = false;          // startAp() began the pre-AP scan; hwNetTick() brings the AP up when it ends
static void apUp() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(s_apSsid, s_apPass);
  IPAddress ip = WiFi.softAPIP();
  snprintf(s_address, sizeof s_address, "%s", ip.toString().c_str());
  s_dns.start(53, "*", ip);                  // captive portal: every name resolves to us
  s_mode = NET_AP;
  s_resolved = true;
  snprintf(s_status, sizeof s_status, "AP %s", s_apSsid);
  Serial.printf("net: AP %s pass %s at %s\n", s_apSsid, s_apPass, s_address);
}
static void startAp() {
  s_dns.stop();
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_STA);
  WiFi.scanNetworks(true, false, false, 300);  // ~2-3 s in the background, before any phone is here
  s_apScanFirst = true;
}

static uint32_t s_apDropAt = 0;              // after a join from the phone: when the AP goes (0 = not pending)
static void enterHome(bool keepApBriefly = false) {
  s_dns.stop();
  // Drop the AP sub-radio if a join left it in AP_STA. After a join asked from the phone,
  // keep it 4 s so the phone gets the "joined" answer over the network it is still on.
  if (keepApBriefly) s_apDropAt = millis() + 4000; else WiFi.mode(WIFI_STA);
  s_mode = NET_HOME;
  s_resolved = true;
  snprintf(s_address, sizeof s_address, "%s", WiFi.localIP().toString().c_str());
  snprintf(s_status, sizeof s_status, "%s rssi %d", s_address, (int)WiFi.RSSI());
  MDNS.end();
  MDNS.begin("jayveetv");
  MDNS.addService("http", "tcp", 80);
  Serial.printf("net: home, %s, jayveetv.local, rssi %d, heap %u\n", s_address, (int)WiFi.RSSI(), (unsigned)ESP.getFreeHeap());
}

// ---- boot ---------------------------------------------------------------------
bool hwNetBegin() {
  loadOrMakeApCreds();

  Preferences p; p.begin(PREF_NS, true);
  String ssid = p.isKey("ssid") ? p.getString("ssid", "") : String();
  String pass = p.isKey("pass") ? p.getString("pass", "") : String();
  p.end();

  const char* useSsid = nullptr; const char* usePass = nullptr;
  if (ssid.length()) { useSsid = ssid.c_str(); usePass = pass.c_str(); }
#if NET_HAVE_DEV_CREDS
  else { useSsid = WIFI_SSID; usePass = WIFI_PASS; Serial.println("net: no saved network, using secrets.h (dev fallback)"); }
#endif

  if (useSsid) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);                    // no modem sleep: worst-case CPU/RAM picture first
    WiFi.begin(useSsid, usePass);
    s_bootConnecting = true; s_bootT0 = millis();
    snprintf(s_status, sizeof s_status, "connecting %s", useSsid);
    Serial.printf("net: connecting to %s (30 s deadline)\n", useSsid);
    // useSsid/usePass point at String::c_str() buffers that are about to go
    // out of scope; WiFi.begin() has already copied them into the driver.
    return true;
  }

  Serial.println("net: nothing saved, no secrets.h -> AP");
  startAp();
  return true;
}

void hwNetTick(uint32_t now) {
  if (s_bootConnecting) {
    if (WiFi.status() == WL_CONNECTED) { s_bootConnecting = false; enterHome(); }
    else if ((int32_t)(now - s_bootT0) > (int32_t)BOOT_TIMEOUT_MS) {
      s_bootConnecting = false;
      Serial.println("net: saved network not found in 30 s, AP for this session (kept for next boot)");
      startAp();
    }
    return;                                   // nothing else to pump until resolved
  }
  if (s_apScanFirst) {
    int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return;
    s_apScanFirst = false;
    if (n >= 0) scanToCache(n); else Serial.printf("net: scan failed (%d)\n", n);
    apUp();
    return;
  }

  if (s_apDropAt && (int32_t)(now - s_apDropAt) >= 0) { s_apDropAt = 0; WiFi.mode(WIFI_STA); Serial.println("net: AP off"); }

  if (s_joinPending) {
    wl_status_t st = WiFi.status();
    if (st == WL_CONNECTED) {
      hwNetSave(s_joinSsid, s_joinPass);
      enterHome(true);
      s_joinPending = false; s_joinOk = true; s_joinReason[0] = 0;
    } else if (((st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) && (int32_t)(now - s_joinT0) > 1500) ||   // right after begin() the status can still be the LAST attempt's failure; signed: the stamp is set on the httpd task, often after this loop's `now`
               (int32_t)(now - s_joinT0) > (int32_t)JOIN_TIMEOUT_MS) {
      const char* why = st == WL_NO_SSID_AVAIL ? "not found" : st == WL_CONNECT_FAILED ? "wrong password" : "timeout";
      strncpy(s_joinReason, why, sizeof s_joinReason - 1); s_joinReason[sizeof s_joinReason - 1] = 0;
      s_joinPending = false; s_joinOk = false;
      WiFi.disconnect(true, false);
      WiFi.mode(WIFI_AP);                     // back to plain AP; softAP config/DNS untouched, never stopped
      snprintf(s_status, sizeof s_status, "AP %s", s_apSsid);
      Serial.printf("net: join %s failed: %s\n", s_joinSsid, why);
    }
  }

  if (s_scanRunning) { int16_t n = WiFi.scanComplete(); if (n != WIFI_SCAN_RUNNING) { s_scanRunning = false; if (n >= 0) scanToCache(n); else Serial.printf("net: scan failed (%d)\n", n); if (s_mode == NET_AP && !s_joinPending) WiFi.mode(WIFI_AP); } }

  if (s_mode == NET_AP) s_dns.processNextRequest();
  else {
    bool up = WiFi.status() == WL_CONNECTED;
    static bool wasUp = true;
    if (!up && wasUp) { snprintf(s_status, sizeof s_status, "lost"); Serial.println("net: connection lost, reconnecting"); WiFi.reconnect(); }
    else if (up) snprintf(s_status, sizeof s_status, "%s rssi %d", s_address, (int)WiFi.RSSI());
    wasUp = up;
  }
}

bool        hwNetUp()      { return s_resolved && (s_mode == NET_AP || WiFi.status() == WL_CONNECTED); }
const char* hwNetStatus()  { return s_status; }
NetMode     hwNetMode()    { return s_mode; }
const char* hwNetAddress() { return s_address; }
const char* hwNetApSsid()  { return s_apSsid; }
const char* hwNetApPass()  { return s_apPass; }

// ---- NVS credentials ----------------------------------------------------------
bool hwNetHasSaved() {
  Preferences p; p.begin(PREF_NS, true);
  bool has = p.isKey("ssid") && p.getString("ssid", "").length() > 0;
  p.end();
  return has;
}
void hwNetSave(const char* ssid, const char* pass) {
  Preferences p; p.begin(PREF_NS, false);
  p.putString("ssid", ssid); p.putString("pass", pass);
  p.end();
  Serial.printf("net: saved credentials for %s\n", ssid);
}
void hwNetForget() {
  Preferences p; p.begin(PREF_NS, false);
  p.remove("ssid"); p.remove("pass");        // the AP password (appass) stays: it's on the set's label, not a secret worth rotating
  p.end();
  Serial.println("net: forgot saved credentials");
}

// ---- scan -----------------------------------------------------------------------
bool hwNetScanStart() {
  if (s_scanRunning || s_joinPending) return false;
  if (s_mode == NET_AP) WiFi.mode(WIFI_AP_STA);   // the AP stays up; the phone may blink for a moment
  WiFi.scanNetworks(true /*async*/, false /*hidden*/, false /*passive*/, 300 /*ms/chan*/);
  s_scanRunning = true;
  return true;
}
bool hwNetScanDone() { return !s_scanRunning; }
int hwNetScanResults(NetApInfo* out, int maxN) {
  int n = s_scanCacheN < maxN ? s_scanCacheN : maxN;
  for (int k = 0; k < n; k++) out[k] = s_scanCache[k];
  return n;
}

// ---- join from the remote --------------------------------------------------------
bool hwNetJoinStart(const char* ssid, const char* pass) {
  if (s_joinPending || s_scanRunning) return false;
  strncpy(s_joinSsid, ssid, sizeof s_joinSsid - 1); s_joinSsid[sizeof s_joinSsid - 1] = 0;
  strncpy(s_joinPass, pass, sizeof s_joinPass - 1); s_joinPass[sizeof s_joinPass - 1] = 0;
  WiFi.mode(WIFI_AP_STA);                     // AP stays up: the caller's own phone must not drop mid-request
  s_joinT0 = millis();                        // stamp before pending: the main loop may tick in between
  WiFi.begin(s_joinSsid, s_joinPass);
  s_joinPending = true;
  Serial.printf("net: trying %s (20 s)\n", s_joinSsid);
  return true;
}
bool        hwNetJoinPending() { return s_joinPending; }
bool        hwNetJoinOk()      { return s_joinOk; }
const char* hwNetJoinReason()  { return s_joinReason; }
const char* hwNetJoinSsid()    { return s_joinSsid; }
