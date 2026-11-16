# Sacred Gear: server-side mods (design proposal)

Status: proposal, nothing built. Goal: mods for online play, for example a per-player collect-a-thon ("player A
defeats 10 Orcs": the host congratulates them, stores the achievement per player, hands out items or gold).

## What Sacred's multiplayer does (research)

Sources: `docs/RE_NOTES.md`, the Sacred.exe decompile (`B:\DevDrive\sources\decomp-sacred`) and a new headless
decompile of GameServer.exe (`B:\DevDrive\sources\decomp-gameserver`, 8,447 functions; the main loop `0x4C3F20`
needed a 900 s decompile, output in `long/`). All addresses below are GameServer.exe unless noted.

- **There is a separate server process.** Hosting a game starts `GameServer.exe` (no window). The host's own
  `sacred.exe` joins it as an ordinary client, like every other player.
- **Players.** 16 slots. Per-player records at `net + 0x1B0 + slot * 0x518`. Slot N owns hero object id N (the
  object manager's ids 1..16 are the heroes), so slot == hero object id == a per-session player id. A record has an
  "active" flag at +9, a connection id at +0x90 and a data-flow setting. Nothing in the protocol is a stable
  account id: across sessions a player is only the hero (name, class) they join with.
- **The server is not a pure relay.** Its main loop `0x4C3F20` dequeues messages (0x10-byte queue entries: header
  dword with the sender slot in bits 27..31, 1..16 = player, 0x11 = server; message type as u16 at +0xE) and
  dispatches through a jump table (`0x4CC9C0`) on the type (3..0x1D5): 183 distinct handlers and one shared
  default for the other 238 types (relay/ignore). So about 180 message types, mostly the 0x101..0x1D5 object
  messages, are interpreted. The server creates real creature objects for the players' heroes (hero death handling
  at `0x4C3F20` around the `*puVar13 == 7` case sets state `+0xFC = 9`, `+0x150 = 6`, hp 0, experience, gold and level
  fields `+0x3B4 / +0x3EE / +0x3FE`) and tracks object ownership (`setObjectOwner` 0x4D6950, message 0x171).
- **Where kills happen.** Creatures are simulated by an owner client (`+0x39` owner slot, ISDN players never own
  ambient NPCs). Experience, gold and death are decided by the owner's `sacred.exe` and sent as messages; the
  server mirrors them. So a kill is visible in three places: the killing player's client, the owner's client and
  the server's object copy.
- **Chat / messages to a client:** not yet mapped (needs the message type of the chat line; the client's UI chat
  code is the entry point, then the server handler for that type).
- **Grants:** a client changes its own hero's gold/inventory locally and the engine broadcasts it; Gear's
  `add_gold`, inventory calls and spawn work this way but are refused in network games (`world.can_change()`).
- **Gear today:** only runs in `sacred.exe` (refuses `gameserver.exe`); mods have no multiplayer support, and
  there is a planned handshake that checks that all peers run the same mod set (`mod set` hash).

## Decision (from John, 2026-11-16)

Start with **host-client mods** (option 1); GameServer.exe mods (option 2) after, using the same Lua API.

## Phase 1: host-client mods, with a mod channel

The mod runs in every player's `sacred.exe` (everyone has the same mods, checked by the mod-set handshake). The
host's copy is the "authority" by convention. A client sees its own hero and what the engine shows it, so kills
are *reported by the killer's client* instead of observed by the host:

```
 player client --kill--> mod channel ---> host's Gear (counts, stores, decides) ---> grant/say messages
                                                                 ^---------------------------+
 each client grants its own hero (gold/items) when the host's mod asks it to
```

The channel rides the game's own connection (a game message the server already relays; to be chosen once the
chat/relay type is mapped), so it needs no extra ports and works in matchmaker games. A mod that is not installed
on a client simply gets no reports from it (the handshake normally prevents that).

Trust model: this is cooperative, not cheat-proof (a modified client could report false kills). Phase 2 (server
side) can verify.

### Lua API sketch

Everything is under `gear.mp`; in single player `gear.mp.active()` is false and the rest returns empty values.

