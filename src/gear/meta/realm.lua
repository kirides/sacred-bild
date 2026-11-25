---@meta _
-- Sacred Gear: realms, the online play server.

-- ## Realms (gear.realm)

--- An online play (register / login) server the game can talk to.
---@class GearRealm
---@field id string Short name for scripts: letters, digits, `_` and `-` (the built-in one is `'default'`).
---@field name string What the player reads.
---@field host string The server's host name or IPv4 address (the built-in realm: the host of Settings.cfg once known, else empty).
---@field port? integer The lobby port (omitted: the game's own, NETWORK_LOBBYPORT).
---@field builtin? boolean The realm the game started with (from Settings.cfg); it cannot be removed.

-- path gear.realm
--- Choosing the server of the game's online play (register and login). The game reads the lobby host and port when
--- the multiplayer menu opens Closed or Open Internet (LAN is not touched), so a chosen realm applies from the next
--- time the player opens one of those; an open lobby connection keeps its server until the player leaves. The choice
--- is native (`mods\gear_realm.txt`, applied before any mod loads); the list and the active realm are kept in
--- `mods\gear_realms.json`. A realm names the server the player's password goes to: add realms only from sources you
--- trust.
---@class GearRealmApi
local GearRealmApi = {}

--- The realms: the built-in one first, then the added ones.
---@return GearRealm[]
function GearRealmApi.list() end

--- The chosen realm.
---@return GearRealm
function GearRealmApi.current() end

--- Adds a realm, or replaces the one with that id. `id` is 1 to 32 letters, digits, `_` or `-` and is not
--- `'default'`; `host` is a valid host name or IPv4 address (letters, digits, `.`, `-`, `_`); `port` 1 to 65535 (omitted:
--- the game's NETWORK_LOBBYPORT).
---@param def {id: string, name?: string, host: string, port?: integer}
---@return boolean? ok
---@return string? message Why not.
function GearRealmApi.add(def) end

--- Removes a realm; not the built-in one and not the one in use.
---@param id string
---@return boolean? ok
---@return string? message Why not.
function GearRealmApi.remove(id) end

--- Chooses the realm for the next Closed or Open Internet and saves the choice. Raises the event `realm.changed` when
--- the realm changed.
---@param id string
---@return boolean? ok
---@return string? message Why not.
function GearRealmApi.use(id) end
