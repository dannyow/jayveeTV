# tools/remote

The phone remote's page, and the script that gets it into the firmware.

- **`page.html`**: the page source, in the Capsule look (white case, black keys, one orange accent).
  On the set's own network (AP mode) it opens on the Wi-Fi setup screen; on the home
  network it is the remote: the shell (connection state, player slot, phones online, now
  playing, power/mute/tune/volume, the channel list) is the same on every channel, and the
  panel below it is built at runtime from the current channel's `panel` JSON in
  `/channels.json`: `slider`, `dpad`, `buttons` and `tabs` blocks, per `docs/REMOTE.md`.
  `keyboard` and `files` blocks show a placeholder; no channel uses them yet. Tapping the
  now-playing strip toggles between the panel and the channel list. Edit this file, never
  `webctl_page.h`.
- **`build_page.py`**: gzips `page.html` into `firmware/platform/hw/webctl_page.h`.
  That header is generated (it says so at the top) and is served as-is
  (`Content-Encoding: gzip`); flash only ever holds the gzip. The build still refuses
  past 32 KB raw: not a size limit, a readability guard against the page quietly
  growing unreadable.
- **`smoke.py`**: smoke test against a live set: `GET /`, `GET /channels.json`, and the
  WebSocket player-slot / tune-broadcast behaviour from `docs/REMOTE.md`. Run as
  `python3 tools/remote/smoke.py http://<set-address>/` (needs `websockets`; the
  set's address is required).

## The loop

Edit `page.html` → `python3 tools/remote/build_page.py` → commit both `page.html` and
the regenerated `webctl_page.h` in the same commit. Never hand-edit `webctl_page.h`.
