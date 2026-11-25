# Sacred Gear Lua API reference

Everything a mod can call, by area: functions, options, fields and events. It is generated from the LuaLS annotations
in `src/gear/meta/` (how to change them: `docs/Development.md`, "The Lua API annotations"). The guide to writing a mod
(install, `mod.lua`, events, the file overlay, the mod manager, automation, examples) is [Gear.md](Gear.md).

## Editor support (Lua Language Server)

`src/gear/meta/*.lua` are [LuaLS](https://luals.github.io/) definition files (`---@meta`): they declare the `gear` global,
the objects it returns, the events and callbacks, the options tables, and the manifest of `mod.lua`. With them the
editor completes and checks mod code (VS Code with the "Lua" extension by sumneko, or any editor with LuaLS).

In this repository nothing is needed: `.luarc.json` at the root points LuaLS at `src/gear/meta`, so the examples in
`src/gear/examples` get completion and checks.

**Without this repository: the MDK.** Each release (and each nightly) has `sacred-gear-mdk.zip`, a mod development kit built from
`src/gear/meta` by `tools/gearapi/gen_gear_mdk.py` (so it is always current): the definitions, this reference, `Gear.md`, a mod
template, a few examples and scripts to create and pack a mod. Extract it, open the folder in VS Code and run
`tools/new-mod.ps1`; `.luarc.json` is already set up.

**In a mod repository** (for instance `sacred-gear-mods`, next to a checkout of this one): copy
`src/gear/meta/luarc.template.json` to `.luarc.json` in the root of the mod repository and set `workspace.library` to the
`src/gear/meta` folder (a relative path from that root, or an absolute one). That is all.

What the template sets besides the library:

- `runtime.version` is `LuaJIT`, the Lua Sacred Gear runs.
- `runtime.builtin` hides the libraries a mod does not get: `io`, `debug`, `package`, `jit`, `ffi`, `utf8`, `bit32`, and
  `os`, which `src/gear/meta/gear.lua` declares again with the four functions mods have (`clock`, `time`, `date`,
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
