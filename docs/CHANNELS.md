# Writing a channel

A channel is a small, self-contained programme: it paints rows of pixels, reacts to input
and asks for sounds. The platform does everything else, including making it look and
sound like it is on a 1975 television. You should be able to write one without reading
any platform code. If you have to, this document is wrong; please fix it.

## The contract

```cpp
// firmware/platform/channel.h
struct ChannelInfo {
  const char* id;        // "pong": folder name, URL, CLI name
  const char* title;     // "Pong"
  const char* blurb;     // one line for the channel list: "Your phone is the paddle."
  uint8_t     flags;     // CH_GAME | CH_WIDE | CH_ENDLESS | CH_MISBEHAVE
  const char* panel;     // JSON: which blocks the phone shows (see REMOTE.md)
};

struct Channel {
  ChannelInfo info;
  void     (*begin)();                         // once at boot: build tables, no hardware
  void     (*start)(uint32_t nowMs);           // the set locked onto you: reset, claim buffers
  void     (*stop)();                          // tuned away / powered off: release buffers
  void     (*tick)(uint32_t nowMs);            // once per frame, before painting: input + state
  void     (*row)(int y, uint16_t* dst);       // paint logical row y (0..239); see below
  void     (*sound)(uint32_t nowMs, Sound* s); // optional (nullptr): tones or PCM for this frame
  uint32_t (*length)();                        // optional: programme length in ms; 0 = endless
};
```

| flag | meaning |
|---|---|
| `CH_GAME` | the set's physical keys belong to you: KEY (IO10) → `IN_A`, BOOT → `IN_B`. A long press still tunes away. |
| `CH_WIDE` | `row()` fills **480** panel pixels instead of 240 (for sources wider than 240, e.g. 256-px machines). |
| `CH_ENDLESS` | never drift off by yourself (games, emulators). Without it, `length()` ends the programme and the set drifts into snow. |
| `CH_MISBEHAVE` | the set wakes up on you in snow instead of locking straight in: a knock (IMU tap) tunes it in at once, otherwise it finds the picture by itself after 2 s. A hard knock on the picture knocks it back into snow, which waits for the next knock. The test card has this. |

### The picture

- The logical picture is **240×240**. Each logical row becomes two panel rows (the
  second darker: scanlines) and each pixel two panel pixels wide. A `CH_WIDE` channel
  paints 480 pixels per row and maps its own columns.
- Pixels are **RGB565**, written left to right, logical "up". Never rotate: the panel does.
- **The screen's corners are rounded** and hide part of the picture. Keep text and anything
  the player must see out of the corners. Centre the score line, or keep it about 20 px
  in from each corner. Edges of a playfield may run into the corners.
- Build palettes in `begin()` / `tick()` and honour `gChroma` (0 = black-and-white,
  16 = full colour); the tuner drains colour when the signal weakens. The test card's
  `buildPalette()` (`channels/testcard/testcard.cpp`) is the pattern: rebuild when
  `gChroma` changes.

### Colour and text

The platform has no drawing library on purpose (a channel paints rows), but two things
every channel needs are easy to get wrong:

```cpp
// RGB565 from 8-bit components (copy this into your .cpp)
#define RGB(r,g,b) ((uint16_t)((((r)&0xF8)<<8) | (((g)&0xFC)<<3) | ((b)>>3)))

// A palette that follows gChroma (declared in platform/channel.h): build it in begin(),
// rebuild in tick() whenever gChroma changes. 16 = full colour, 0 = grey.
static uint16_t PAL[N]; static int s_chromaBuilt = -1;
static void buildPalette() {
  for (int i = 0; i < N; i++) {
    int r = SRC[i][0], g = SRC[i][1], b = SRC[i][2];
    int l = (r * 77 + g * 150 + b * 29) >> 8;            // luma
    r = l + ((r - l) * gChroma) / 16; g = l + ((g - l) * gChroma) / 16; b = l + ((b - l) * gChroma) / 16;
    PAL[i] = RGB(r, g, b);
  }
  s_chromaBuilt = gChroma;
}
```

Text: `platform/font5x7.h` gives `font5x7Glyph(c)`: five column bytes for ASCII 32..126,
bit 0 = top row; a character is 5×7 with a 6-pixel advance. Painting a string inside
`row()` stays integer-only:

```cpp
if (y >= y0 && y < y0 + 7) {                       // the string's band
  int gy = y - y0;
  for (int i = 0; s[i]; i++) {
    const uint8_t* g = font5x7Glyph(s[i]);
    for (int c = 0; c < 5; c++) if ((g[c] >> gy) & 1) dst[x0 + i * 6 + c] = PAL[WHITE];
  }
}
```
Prepare the string (`snprintf` a score) in `tick()`, not in `row()`.

### Hard rules for `row()`

1. **Integer only.** The ESP32-C6 has no FPU. Floats are fine in `begin()` and in
   `tick()` (once per frame), never per pixel.
2. **Budget: all 240 rows in under 8 ms** on the board (~33 µs per row). On a Mac the
   host check wants under 1 ms per frame.
3. **No hardware, no allocation, no logging.** `row()` is called inside the display
   transaction; touching I²C, Wi-Fi or `Serial` here blanks the panel.
4. Anything that doesn't change per row belongs in `tick()`.

### Input

Drain the queue in `tick()`:

