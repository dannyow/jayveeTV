// REMOTE card — how to pick up the remote: AP mode shows a Wi-Fi QR that
// joins the set's own network; home mode shows a QR of the controller
// page's URL. Either way, two lines of text under the QR and how many
// phones are on. White on black, like a caption card. Part of the platform,
// not a numbered station — the platform shows it on a long BOOT press or
// serial 'q' and returns to whatever station was on air. See docs/REMOTE.md
// and docs/NETWORK.md.
//
// remoteCardSetNet() is pushed by the platform whenever the network mode or
// address changes. The client count comes from webctlClients(); the host
// harness (which never links hw/webctl.cpp) sees a weak stub that says 0.
#pragma once
#include <stdint.h>

enum RemoteNet { REMOTE_NET_NONE, REMOTE_NET_AP, REMOTE_NET_HOME };

void remoteCardBegin();
void remoteCardTick(uint32_t nowMs);
void remoteCardRow(int y, uint16_t* dst);

// NONE: card says "NO NETWORK". AP: a = SSID, b = password -> QR
// "WIFI:T:WPA;S:<a>;P:<b>;;", both shown in text. HOME: a = "http://<ip>/",
// b = "jayveetv.local" -> QR of the URL, both shown in text. Copied.
void remoteCardSetNet(RemoteNet mode, const char* a, const char* b);

// A join attempt from the remote is in flight for this SSID: overlays
// "TUNING TO <ssid>" on the card. nullptr/"" clears it.
void remoteCardSetJoining(const char* ssid);

// BOOT held 5 s on the REMOTE card, arming the physical Wi-Fi forget: overlays
// "FORGET WI-FI? / PRESS BOOT", replacing the rest of the card until the
// platform clears it (confirmed, cancelled, or timed out).
void remoteCardSetForgetArmed(bool armed);
