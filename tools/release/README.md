# tools/release

- **`build-bin.sh [version]`**: builds `firmware/` (`release1`) and merges bootloader,
  partition table, `boot_app0` and the app into one image that flashes at `0x0`:
  `dist/jayveetv-<version>-esp32c6-16mb.bin` plus its `.sha256`. Run from anywhere;
  the version defaults to `git describe`. Needs PlatformIO (`PIO=/path/to/pio` if it is
  not on `PATH`); esptool comes with it.

Flash the result with `esptool.py --chip esp32c6 write_flash 0x0 <file>.bin`.
