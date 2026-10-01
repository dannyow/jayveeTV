# Notes for coding agents

JayVeeTV channels are meant to be written by LLM agents. A channel is one folder, the
contract is small and written down, and a host-side check tells you whether it works
before anyone touches a board. If you need anything that is not written down, the docs
are wrong: say so in your answer.

## Writing a channel

1. Read [`docs/CHANNELS.md`](docs/CHANNELS.md) to the end. It is the contract; you should
   not need to read platform code.
2. Use the two channels as patterns: `firmware/channels/pong/` (a game: input, sound,
   phone panel) and `firmware/channels/testcard/` (a palette that follows `gChroma`).
3. Create `firmware/channels/<id>/<id>.cpp`, `<id>.h` and `README.md`. Register the channel
   in `firmware/channels/channels.cpp`: `#include` your header and add `&CH_<ID>` at the end
   of `CHANNELS[]`: that list is the station order; the board build and the host check
   both read it.
4. Declare what the phone shows in `info.panel` (blocks: `slider`, `dpad`, `buttons`,
   `tabs`; see [`docs/REMOTE.md`](docs/REMOTE.md)). A paddle that moves sideways wants
   `{"slider":{"dir":"h"}}`. Read "Making a game playable" in `docs/CHANNELS.md` before you
   call a game done.
5. Check it:
   ```
   cd tools/hostrender && make && ./hostrender --check <id> --quiet
   ```
   It must end with `CHECK ok`. Then look at `tools/hostrender/out/<id>/sheet.png`: the
   check catches slow, not wrong. It presses START and sweeps the slider if your panel has
   them; for anything else, script it with `--input` (see `./hostrender --help`) so the
   sheet shows the channel working, not an idle screen.
6. Make the previews: `tools/hostrender/make-previews.sh <id>` writes `preview.png` and
   `preview.gif` into your channel's folder (needs ffmpeg). Add a row with the GIF to the
   channel table in `README.md`.
7. If PlatformIO is installed, build for the board: `cd firmware && pio run -e release1`.

## Do not

- Change `firmware/platform/` or `tools/` to make your channel work. If the contract does
  not cover what you need, stop and say what is missing.
- Use floats, division or calls in the per-pixel loop of `row()`; allocate memory; touch
  `Serial`, I²C or Wi-Fi from `row()`.
- Add content you cannot license (ROMs, images, sound). If you add any: a `LICENSE` file
  next to it and a row in `THIRD_PARTY.md`.
- Hand-edit `firmware/platform/hw/webctl_page.h`: it is generated from
  `tools/remote/page.html` by `python3 tools/remote/build_page.py`.

## Everything else

- `tools/hooks/check.sh` must pass. Commit subjects name the layer (`channel(<id>): …`,
  `platform: …`, `docs: …`). See [`CONTRIBUTING.md`](CONTRIBUTING.md).
- You have no board. Say plainly which steps need a person with the hardware
  (flashing, knocking, playing on the phone).
