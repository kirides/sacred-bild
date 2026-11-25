# Versions and the mod index

Sacred-Bild and Sacred Gear are two DLLs with two versions (`major.minor.patch`), each in a flat text file in the code it versions:

| Component | DLL | Version file | Code that counts |
|---|---|---|---|
| Sacred-Bild | `ddraw.dll` | `src/bild/SacredBild_version.txt` | `src/bild/` (with `settings/SacredBild.ini`), `src/core/`, `CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json`, `ports/`, `triplets/` |
| Sacred Gear | `SacredGear.dll` | `gear/SacredGear_version.txt` | `gear/` (not `examples`, `mdk`, `dist`, `meta`) and the shared core `src/core/` |

Test files (folders `test`, `tests`, `tools`, anything ending in `test`/`tests`, `test_*`) never count. The release workflow copies the
version file into the zip next to the DLL and names the zips `SacredBild-v<version>.zip` and `SacredGear-v<version>.zip`.

## Bumping

`tools/bump_versions.py` reads the history since the last commit that changed the version file:

- any change to the code of the component: patch (`1.2.3` to `1.2.4`)
- a commit message with `feat(<scope>)` or starting with `feat:`: minor (`1.3.0`)
- `break(<scope>)`, `feat!:` or `feat(<scope>)!:`: major (`2.0.0`)

Scopes: `gear` / `sacred-gear` for Sacred Gear, `bild` / `sacred-bild` / `ddraw` for Sacred-Bild, `feat(gear, bild)` for both. `feat:`
without a scope counts for every component the commit changed. The highest level wins.

```
python tools/bump_versions.py            # show what would change
python tools/bump_versions.py --commit   # one commit per component, then refresh mod-index.json
python tools/bump_versions.py --check    # exit 1 when a bump is pending
```

Run it on `main`, not on feature branches (parallel branches would all edit the same file and conflict).

## Making sure it happens

Activate the hooks once per clone (all worktrees share it): `git config core.hooksPath .githooks`.

- `post-merge`: after a merge into `main` the bump runs and is committed automatically.
  Skip it with `SACRED_NO_BUMP=1` (agents use `tools/agent-git.ps1 <git args>`, which sets it).
- `pre-push`: pushing `main` or a `v*` tag is refused while a bump is pending or `mod-index.json` is stale (bypass: `git push --no-verify`).
- `CLAUDE.md` tells the assistant to run the script after committing on `main`.
- CI checks that `mod-index.json` matches the version files.

A pre-commit hook was left out on purpose: at that point the commit is not in the history yet, so the level cannot be derived, and bumping
on every commit would make every feature branch conflict on the version file.

## Mod index

`mod-index.json` (schema 1, see the Gear Mod Manager's `docs/ModIndex.md`) lists both DLLs as `kind: plugin` items for the manager and
points it at the release assets (`download.github` plus an asset pattern, nightlies included as pre-releases). It also follows the index of
the Gear mods. `tools/make_mod_index.py` generates it from the version files; edit the item table in that script for descriptions, tags
or install rules.
