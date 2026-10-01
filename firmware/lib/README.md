# firmware/lib

PlatformIO local libraries: dependencies not published anywhere `lib_deps` can reach.
PlatformIO's library finder picks this directory up on its own; nothing here needs an
entry in `platformio.ini`'s `build_src_filter`.

## `ES8311/`

Espressif Systems' driver for the ES8311 audio codec (`es8311.c`, `es8311.h`,
`es8311_reg.h`), vendored unmodified so `firmware/platform/hw/audio.cpp` can find
`es8311.h`; it isn't in the Arduino core or available as a `lib_deps` package. Full
provenance is in `THIRD_PARTY.md`.

Licence: **Apache License 2.0**, Copyright Espressif Systems (Shanghai) CO LTD. Full
text in `ES8311/LICENSE.md`.