```lua
---@meta _

-- ## Multiplayer
--: Events and calls for network games. A player is a table valid for the current game; `gear.mp.players()` lists them.

---@class GearPlayer
---@field slot integer         Player slot 1..16 (the same number as the hero's object id).
---@field name string          The hero's name.
---@field class string         The hero's class name.
---@field is_local boolean     This is the player at this computer.
---@field is_host boolean      This player started the game.
---@field key string           Identity for storage: name .. class lower-cased; stable between sessions, not unique across machines.
local GearPlayer = {}

---@return Creature? hero      The player's hero (nil if the engine has not got it).
function GearPlayer:hero() end
---@param text string
function GearPlayer:say(text) end                       -- system message in that player's chat
---@param amount integer
---@return boolean ok
function GearPlayer:give_gold(amount) end               -- routed to that player's client
---@param type integer|string
---@param opts? {count?: integer}
---@return boolean ok
function GearPlayer:give_item(type, opts) end
---@return GearStore
function GearPlayer:store() end                         -- per-player persistent table (see below)

---@class GearStore
---@field get fun(self: GearStore, key: string, default?: any): any
---@field set fun(self: GearStore, key: string, value: any)   -- JSON-able values; written to mods/<id>/players/<key>.json next to the host's saves
---@field add fun(self: GearStore, key: string, delta: number): number

-- path gear.mp
---@class GearMp
local GearMp = {}
---@return boolean
function GearMp.active() end                            -- in a network game
---@return boolean
function GearMp.is_host() end
---@return GearPlayer[]
function GearMp.players() end
---@param slot integer
---@return GearPlayer?
function GearMp.player(slot) end
---@param text string
function GearMp.say(text) end                           -- to all players
---@param channel string                                -- your mod's own channel name
---@param payload table                                 -- JSON-able
---@param to? GearPlayer|'host'|'all'                  -- default 'host'
function GearMp.send(channel, payload, to) end

-- events (gear.on overloads)
-- 'mp_join'   function(player: GearPlayer)            -- host side: a player joined / the game started
-- 'mp_leave'  function(player: GearPlayer)
-- 'kill'      function(ev: {killer: GearPlayer?, victim: Creature, type: integer, type_name: string})
--             runs on the killer's client always; on the host also for kills the host's client sees
-- 'mp_message' function(from: GearPlayer, channel: string, payload: table)
```

The collect-a-thon, in this API:

```lua
gear.on('kill', function(ev)
    if not ev.killer or not ev.killer.is_local then return end
    if ev.type_name == 'Orc' then gear.mp.send('collect', {kind = 'orc'}) end   -- to the host
end)

gear.on('mp_message', function(from, channel, p)                                -- runs on the host
    if channel ~= 'collect' then return end
    local n = from:store():add('orcs', 1)
    if n == 10 and not from:store():get('orc_done') then
        from:store():set('orc_done', true)
        gear.mp.say(from.name .. ' has defeated 10 Orcs!')
        from:give_gold(500)
    end
end)
```

Manifest: new key `side = 'any' | 'host'` (default `'any'`): a `'host'` mod is loaded in the host's game only
(others get a log line, not an error). Keeping UI out of this: anything it shows uses Gear's existing UI, which is
controller-usable already.

### Needed engine work (phase 1)

1. Hook the creature-death path to raise `kill` with the killer (the client knows who dealt the last blow for its own
   hero's kills; other players' kills on the host come from the engine's own messages).
2. Find the chat/system message path (client: UI chat; server relay type) for `say` and the mod channel; the
   channel needs a message type that the unmodified server relays. Candidates are the chat type or an unused one
   in the default-relay group (238 types go to the shared handler).
3. `mp_join` / `mp_leave`: player list changes (the player info object and the player list UI source).
4. Lift `world.can_change()` for the local hero's gold and inventory in network games, because the engine then
   syncs it (the same path the game's own pickup and trade take).
5. Per-player storage: files next to the host's saves, atomic writes (reuse the autosave code).
6. LuaLS annotations in `gear/meta/mp.lua`, `check_gear_api.py` / `gen_gear_api_docs.py` updates, a kernel test
   with the native side mocked, and a small `-gear-auto` script run with two Sacred instances.

## Phase 2: real server mods in GameServer.exe

A headless Sacred Gear flavor loaded into `gameserver.exe` (SacredBild already injects into it with Detours),
without any UI. It hooks the dispatch loop at `0x4C3F20` to see messages with the sender slot, can
verify kills against the server's creature copies (state `+0xFC == 9` etc.) and can send server messages to a
slot. Same `gear.mp` API; `side = 'host'` mods run there. The game has no extra server-side persistence, so
`store()` writes under the server's folder. Open: which message types carry kill credit (killer id) and a chat
line; that needs the type map of the 183 handlers, which is a few hours of work with the decompile and a live
two-client capture.

## Decisions so far (John, 2026-11-16)

- Phase 1 may require the mod on every client.
- Grants: the host **drops the item at the player's feet** (no client-side sync). `player:give_item` spawns the item
  on the ground at the hero's position on the host (the engine broadcasts world objects); `give_gold` drops a gold
  pile (types 0x140C-0x140F) the same way. This replaces "routed to that player's client" above.
- Player key: not answered; the proposal stays at hero name + class lower-cased until John says otherwise.

## Open questions for John (answered ones above)

1. Is "everyone installs the mod" acceptable for phase 1 (client-reported kills), or must it work with unmodded
   clients (then only phase 2)?
2. Player identity: name + class is weak. Should the host assign and remember a GUID per hero in a file the
   client keeps, or is name + class enough for now?
3. Grants through the owning client need the engine's sync path; if that proves unreliable in tests, is
   "host spawns the loot on the ground next to the player" an acceptable fallback?
