# firmware

PlatformIO project. `platformio.ini` sets `src_dir = .` and a `build_src_filter` that
picks up `platform/`, `channels/` and `src/`; see the comment at the top of that file.
Layout:

- `platform/`: everything that makes it a television: the set (`set.cpp`, what
  `main.cpp`'s `setup()`/`loop()` delegate to), tuner, CRT model, glitch layer, sound,
  input queue, hardware drivers (`hw/`), the REMOTE card, the arena. Channels never touch
  hardware.
- `channels/<id>/`: one folder per channel: code, README, a host-rendered screenshot,
  registered by one line in `channels/channels.cpp` (the order is the station numbering).
- `src/main.cpp`: tiny: `setup()`/`loop()` calling into `platform/set.cpp`.
- `lib/`: PlatformIO local libraries not published anywhere `lib_deps` can reach
  (currently just `ES8311/`, the codec driver; see `THIRD_PARTY.md`). PlatformIO's
  library finder picks this directory up on its own; it needs no entry in
  `build_src_filter`.

The phone page source lives at the repo's top level, `tools/remote/page.html`, built
into `platform/hw/webctl_page.h` by `tools/remote/build_page.py` (not `firmware/remote/`:
there is one home for it, and `tools/` is where the rest of the build-time generators
live too, alongside `tools/hostrender/`).
