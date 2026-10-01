# Third-party code and content

Every file in this repository that was not written for it is listed here with its source
and licence. The pre-commit hook refuses binary content without a licence next to it.

| path | what | licence | source |
|---|---|---|---|
| `firmware/platform/board.h`, `hw/audio.cpp`, `hw/display.cpp`, `hw/imu.cpp`, `hw/power.cpp` (parts marked "lifted from") | board pin map and hardware bring-up for this module, from the ESP32 port of Anthropic's claude-desktop-buddy | MIT, Copyright 2026 Anthropic, PBC (notice in `firmware/platform/hw/NOTICE-claude-desktop-buddy-esp32.md`) | https://github.com/vthinkxie/claude-desktop-buddy-esp32 |
| `firmware/platform/qrcodegen.c`, `qrcodegen.h` | QR Code generator library (C), Project Nayuki | MIT | https://www.nayuki.io/page/qr-code-generator-library |
| `firmware/lib/ES8311/` | ES8311 codec driver, Espressif Systems (vendored as a PlatformIO local library; `hw/audio.cpp` needs `es8311.h`, which isn't in the Arduino core or `lib_deps`) | Apache-2.0 | via the `claude-desktop-buddy-esp32` board bring-up (https://github.com/vthinkxie/claude-desktop-buddy-esp32) |
| (`lib_deps`, not vendored) | GFX Library for Arduino (moononournation) | BSD (Adafruit-derived) | https://github.com/moononournation/Arduino_GFX |
| (`lib_deps`, not vendored) | XPowersLib (lewisxhe) | MIT | https://github.com/lewisxhe/XPowersLib |
| (`lib_deps`, not vendored) | SensorLib (lewisxhe) | MIT | https://github.com/lewisxhe/SensorLib |
