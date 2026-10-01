# Network: first boot, setup, and forgetting

The set works with no configuration at all, and learns your home Wi-Fi only if you want
it to. Nothing leaves your network; there is no cloud.

## Modes

| mode | when | the phone reaches the set at | cost |
|---|---|---|---|
| **AP**: the set is its own Wi-Fi | first boot, no saved network, or the saved one not found | `http://192.168.4.1/` (captive page opens by itself) | ~35 KB heap |
| **Home**: the set joins your Wi-Fi | a network was saved | `http://jayveetv.local/` or the IP on the REMOTE card | ~35 KB heap |

Only one mode at a time; running both costs RAM the channels need.

## First boot

1. The set starts in AP mode as `JayVeeTV-XXXX` (last 4 hex digits of the MAC), WPA2,
   with a random 8-character password generated on first boot and kept in NVS.
2. Hold **BOOT**: the REMOTE card shows a **Wi-Fi QR** (`WIFI:T:WPA;S:JayVeeTV-XXXX;P:…;;`),
   the network name and the password in text.
3. Scan it: the phone joins the set's network. A tiny DNS server answers every name
   with the set's address, so the phone's captive-portal check opens the page by itself.
   No typing an address.
4. On the set's own network the page opens on the **Wi-Fi setup screen** (below). A link
   under it, "Use as a remote without Wi-Fi", gives the full remote instead: Pong works
   anywhere, with no router.

## Joining your home Wi-Fi (optional)

1. On the setup screen the set lists the networks it can see (a scan, 2.4 GHz only, or
   "Other network…" to type a hidden one); you pick one and type the password. The REMOTE
   card comes up on the TV by itself when the join starts.
2. The set saves it to NVS and **tries it right away** for up to 20 s while the screen
   shows "TUNING TO <SSID>…" (a proper tuning animation, it is a TV after all).
   - Joined: the screen shows the new address and `jayveetv.local`, the REMOTE card's QR
     becomes the URL, and the set restarts its web server on the home network. The phone
     has to switch back to the home Wi-Fi; the page says so (the set keeps its own network
     4 s after joining so that answer reaches the phone).
   - Failed: back to AP mode, the screen shows why (wrong password / not found), nothing
     saved.
3. Next boots go straight to the home network. If it can't be found within 30 s (the
   set was carried to a friend's house), it falls back to AP mode for this session and
   keeps the saved network.

## Forgetting the network (the physical reset)

On purpose, and hard to do by accident:

1. Hold **BOOT** → the REMOTE card.
2. On the REMOTE card, **hold BOOT for 5 s** → the screen asks `FORGET WI-FI? PRESS BOOT`.
3. **Press BOOT once more** within 5 s → the saved network is erased, the set restarts in
   AP mode. Anything else, or waiting, cancels.

Also available from the remote (tap the channel strip → Wi-Fi → Forget this Wi-Fi), which asks for the same
confirmation on the TV screen. A phone on the network alone cannot wipe the set.

## Time

The PCF85063 RTC (`hw/rtc`) keeps LOCAL time, not UTC, and has no idea what timezone it's
in; nothing on the board does, until a phone says so.

- **Phone**: right after the page's WebSocket opens it sends `tm <epochUtc> <offsetMin>`
  (docs/REMOTE.md), `offsetMin` = `-new Date().getTimezoneOffset()`: minutes east of UTC,
  e.g. `120` for CEST. The set sets the RTC and remembers the offset in NVS.
- **NTP**: once the set has joined a home network it syncs the instant (UTC) once via SNTP
  (`pool.ntp.org`, non-blocking), then sets the RTC using whichever offset a phone last
  supplied. If no phone has ever visited, NTP leaves the RTC alone rather than guess a
  timezone; the log says so. Re-syncs every few hours.
- **Serial** (bench only): `T <epochUtc> [offsetMin]`; offset defaults to whatever's
  stored in NVS, or 120 (CEST) if nothing ever was.
- Whichever set the clock last, phone or NTP, wins; they don't fight. The phone is
  always the authority for *which* timezone; NTP is always the authority for *the instant*.

A stranger's freshly flashed board with no serial cable gets a working clock the moment
someone opens the remote page on their phone, before it's ever seen the owner's Wi-Fi.

## Security notes

- The AP password is per device and shown only on the set's own screen.
- The setup pages (scan, save, forget) exist only in AP mode or after the on-screen
  confirmation; in home mode the remote cannot read or change the saved password.
- Credentials sit in NVS unencrypted (fine for a desk toy, and said so in the README).
- No authentication on the remote itself: anyone on the same network can change the
  channel. That is the point of a TV remote; keep it on a trusted network.

## How it is built

- `platform/hw/net`: `hwNetBegin()` reads NVS and starts the AP (`WiFi.softAP` + a DNS
  server answering every name) or joins the saved network with a 30 s deadline, falling
  back to the AP. `hwNetSave()`, `hwNetForget()`, `hwNetScanStart()`, `hwNetMode()`,
  `hwNetAddress()`. A join asked from the phone runs AP and station together for up to
  20 s, so the phone that asked is not dropped mid-request. The web server keeps running
  across the switch; only the address it hands out changes.
- `platform/remotecard`: the QR is a `WIFI:` join code in AP mode and the URL at home, with
  two overlays: a join in flight ("TUNING TO `<ssid>`") and the forget confirmation
  ("FORGET WI-FI? PRESS BOOT").
- Remote: `GET /api/wifi/scan`, `POST /api/wifi {"s":<ssid>,"p":<password>}` (AP mode
  only; short keys keep the page small), `POST /api/wifi/forget` → on-screen confirmation.
  The WebSocket `st` message carries `net=ap|home`.
- mDNS: `jayveetv.local` with `_http._tcp`, home mode only.
- Development: a gitignored `firmware/secrets.h` (`WIFI_SSID`, `WIFI_PASS`) is used only
  when NVS holds no network, and is never written to NVS.
