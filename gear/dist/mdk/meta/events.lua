---@meta _
-- Sacred Gear: hooks between mods (gear.events).

-- ## Hooks between mods (gear.events)
--: A mod that has something other mods may want to react to (a bounty that is done, a world event that starts) *publishes a
--: hook*: it declares a custom event with `gear.events.declare` and fires it with `gear.emit`. Any mod subscribes with
--: `gear.on(name, fn)`, or with `gear.on('prefix.*', fn)` for every event of a mod (the handler then gets the event name first).
--: Rules: the name is `<mod>.<what>` (`bounty.completed`), the payload is one table (new fields may be added, never removed
--: or renamed), handlers must not change it, and a handler's error never reaches the publisher. Subscribing needs no
--: dependency: a handler for an event nobody emits just never runs, so the load order does not matter. Each computer emits
--: for what happens on it (in a network game every player's mod fires its own hero's hooks). The mods' hooks are listed in
--: the documentation of the mod that publishes them; the game server's Lua mods use the same names and payloads.

--- A declared hook.
---@class GearEventInfo
---@field name string
---@field mod string The id of the mod that declared it.
---@field doc string When it fires.
---@field args string The fields of the payload.
---@field listeners integer How many handlers are subscribed now.

--- What `declare` takes.
---@class GearEventDecl
---@field doc? string When the hook fires.
---@field args? string The fields of the payload table.

-- path gear.events
--- The hook registry.
---@class GearEvents
local GearEvents = {}

--- Says that this mod emits `name` (`bounty.completed`). Declaring twice is fine; another mod's name is refused. The declaration
--- ends when the mod unloads.
---@param name string
---@param info? GearEventDecl|string
function GearEvents.declare(name, info) end

--- The declared hooks, by name, optionally only those starting with `prefix` (`'bounty.'`).
---@param prefix? string
---@return GearEventInfo[]
function GearEvents.list(prefix) end

--- Whether a loaded mod has declared `name`.
---@param name string
---@return boolean
function GearEvents.declared(name) end
