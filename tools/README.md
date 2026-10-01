# tools

Build-time and dev tooling. Nothing here is compiled into the firmware directly;
`remote/page.html` is baked into a header by `build_page.py` before the build.

- [`hooks/`](hooks/README.md): the repo-rules check (`check.sh`) and the git hooks that
  run it on commit and refuse pushes to `main`.
- [`hostrender/`](hostrender/README.md): renders channels on the host (Mac) to PNGs
  through the real glitch layer, and checks the per-row render budget before anything
  goes near the board.
- [`remote/`](remote/README.md): the phone remote's page source and the script that
  gzips it into the firmware.
- [`release/`](release/README.md): builds the single firmware image attached to a release.
