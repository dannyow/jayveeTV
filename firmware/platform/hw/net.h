// Wi-Fi: AP (the set is its own network) or STA joined to a saved home
// network — never both served at once. See docs/NETWORK.md for the spec;
// this header is "the firmware sketch" section, kept under hw/ (like the
// rest of the hardware layer) and using the hwNet* prefix the other hw/*.cpp
// files use, rather than NETWORK.md's bare "net*" sketch names.
//
// Credentials: NVS (Preferences, namespace "jvtv-net"), saved with
// hwNetSave(). Empty NVS falls back to firmware/secrets.h (gitignored,
// optional, dev bench only): WIFI_SSID / WIFI_PASS. Neither present -> AP
// from first boot, no typing required.
#pragma once
#include <stdint.h>

enum NetMode : uint8_t { NET_AP = 0, NET_HOME = 1 };

bool        hwNetBegin();                 // boot: saved/dev creds -> STA, 30 s deadline, else AP; nothing saved -> AP
void        hwNetTick(uint32_t nowMs);    // pump the boot-connect deadline, the join deadline, the captive DNS server
bool        hwNetUp();                    // AP: true once begun. HOME: has an IP.
const char* hwNetStatus();                // short text for the status line / fps log
NetMode     hwNetMode();
const char* hwNetAddress();               // "192.168.4.1" (AP) / STA IP (HOME) / "" before hwNetUp()
const char* hwNetApSsid();                // "JayVeeTV-XXXX", XXXX = last 4 hex of the MAC
const char* hwNetApPass();                // the stored 8-char AP password ([a-z2-9], generated once)

// ---- NVS credentials --------------------------------------------------------
bool hwNetHasSaved();                              // NVS has a saved ssid
void hwNetSave(const char* ssid, const char* pass); // -> NVS; does not itself switch mode
void hwNetForget();                                 // erases the saved ssid/pass only (not the AP password); caller restarts

// ---- scan (AP mode only; async) --------------------------------------------
struct NetApInfo { char ssid[33]; int8_t rssi; bool secure; };
bool hwNetScanStart();                              // begin; false if a scan is already running
bool hwNetScanDone();                               // results ready (also true if none ever started)
int  hwNetScanResults(NetApInfo* out, int maxN);    // 2.4 GHz only; returns the count written

// ---- joining a candidate network from the remote ---------------------------
// Runs the STA radio alongside the AP for up to 20 s so the phone that asked
// for this doesn't get dropped mid-request. Pumped from hwNetTick(), so a
// caller on another task (the httpd handler) polls hwNetJoinPending() rather
// than blocking inside net.cpp itself. On success the set has already saved
// the credentials and switched to HOME by the time hwNetJoinPending() goes
// false; on failure it's back to plain AP, nothing saved.
bool        hwNetJoinStart(const char* ssid, const char* pass);  // false if a scan or another join is already running
bool        hwNetJoinPending();
bool        hwNetJoinOk();                          // valid once !hwNetJoinPending()
const char* hwNetJoinReason();                      // "" | "wrong password" | "not found" | "timeout"
const char* hwNetJoinSsid();                        // the candidate; valid while pending, for the TUNING overlay
