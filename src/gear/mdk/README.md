# Sacred Gear MDK

Everything to write [Sacred Gear](https://github.com/kirides/sacred-bild/blob/main/docs/Gear.md) mods in Lua with
editor help.

| Folder | What it is |
|---|---|
| `meta/` | LuaLS definitions of the `gear` API: completion, parameter help and type checks in the editor. Generated from the same sources as the API reference. |
| `docs/GearApi.md` | The API reference. |
| `docs/Gear.md` | The guide to writing a mod: `mod.lua`, events, the file overlay, the mod manager, automation. |
| `example-mod/` | A small mod to start from: `mod.lua` (manifest with a setting and a key bind) and `main.lua` (draws text). `new-mod.ps1` copies it. |
| `examples/` | Small working mods (`hello`, `uidemo`, `saveinfo`, `hooks`: reacting to other mods' hooks). |
| `tools/` | `new-mod.ps1` makes a mod from `example-mod`; `pack-mod.ps1` zips a mod for players. |

## Start

1. Install [SacredBild and Sacred Gear](https://github.com/kirides/sacred-bild#install) into the game folder.
2. Install [Visual Studio Code](https://code.visualstudio.com/) and open this folder in it. It recommends the **Lua**
   extension (sumneko); `.luarc.json` already points it at `meta/`. Any editor with LuaLS works the same way.
3. Make a mod:

   ```powershell
   .\tools\new-mod.ps1 -Id my-mod -Name "My mod"
   ```

   This creates `src\my-mod` with `mod.lua` (the manifest) and `main.lua` (the code).
4. Copy the folder to `<game folder>\mods\my-mod`, start the game and press **F11**: the mod is listed, switch it on.
   Whatever `print()` writes goes to `SacredGear.log` next to the game.
5. Give it to players with `.\tools\pack-mod.ps1 -Mod src\my-mod`: the zip extracts into the `mods` folder.

## Good to know

- A mod is a folder: `mod.lua` returns the manifest (id, name, version, `settings`, `keys`, `requires`), `main.lua` is
  loaded with a `gear` global and no `io`, `os` (but four time functions), `debug` or `ffi`. The editor settings in
  `.luarc.json` hide what a mod does not get.
- Annotate `mod.lua` with `---@type GearModManifest` and the editor completes the manifest keys.
- Settings and keys declared in `mod.lua` appear in the Mods screen; read them with `gear.settings.get`, react to
  `gear.keys.on`.
- More mods to read: [sacred-gear-mods](https://github.com/kirides/sacred-gear-mods).
- The MDK is part of every SacredBild release, so `meta/` matches the Sacred Gear you download with it. Using an
  older Sacred Gear with a newer MDK may show functions the game does not have.

Licensed as SacredBild (see `LICENSE`).
