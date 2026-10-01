# Contributing (and the rules we hold ourselves to)

1. **Never commit to `main` directly.** Branch, open a pull request, let CI pass, squash-merge.
   The pre-push hook refuses a push to `main`.
2. **Commit subjects name the layer**: `platform: …`, `channel(pong): …`, `remote: …`,
   `tools: …`, `docs: …`, `cad: …`, `ci: …`. English, imperative, what and why.
3. **No secrets.** `.env` and `firmware/**/secrets.h` never enter the index; the hook checks.
4. **No content without a licence.** Binary content (ROMs, tapes, clips, audio) needs a
   `LICENSE*` file in the same directory and a row in `THIRD_PARTY.md`.
5. **No large files by accident.** Anything over 1 MB must be listed in `.large-files-allowed`.
6. **Channels are verified on the host first** (`tools/hostrender`), on the board second.
7. **Releases are tags** (`v0.1`, `v0.2`, …); Printables links to a tag, never to `main`.

Install the hooks once per clone: `tools/hooks/install.sh`.
