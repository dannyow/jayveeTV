# tools/hostrender

Renders a channel on the host (Mac), through the real platform code (tuner, CRT model,
glitch layer, the channel's own `tick()`/`row()`) to PNG frames, exactly as
`docs/CHANNELS.md` describes the pipeline. Also the render-budget check every channel
must pass before it goes near the board.

## Build

```
cd tools/hostrender
make          # builds ./hostrender
make check    # renders the test card once, decodes the PNG with sips (macOS)
make clean
```

No external libraries. `stubs/Arduino.h` is a minimal host stand-in for the Arduino
functions a channel might use (`millis()`, `random()`, `Serial`, …); `millis()` returns
the harness's fake, deterministic clock, not wall time. The harness renders exactly the
stations the board registers in `firmware/channels/channels.cpp`: register a channel
there once and `--channel`/`--check` know it; the Makefile picks up
`firmware/channels/*/*.cpp` on its own. `ch_inputprobe.cpp` is a host-only
channel (`inputprobe`, not registered on the board) that shows what `input.h` is
delivering (a grid of keys plus the knob axis) and doubles as the reference for how a
game channel drains `inputPoll()`/reads `inputDown()`/`inputAxis()`. `png.cpp`/`png.h` is
a small dependency-free PNG writer.

## Command line

Run from `tools/hostrender/`; paths in `--check` and the default `--out` are relative to
that directory.

| flag | does |
|---|---|
| `--channel NAME` | channel to render (required unless `--check`/`--list`) |
| `--check ID` | docs/CHANNELS.md's definition-of-done check: 5 s at 24 fps, fails (exit 1) if the mean frame is over 1 ms on the host; writes `out/<id>/sheet.png` and nothing in the repo. Without `--input` it plays the channel through its own panel: presses START at 0.5 s and sweeps the slider, if the panel has them. Writes no per-frame PNGs. |
| `--list` | print registered channel names and exit |
| `--frames N` | frames to render (default 24; `--check` defaults to 5 s worth) |
| `--fps N` | fake clock's frame rate (default 24) |
| `--t0 MS` | fake `millis()` at frame 0 (default 0) |
| `--out DIR` | output dir for `fNNN.png` (default `out/`) |
| `--ppm` | write PPM instead of PNG |
| `--tuner SPEC` | `locked\|losing\|lost\|tuning\|auto`, optionally a timeline (`locked,kick@2000,lost@6000`); default `locked` |
| `--crt SPEC` | `on\|warmup\|off`, optionally a timeline (`on,off@1500,on@4000`); default `on` |
| `--boot` | shorthand for `--crt warmup --tuner tuning` |
| `--chroma 0..16` | force `gChroma` every frame instead of letting the tuner set it |
| `--input "t=MS key=NAME down=0\|1 [player=N] [value=V]"` | queue an input event before that frame's `tick()`; repeatable |
| `--sheet FILE.png` | contact sheet, one 240×240 tile per second, labelled |
| `--no-dim` | skip the panel-brightness multiply (tuner sag, CRT warm-up) |
| `--url URL` | sets what the REMOTE card's QR encodes (`remoteCardSetNet`) |
| `--clock HH:MM\|none` | the wall clock the platform hands the test card (default `10:10`, as on the set) |
| `--drive` | play the channel through its own panel, as `--check` does (START, slider) |
| `--quiet` | no per-frame log line |

## Examples

```
./hostrender --channel testcard --frames 48 --sheet out/testcard/sheet.png
./hostrender --boot --channel testcard --frames 120
./hostrender --check pong
```

## Output

Everything goes under `out/` (gitignored): frames under `--out`, sheets, and `--check`'s
`out/<id>/sheet.png`. Only `make-previews.sh` writes into the repo, and only into the
channel folders.

## README animations

`make-previews.sh [id ...]` writes `preview.png` and `preview.gif` into each channel's folder
(every station by default), playing it through its panel like `--check` (`--drive`). A
channel can tell its own story in `firmware/channels/<id>/preview.conf` (the test card:
snow, a knock, the picture tunes in). Needs ffmpeg. The README's channel table shows these
GIFs. Run it again after a channel or the glitch layer changes.
