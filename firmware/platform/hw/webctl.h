// Web controller — a phone as the remote / game pad, over Wi-Fi. One
// esp_http_server on port 80. Protocol is docs/REMOTE.md; summary:
//   GET /              the controller page (gzipped in flash, tools/remote/page.html)
//   GET /channels.json the station list (id, title, blurb, flags, panel) + current station
//   GET /ws            WebSocket, text frames, one message per frame:
//     client -> server   k <KEY> <0|1>     KEY = LEFT RIGHT UP DOWN A B START SELECT
//                                          (a bare 0-9/C-F digit is a HEX key too)
//                        a <value>         knob axis, -32768..32767 (top = -32768)
//                        c <code> <0|1>    typed key (ASCII), for future channels
//                        v <0..100>        volume
//                        t <n>  / t +      tune to station n / next station
//                        m                 mute toggle
//                        pw                power toggle
//                        tm <epoch> <off>  the phone's clock: epoch = UTC seconds, off =
//                                          -new Date().getTimezoneOffset() (minutes east of
//                                          UTC). Sent right after the socket opens. Staged
//                                          here (validated: epoch > 1600000000, off in
//                                          -720..840); webctlTimeRequested() hands it to the
//                                          main loop, which owns the RTC (no I2C from this
//                                          task) — see docs/NETWORK.md "Time".
//                        h                 heartbeat (every 5 s; silent 20 s = dropped)
//     server -> client   p <player>        on connect: 0, 1, or -1 (both slots taken)
//                        full              with p -1, then the socket is closed
//                        s <n>             live player count, whenever it changes
//                        ch <station>      current station: on connect, and on every change
//                        st net=ap|home    network mode: on connect, and on every change
// The first live socket is player 0, the second player 1. A closed socket frees
// its slot and pushes key-up for everything it still held.
//
// Wi-Fi setup (docs/NETWORK.md), AP mode only unless noted. JSON keys are single
// letters (s = ssid, p = pass) because tools/remote/page.html is size-budgeted by
// build_page.py and every byte of the client's own request bodies counts too:
//   GET  /api/wifi/scan   {"aps":[{"s":..,"rssi":N,"secure":bool}, ...]} (async; may come
//                         back empty while a scan is still in flight — reopening asks again)
//   POST /api/wifi        {"s":..,"p":..} — blocks the request up to 20 s while the set
//                         tries it (screen shows "TUNING TO <ssid>"); replies
//                         {"ok":true,"address":".."} or {"ok":false,"reason":".."}
//   POST /api/wifi/forget both modes — never wipes by itself: only arms the same on-screen
//                         confirmation the physical BOOT-hold does; replies {"ok":true}
//   /hotspot-detect.html, /generate_204, /ncsi.txt, /connecttest.txt, and any other unmatched
//   GET (AP mode only) redirect to / — the captive-portal opens the page without typing
//
// Threading: the server runs in its own FreeRTOS task. It never touches
// input.h (a plain ring, single producer); events are staged here and
// webctlTick() — from the platform's loop — pushes them with inputPush(SRC_NET, …),
// knob updates coalesced to ≤ 30 Hz per player. The /api/wifi POST handler blocks its
// own httpd worker task while it polls hw/net.h's join state machine, which is pumped
// by hwNetTick() from the main loop — never from here.
#pragma once
#include <stdint.h>
#include "platform/channel.h"

bool        webctlBegin();               // once, after hwNetUp(); false = server failed to start
void        webctlTick(uint32_t nowMs);  // every loop: drain staged events into input.h, drop stale clients, broadcast "st"
int         webctlClients();             // live player sockets (0..2)
const char* webctlUrl();                 // "http://<ip>/" (AP or home, whichever is current), "" before begun

// The station list /channels.json serves and the "ch <n>" broadcasts announce.
// Call webctlSetChannels() once at boot; webctlSetStation() whenever the
// platform actually changes station (tuneTo(), not while showing the REMOTE card).
void webctlSetChannels(const Channel* const* chs, int n);
void webctlSetStation(int n);
// The set's volume (0..100) and mute, shown on every phone ("st mute=0|1 vol=N").
void webctlSetAudio(int volume, bool muted);

// POST /api/wifi/forget only arms the platform's on-screen confirmation (docs/NETWORK.md);
// the platform polls this once per loop and clears it. A phone alone can never wipe.
bool webctlForgetRequested();

// A validated "tm <epoch> <offsetMin>" staged by the httpd task; true at most once per
// message, *epochUtc/*offsetMin valid only when it returns true. The platform (which owns
// the I2C bus) sets the RTC and persists the offset — see docs/NETWORK.md "Time".
bool webctlTimeRequested(uint32_t* epochUtc, int* offsetMin);