```cpp
InputEvent e;
while (inputPoll(&e)) {
  if (e.key == IN_START && e.down) restart();
}
int16_t knob = inputAxis(IN_KNOB, 0);   // -32768 top .. 32767 bottom, player 0
bool left    = inputDown(IN_LEFT, 1);   // held state, player 1
```

Keys: `IN_LEFT/RIGHT/UP/DOWN`, `IN_A/B/START/SELECT`, `IN_KNOB` (axis), `IN_HEX0..F`,
`IN_CHAR` (typed character, `value` = code). Players 0 and 1 (two phones). `IN_TUNE`,
`IN_POWER`, `IN_MUTE`, `IN_VOLUME` never reach you; the platform takes them.

### Sound

```cpp
// firmware/platform/sound.h
struct Sound {
  float detune;   // in:  the tuner's detune this frame, 0 = locked .. 1 = signal gone
  bool  tone;     // out: left false = silence, the default after `Sound s = {};`
  float hz;       // out
  float level;    // out: 0..1, before the platform's own volume/warm-up scaling
};

static void mySound(uint32_t now, Sound* s) {
  if (justHit) soundBeep(s, 440, 40);      // Hz, ms; sets tone/hz/level = 1.0
  // or feed samples: soundPcm(s, buf, n)  // s8, 16 kHz, for emulators and films
}
```
The platform mixes you over the set's own hiss and hum, scales everything by the
volume, and fades you out as the signal drifts.

`sound()` is called once per frame, whether or not you have anything to say: leave
`tone` false and it mixes in nothing. `detune` comes in already set, so a tone that
"belongs to the signal" (the test card's 1 kHz PM5544 tone: full level near lock, mute
in snow) reads it directly instead of the platform special-casing your channel; a short
event-driven tone (a paddle hit) instead remembers its own "until" timestamp from
`tick()` and just answers whether it's still within it, each frame; see
`channels/pong/pong.cpp`.

### Memory

Small state: `static` in your `.cpp`. Big buffers (> 4 KB): claim them from the arena in
`start()` and release them in `stop()`. Never `malloc` in `tick()` or `row()`.

## Making a game playable

The host check measures speed and the sheet shows the picture; neither tells you a game
is fun. These come from the first channels written by agents, each one a real bug:

- **The control covers the whole play area.** Map the full axis range (−32768 … 32767) onto
  the paddle's full travel, edge to edge. Check the sheet shows the paddle at both ends.
- **Match the slider to the motion.** A paddle that moves sideways gets
  `{"slider":{"dir":"h"}}`; one that moves up and down the plain slider.
- **Keep the usual key names on buttons.** START is labelled `start`; don't invent a word.
- **The ball must not skip.** The board runs at 13–25 frames a second, not the check's 24:
  at Pong's top speed (520 px/s) that is up to 40 px a frame. Move in steps no longer than
  half the thinnest thing the ball can hit (sub-step in `tick()`), and always bounce: a
  ball that passes through bricks is a bug, not a power-up.
- **Serve from the player.** The ball starts at the paddle, not at a random spot.
- **One text size for the score line.** Label and number in one string; big digits over
  tiny labels overlap.
- **Pace:** `pong` is the reference: serve at 150 px/s, rising with play.
- **Give the end a reason.** When a level is cleared, say so on screen and make the next
  one different or harder; a game that just refills the same wall gets abandoned.

## The files of a channel

```
firmware/channels/<id>/
  <id>.cpp        the channel
  <id>.h          extern const Channel CH_<ID>;
  README.md       what it is, controls, credits
  preview.png     the last frame  ┐ made by tools/hostrender/make-previews.sh <id>,
  preview.gif     a short loop    ┘ playing the channel through its panel
  preview.conf    optional: your own preview story (hostrender options, GIF size)
```
Register it in `firmware/channels/channels.cpp`: an `#include` of your header and `&CH_<ID>`
at the end of `CHANNELS[]`. Nothing else: the board build and the host check both read that list. Content that isn't yours (a
ROM, a clip) goes next to it with a `LICENSE` file and a row in `THIRD_PARTY.md`.

## Check it on your computer first

```
cd tools/hostrender && make && ./hostrender --check <id> --quiet
```
renders 5 s at 24 fps through the real glitch layer, playing your channel the way its own
phone panel would (START pressed at 0.5 s, the slider swept up and down, if the panel
has them; `--input` replaces this with your own script), fails (non-zero exit) if the mean frame
is over 1 ms on the host, and writes a contact sheet under `tools/hostrender/out/`; it
changes nothing in the repo. When the channel looks right, make its previews:
`tools/hostrender/make-previews.sh <id>` (needs ffmpeg). Look at the sheet: the 1 ms budget only catches
*slow*, not *wrong*: a `row()` that silently reads a stale palette still renders in time.
(A static check for a stray float in `row()` is not implemented; the frame-budget check
above is the concrete proxy for "cheap enough for the board".)

## Definition of done

- [ ] `./hostrender --check <id>` (in `tools/hostrender/`) passes and the sheet looks right to a human
- [ ] runs on the board through a tune-in, a knock and a drift without artefacts
- [ ] `gChroma 0` looks right (black-and-white)
- [ ] the panel declared in `info.panel` makes the channel playable from a phone
- [ ] README with controls and credits; licences for anything not yours
