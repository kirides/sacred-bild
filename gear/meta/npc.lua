---@meta _
-- Sacred Gear: custom NPCs (gear.npc).

-- ## Custom NPCs (gear.npc)
--: An NPC kind names an object type of the game (its model and animations are used as they are), a name of its own, dialog pages,
--: a daily routine and a look. `npc.spawn` makes a creature of a kind in the world. The creature carries the kind's name as its
--: own name text (`gear.text.key`, a text id taken from the kind's id: the same in every session, saved with the savegame, which
--: is also how a kind is recognised again after a load), so the game's overhead labels show it. The creatures a mod made are listed
--: in the mod's per-savegame data (`npc.json`, `gear.store.game`). Single player. The library is shipped in the DLL
--: (`gear/lua/npc.lua`) and runs once per mod, the first time the mod uses `gear.npc`.
--:
--: Clicking an NPC that has a dialog walks the hero up to it (`talk_range`) and opens the dialog window: a portrait, the page's text
--: and a button for each option of the page. A mod reacts to a button with the option's `on`, the kind's `on_talk` or an
--: `npc.on('button')` handler; the handler may open its own window (`e.close()` closes the dialog, `e.go(page)` shows another page).
--:
--: A routine puts the NPC at a point from a time of the game's clock on. When the time of the next point has come: if the hero is
--: within `walk_radius` of the NPC or the point, the NPC walks there (the game's own move order); when it makes no way for three
--: seconds (blocked) or needs more than two minutes, it is put there; if the hero is far away it is put there at once.

--- One option (button) of a dialog page.
---@class GearNpcOption
---@field id? string What `e.option` holds for a handler (default: the option's number).
---@field label string|fun(e: GearNpcEvent): string The button's text.
---@field on? fun(e: GearNpcEvent) Called when it is pressed.
---@field go? string The page to show afterwards.
---@field close? boolean Close the dialog afterwards (when nothing else was chosen).
---@field when? fun(e: GearNpcEvent): boolean Whether the option is shown (default: always).
---@field enabled? fun(e: GearNpcEvent): boolean Whether it can be pressed (default: always).

--- A page of an NPC's dialog.
---@class GearNpcPage
---@field text string|fun(e: GearNpcEvent): string What the NPC says.
---@field options? GearNpcOption[]

--- An NPC's dialog.
---@class GearNpcDialog
---@field start? string The first page (default: any one).
---@field pages table<string, GearNpcPage>

--- A point of a routine.
---@class GearNpcPoint
---@field time integer|string Minutes since midnight or `'HH:MM'`: from then on the NPC stands here.
---@field x number
---@field y number
---@field layer? integer

--- What `gear.npc.define` takes.
---@class GearNpcDef
---@field type integer|string The object type (id or name) whose model the NPC uses.
---@field name? string|table<string, string> The name shown (a string or `{en = ..., de = ...}`); default: the type's name.
---@field level? integer The creature's level (`creature:set_level`).
---@field like? Creature|integer A creature whose faction, level and health the NPC takes (default: the hero's faction, so the NPC is friendly: its label is not red and a click does not attack it).
---@field hostile? boolean Keep the faction of the type (an enemy NPC).
---@field dialog? GearNpcDialog
---@field routine? GearNpcPoint[]
---@field talk_range? number How close the hero must be to talk (world units, default 140).
---@field walk_radius? number The hero is this close (to the NPC or its goal) for the NPC to walk to the next point (default 900); otherwise it is put there.
---@field tint? integer|{all: integer?, parts: table<integer, integer>?} The colour of the model (`creature:set_tint`: a colour for the whole model or per mesh, e.g. the cloth); set again after every savegame load.
---@field texture? string|{pak: string}|{all: string|{pak: string}?, parts: table<integer, string|{pak: string}>?} Other textures for the model (`creature:set_texture`): a file of the mod (`'skin.png'`, PNG, TGA, JPG or DDS, both sides a power of two), `{pak = 'NAME.TGA'}` (a texture of the game's Texture.pak) or `{all = ..., parts = {[n] = ...}}` of those for single meshes. They replace the mesh's own textures, so they need the same texture layout; opaque images only. Set again after every savegame load.
---@field on_talk? fun(e: GearNpcEvent) Called when the dialog is opened.

--- What a dialog or routine handler gets.
---@class GearNpcEvent
---@field npc string The kind's id.
---@field creature Creature The NPC.
---@field hero? Creature
---@field page? string The page shown.
---@field option? string|integer The option's `id` (or number); `button` events.
---@field label? string
---@field point? GearNpcPoint The point reached; `arrived` events.
---@field close fun() Closes the dialog (after the handlers).
---@field go fun(page: string) Shows another page (after the handlers).
---@field say fun(text: string) Replaces the page's text until the page changes.

--- An NPC of the mod, as `list` gives it.
---@class GearNpcInfo
---@field creature Creature
---@field kind string
---@field data any What `spawn` was given as `opts.data`.

--- What `spawn` takes.
---@class GearNpcSpawnOpts
---@field layer? integer Default: the hero's.
---@field like? Creature|integer
---@field level? integer
---@field tint? integer|{all: integer?, parts: table<integer, integer>?} This NPC's own tint (instead of the kind's).
---@field texture? any This NPC's own texture (instead of the kind's), as `GearNpcDef.texture`.
---@field routine? GearNpcPoint[] This NPC's own routine (instead of the kind's).
---@field data? any Kept with the NPC in the savegame data (JSON values).

-- path gear.npc
--- Custom NPCs.
---@class GearNpc
local GearNpc = {}

--- Defines (or redefines) an NPC kind. `id` is a string that is yours alone (`'mymod.smith'`): the name text id comes from it and
--- the savegame keeps that id, so keep it stable.
---@param id string
---@param def GearNpcDef
---@return GearNpcDef def
function GearNpc.define(id, def) end

--- The definition of a kind, nil if there is none.
---@param id string
---@return GearNpcDef?
function GearNpc.kind(id) end

--- The ids of the kinds this mod defined, in order.
---@return string[]
function GearNpc.kinds() end

--- Which kind of this mod a creature is (by its name text): the id and the definition.
---@param creature Creature|integer
---@return string? id
---@return GearNpcDef? def
function GearNpc.of(creature) end

--- Makes an NPC of a kind at (x, y).
---@param kind string
---@param x number
---@param y number
---@param opts? GearNpcSpawnOpts
---@return Creature? creature
---@return string? error
function GearNpc.spawn(kind, x, y, opts) end

--- The live NPCs of this mod (of one kind).
---@param kind? string
---@return GearNpcInfo[]
function GearNpc.list(kind) end

--- Removes an NPC from the world and from the mod's list.
---@param creature Creature|integer
---@return boolean? ok
---@return string? error
function GearNpc.remove(creature) end

--- Handlers: `'talk'` (a dialog opened), `'button'` (an option pressed), `'close'` (the dialog ended), `'arrived'` (an NPC reached a
--- routine point); the argument is a `GearNpcEvent`.
---@param name 'talk'|'button'|'close'|'arrived'
---@param fn fun(e: GearNpcEvent)
---@return table handle
function GearNpc.on(name, fn) end

--- Removes a handler of `on`.
---@param handle table
function GearNpc.off(handle) end

--- Opens an NPC's dialog (at `page`, default the start page).
---@param creature Creature|integer
---@param page? string
---@return boolean? ok
---@return string? error
function GearNpc.talk(creature, page) end

--- Closes the dialog that is open.
function GearNpc.close() end

--- The dialog that is open: the kind, the NPC and the page; nil if none.
---@return {npc: string, creature: Creature, page: string}?
function GearNpc.dialog_state() end

--- The routine point an NPC should be at now (by the game's clock): x, y, layer.
---@param creature Creature|integer
---@return number? x
---@return number? y
---@return integer? layer
function GearNpc.where(creature) end

--- Gives one NPC its own routine (nil: the kind's again).
---@param creature Creature|integer
---@param points GearNpcPoint[]?
---@return boolean? ok
---@return string? error
function GearNpc.routine(creature, points) end
