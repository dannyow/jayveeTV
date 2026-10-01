# The remote

The phone is the remote. The television serves the page itself; there is nothing to install.
Scan the QR on the REMOTE card (hold BOOT), and the page opens.

## Two layers

- **The shell** belongs to the platform and is the same everywhere: now-playing strip,
  power, mute, tune, volume, the channel list (number, title, one-line blurb). On a phone
  screen there isn't room for the list and the panel at once, so the page shows one or the
  other below the control row. Tapping the now-playing strip toggles list ↔ panel, and a
  `ch` broadcast (successful tune) always returns to the panel.
- **The panel** belongs to the channel: whatever it declared in `info.panel`, built from
  the blocks below. Switching channels switches the panel.

## Panel blocks

A channel declares its panel as JSON; the page builds it. No channel ships HTML.

> Status: the page builds panels from `/channels.json`: `slider`, `dpad`, `buttons` and
> `tabs` are implemented; `keyboard` and `files` render a "not supported by this page yet"
> placeholder (no channel uses them yet). An empty `blocks` list (the test card) shows a
> tidy empty state, not a blank hole. A top-level `{"tabs":[...]}` panel (as in the second
> example below) is treated the same as `{"blocks":[{"tabs":[...]}]}`.

| block | sends | example |
|---|---|---|
| `slider` | `a <value>`: the finger's absolute position, −32768 (top) … 32767 (bottom); with `{"dir":"h"}` a horizontal track, −32768 (left) … 32767 (right) | Pong's paddle; `{"slider":{"dir":"h"}}` for a paddle that moves sideways |
| `dpad` | `k LEFT/RIGHT/UP/DOWN 1/0`; optional `"buttons"` (same form as the `buttons` block) put beside it | a Tetris; `{"dpad":{"buttons":[{"key":"A","label":"rotate"}]}}` |
| `buttons` | `k <KEY> 1/0` for each named key, with labels | `[{"key":"A","label":"rotate"}]` |
| `keyboard` | `c <code> 1/0`; `layout`: `zx48` (with K-mode legends) or `native` (the phone's own) | a computer emulator (planned) |
| `files` | lists `/files/<channel>`, uploads with `POST`, picks with `f <name>`, **planned**: the set does not serve `/files/*` yet | an emulator's tapes |
| `tabs` | switches between groups of blocks on the phone only | emulator: tape / joystick / keyboard |

Examples:

```json
{"blocks":[{"slider":{}},{"buttons":[{"key":"START","label":"start"}]}]}
```
```json
{"tabs":[
  {"label":"tape","blocks":[{"files":{"accept":".tap,.sna"}},{"buttons":[{"key":"SELECT","label":"reset"}]}]},
  {"label":"joystick","blocks":[{"dpad":{}},{"buttons":[{"key":"A","label":"fire"}]}]},
  {"label":"keyboard","blocks":[{"keyboard":{"layout":"zx48"}},{"buttons":[{"key":"B","label":"break"}]}]}
]}
```

## Protocol

HTTP: `GET /` the page · `GET /channels.json` the list with each channel's panel ·
`GET /files/<channel>` · `POST /files/<channel>/<name>` (planned, not served yet).

WebSocket `/ws`, one text message per frame. Every phone → set frame must stay under 32
bytes: `webctl.cpp`'s receive buffer is `char buf[32]` and drops the socket on anything
`>= 32`. Keep this in mind before adding a new message, not just when reading `tm`'s.

| phone → set | meaning |
|---|---|
| `k <KEY> <0\|1>` | key up/down (`LEFT RIGHT UP DOWN A B START SELECT HEX0..F`, also `TUNE`/`POWER`/`MUTE`; the page uses `t`/`pw`/`m` instead, see below) |
| `a <value>` | knob axis, rate-limited to 30 Hz |
| `c <code> <0\|1>` | typed character (ASCII; 1 = CAPS SHIFT, 2 = SYMBOL SHIFT, 8 = DELETE, 13 = ENTER, 27 = BREAK) |
| `t <station>` / `t +` | tune to a station / the next one |
| `v <0..100>` | volume |
| `m` · `pw` | mute toggle · power toggle |
| `f <name>` | pick a file for the current channel, **not implemented**: `/files/*` isn't served yet and no channel's panel uses a `files` block, so nothing sends this today |
| `tm <epochUtcSeconds> <utcOffsetMinutes>` | the phone's clock and timezone, sent once right after the socket opens (`tm 1790000000 120`); the set sets its RTC and keeps the offset for NTP |
| `h` | heartbeat, every 5 s |

| set → phone | meaning |
|---|---|
| `p <0\|1\|-1>` | your player number (−1: both slots taken) |
| `s <n>` | phones connected |
| `ch <station>` | now playing: the page swaps the panel |
| `st <fields>` | space-separated `key=value` pairs, sent on connect and whenever a field changes. `net=ap\|home` (docs/NETWORK.md), and `mute=0\|1 vol=<0..100>`: the set's own volume and mute, so every phone shows them. More fields may follow; a page ignores keys it does not know. |
| `full` | sent right after `p -1`, then the socket is closed: both player slots are taken |

## Look

Capsule: white case, black keys, one orange
accent for what acts. Channels may bring their own character inside a block (the ZX
keyboard is black and rubbery) but never restyle the shell.
