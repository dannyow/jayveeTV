# tools/hooks

The repo-rules check, and the git hooks that run it.

## Install

```
tools/hooks/install.sh
```

Sets `core.hooksPath` to this directory (no copying; edits here take effect
immediately) and makes the hook scripts executable. Run once per clone.

## What runs

- **`check.sh`**: the actual rules, used both by the pre-commit hook (staged files
  only) and by CI (`check.sh` with no arguments, every tracked file via `git ls-files`).
  Run from the repo root. For each file it checks:
  - not a secret: blocks `.env`, `*/.env`, anything ending `secrets.h`.
  - not an accidental large file: anything over 1 MB must be listed, path exactly as
    tracked, one per line, in `.large-files-allowed` (today: the white case STL).
  - not unlicensed content: any file with a content extension (`bin rom tap tzx sna z80
    ch8 jvcs mp4 mov wav mp3`) needs a `LICENSE*` file in the same directory and a row
    in `THIRD_PARTY.md` (matched by path or filename).
- **`pre-commit`**: runs `check.sh` against the staged files; no-op if nothing is staged.
- **`pre-push`**: refuses a push whose remote ref is `refs/heads/main`. Maintainers only:
  `JVTV_ALLOW_MAIN=1` skips this for the bootstrap push. Don't reach for it otherwise;
  normal work goes through a pull request (`CONTRIBUTING.md`).
