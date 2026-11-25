# Sacred-Bild

## Versions (ddraw.dll and SacredGear.dll)

Both DLLs have a version in a flat text file: `src/bild/SacredBild_version.txt` and `gear/SacredGear_version.txt`. Never edit them by hand.
`tools/bump_versions.py` raises them from the git history (patch for any non-test change, `feat(<scope>)`/`feat:` in a commit message
means minor, `break(<scope>)`/`feat!:` major; scopes `gear`, `bild`) and refreshes `mod-index.json`. See docs/Versioning.md.

- **Agents never bump (see the next bullet). For John's own work: on local `main`, after every merge or direct commit, before you report the work as done:** `python tools/bump_versions.py --commit`
  (the `post-merge` hook does this for merges when `core.hooksPath` is `.githooks`; check with `git config core.hooksPath`, set it with
  `git config core.hooksPath .githooks`).
- **Agents / parallel sessions: always opt out, never run `bump_versions.py`.** Run git through `tools/agent-git.ps1` (or `tools/agent-git.cmd`), or set `SACRED_NO_BUMP=1`;
  the `post-merge` hook then skips the bump and John's own merges still bump.
- Write commit messages so the bump level is right: `feat(gear): ...` for a new feature, `break(bild): ...` for a change that breaks
  saves, settings or the mod API, anything else is a patch.
- **Changelog:** player-facing release notes live in `changelog/bild/vX.Y.Z.md` and `changelog/gear/vX.Y.Z.md` (one file per released version, starting with
  `## vX.Y.Z (date)`, short bullets `New:`/`Fixed:`/`Improved:`/`Under the hood:`, never vague). Write the note for the version a bump will produce, then run
  `python tools/make_changelog.py` (writes each `history.md`, newest first; `--check` verifies). `changelog/` never triggers a bump.
- Never push (John does). His `pre-push` hook refuses `main` with a pending bump, so a forgotten bump shows up there.
- After changing a version or `tools/make_mod_index.py`: `python tools/make_mod_index.py --check` and `python tools/test_bump_versions.py`.
