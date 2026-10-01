# Architecture

JayVeeTV is two things: a **platform** that behaves like a 1970s television, and
**channels** that are the programmes it shows. The platform owns the hardware, the
analogue behaviour and the remote. A channel owns only its picture, its sound events and
the shape of its control panel on the phone.

```
            ┌──────────────────── platform ────────────────────┐
 knock ───▶ │ IMU ─┐                                            │
 lift  ───▶ │      ├─▶ set state ─▶ tuner ─▶ glitch ─▶ panel    │──▶ AMOLED 480×480
 keys  ───▶ │ keys ┤   (on/off,     (lock,    (roll,   (QSPI,   │
 phone ───▶ │ web ─┘    station)     drift)    snow)    stripes) │
            │                 ▲                   ▲             │
            │   input queue   │ channel.tick()    │ channel.row()
            │   sound bed ◀── channel.sound()     │             │──▶ speaker
            │   arena (64 KB, one owner at a time)│             │
            └─────────────────┼───────────────────┼─────────────┘
                              │   channel (content)
                         test card · pong · your channel …
```

## Platform

| part | what it does | channels see it as |
|---|---|---|
| **set state** | power (PWR key, face down/up), current station, the boot sequence, off-air hours | `start()` when tuned in, `stop()` when tuned away |
| **tuner** | locked / losing / lost / tuning; detune drifts; a knock kicks it; a knock's strength jolts the picture | nothing; it happens to them |
| **CRT model** | warm-up bloom, power-off collapse to a dot, afterglow | nothing |
| **glitch layer** | roll, bend, jitter, snow, hum bar, scanlines, chroma drain, applied at blit time to whatever the channel paints | `gChroma` (0..16) to honour when building palettes |
| **display** | 480×480 SH8601 over QSPI, streamed in 8-row stripes inside one transaction (the panel blanks if the bus idles) | nothing; never touch it |
| **sound bed** | ES8311, one synth task at 32 kHz: test tone, hiss, hum, line whistle, power thunk, knock thud, master volume | `sound()` asks for a tone or supplies PCM |
| **input** | one queue for keys, touch, serial and phones; held state and axes | `inputPoll()`, `inputDown()`, `inputAxis()` in `tick()` |
| **knock** | 250 Hz IMU task: jerk between samples → knock + strength; hardware tap as backup | nothing (the tuner reacts) |
| **arena** | one 64 KB static block for buffers that never coexist (a stream's rings, an emulator's RAM) | `arenaClaim()` in `start()`, `arenaRelease()` in `stop()` |
| **network** | AP on first boot (or when nothing's saved), home Wi-Fi saved from the remote (NVS, dev fallback: an optional gitignored `firmware/secrets.h`), physical and remote-armed forget; see [NETWORK.md](NETWORK.md) | nothing |
| **remote** | serves the phone page, `/channels.json`, the WebSocket; turns phone input into queue events; handles tune / power / mute / volume itself | the panel it declared |

Everything above is the television. It is the same for every channel and a channel
cannot change it. That is exactly why every channel looks like it is on a real set.

## Channels

A channel is a `Channel` struct in `firmware/channels/<id>/`, registered by one line in
`firmware/channels/channels.cpp`. The order of that list is the station numbering. The
full contract, the rules and the template are in [CHANNELS.md](CHANNELS.md).

## Timing budget (measured on the board)

| | cost |
|---|---|
| one full frame through the glitch layer + QSPI push | ~40 ms (≈25 fps) |
| QSPI push alone | ~12 ms |
| a channel's 240 `row()` calls | must stay under **8 ms** |
| a channel's `tick()` | must stay under **5 ms**, or run its work in its own task (an emulator would) |
| Wi-Fi + web server | ~45 KB of heap, no measurable fps cost |
| free heap floor | **90 KB** with Wi-Fi and the web server up |

## Build profiles

One source tree, several PlatformIO environments (`firmware/platformio.ini`). A profile
chooses which channels `channels/channels.cpp` registers and the boot station, e.g.
`release1` (test card + Pong, the default), `full` (the same set today; where later
releases grow into as more channels land). A profile that boots straight into one
channel, or a Wi-Fi access-point mode, is a later addition, not built yet.

## Hard-won rules

- The Arduino builder appends `-Os -fno-jump-tables` **after** our flags: a hot
  interpreter needs its own `#pragma GCC optimize`.
- Compare timestamps signed: `(int32_t)(now - stamp) > limit`. An unsigned compare with a
  stamp taken later in the same pass fires with a 4-billion-ms "timeout".
- `delay()` counts RTOS ticks, `millis()` runs on esp_timer; end a precise wait with a
  spin on `millis()`.
- `esp_partition_mmap` of more than ~2 MB at once fails on the C6 (MMU pages); map per file.
- A backslash at the end of a `//` comment continues the comment onto the next line.
