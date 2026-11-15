# Sacred Gear Lua API reference

The reference of everything a mod can call, generated from the LuaLS annotations in `gear/meta/`. The guide to writing
a mod (install, events, examples, automation, engine notes) is [Gear.md](Gear.md).

## Editor support (Lua Language Server)

`gear/meta/*.lua` are [LuaLS](https://luals.github.io/) definition files (`---@meta`): they declare the `gear` global,
the objects it returns, the events and callbacks, the options tables, and the manifest of `mod.lua`. With them the
editor completes and checks mod code (VS Code with the "Lua" extension by sumneko, or any editor with LuaLS).

In this repository nothing is needed: `.luarc.json` at the root points LuaLS at `gear/meta`, so the examples in
`gear/examples` get completion and checks.

**In a mod repository** (for instance `sacred-gear-mods`, next to a checkout of this one): copy
`gear/meta/luarc.template.json` to `.luarc.json` in the root of the mod repository and set `workspace.library` to the
`gear/meta` folder (a relative path from that root, or an absolute one). That is all.

What the template sets besides the library:

- `runtime.version` is `LuaJIT`, the Lua Sacred Gear runs.
- `runtime.builtin` hides the libraries a mod does not get: `io`, `debug`, `package`, `jit`, `ffi`, `utf8`, `bit32`, and
  `os`, which `gear/meta/gear.lua` declares again with the four functions mods have (`clock`, `time`, `date`,
  `difftime`). The basic functions a mod lacks (`load`, `loadstring`, `loadfile`, `dofile`, `collectgarbage`,
  `getfenv`, `setfenv`, `newproxy`, `gcinfo`) are declared there as deprecated, so the editor strikes them through.
  What a mod has: `assert error ipairs next pairs pcall rawequal rawget rawset select setmetatable getmetatable
  tonumber tostring type unpack xpcall`, `_VERSION`, `print` and `require` (its own files only), `string`, `table`,
  `math`, `bit`, `coroutine`, and `gear`.

In `mod.lua`, annotate the returned table for completion of the manifest keys:

```lua
---@type GearModManifest
return {
    id = 'hello',
    name = 'Hello, Sacred Gear',
    version = '1.0',
}
```

Handlers get typed arguments from `gear.on`:

```lua
gear.on('frame', function(dt)            -- dt: number
    local hero = gear.world.hero()       -- Creature?
    if hero then print(hero:hp(), hero:class()) end
end)
gear.on('draw', function(state)          -- state: GearUiState
    if state.mode ~= 'game' then return end
    gear.ui.text('Hello', 24, 24, {color = 0xFFFFD080, shadow = true})
end)
```

Not covered by the annotations: values a mod gets from a custom event (`gear.emit` and its handlers take `...`), keys
of `gear.config` (declare your own with `---@type {my_setting: number}` on a local), and the runtime rule that
`gear.auto` exists only in automation runs.

## Maintaining the annotations

The annotations are part of the API: a change to what the Lua side offers is not done until `gear/meta` says so.

1. Change the code (`gear/lua/kernel.lua`, and the native tables in `gear/lua_host.cpp`).
2. Change the matching annotation in `gear/meta/*.lua`: a new function gets a description, `---@param` and
   `---@return` lines; a new field of a table gets a `---@field`; a new option goes into the options class of the
   function; a new event gets a `gear.on` overload and a `GearEventName` entry.
3. Run `python tools/gen_gear_api_docs.py` (rewrites this file) and `python tools/check_gear_api.py` (exit code 0:
   the annotations and the code agree).

`tools/check_gear_api.py` reads `kernel.lua` and `lua_host.cpp` as text and compares names, both ways: a function,
field, event, option or manifest key in the code but not in the annotations, and one in the annotations but not in the
code. It checks names, not types or signatures; those are reviewed by hand. It fails if this file is not what the
generator produces. If the check cannot find a place in the code it expects, the code changed shape: update the
check rather than silence it.

The format of `gear/meta/*.lua`:

- One file per area (`gear.lua`, `data.lua`, `world.lua`, `ui.lua`, `ui_widgets.lua`, `auto.lua`; the list is `FILES`
  in `tools/gear_meta.py`). Every file starts with `---@meta _`.
- `-- ## Title` starts a section of the reference; `--: text` lines right under it are the section's introduction.
  Both are plain comments to LuaLS.
- A namespace or object type is a `---@class` followed by a local table (`local GearWorld = {}`); its functions are
  `function GearWorld.hero() end` (static) or `function Creature:hp() end` (method). The `gear` and `os` globals are
  `gear = {}` and `os = {}` after their class.
- A plain comment `-- path gear.world` right above the class's doc block says how it is reached; the reference prints
  signatures with it (`gear.world.hero()`, `creature:hp()`).
- Options tables are classes whose name ends in `Opts` (`GearTextOpts`); every `opts.x` the kernel reads must be a field
  of one, and `GearPickupRules` holds the keys of `pickup_rules`. Enumerated strings are `---@alias` with `---| 'value' # meaning`.
- Return a value or `nil` and a message as `---@return Creature? creature` and `---@return string? error`.
- Descriptions use the same words as [Gear.md](Gear.md); the guide explains, the reference lists.

To check the examples against the annotations with LuaLS: `lua-language-server --check gear/examples
--configpath .luarc.json` from the repository root (needs the language server installed; not part of the build).

