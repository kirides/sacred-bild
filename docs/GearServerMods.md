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
- **Chat / messages to a client (wire facts, from the SacredEngineRemake catalog, see below):** public chat is
  message `0x005`, team `0x004`, private `0x003`: four skipped bytes, then NUL-terminated UTF-16 text, the speaker is
  the message header's slot and **slot 0 is the server** (a system message). `0x032` is the long form (u32 speaker,
  i32 recipient, 256-byte UTF-16 name area at +8, text at +264).
- **Kills:** the death command is `0x17D` (u32 object id of the dying creature, u32 "player word"), health `0x128`
  (id, signed hp), and the experience award `0x127` (hero object id, signed amount) reaches the killer. The death
  message has no killer id we know of; credit comes from the matching `0x127` for a hero right after the death.
  **Confirmed by a net trace (2026-11-16, client sends, `GEAR_NETTRACE=1`):** a kill is `0x17C` (attack), then within
  a millisecond `0x127` with payload `{hero id (u32), exp award (u32)}` (hero 1 got 2, 3, 2) and `0x17D` with
  `{victim object id, 1, 1}` (victims 0x3BA, 0x3BC, 0x3B8), both sent by the killer's client. So a client knows its
  own kills exactly: the award's hero id is the killer, the death's id the victim (look the victim up for its type
  before it is retired). A creature killed by another creature produces no award. The first dword of a message
  header has the sender slot in bits 27..31 (the trace showed 33 = slot 1 with flag bits 22..26 = 1); the earlier
  note "slot bits <<22" in the Gear trace code was wrong and is fixed.
- **Chat confirmed (net trace, 2026-11-16):** typing "asdf" and "Hello ..." in game sent message `0x005`, 276 bytes:
  16-byte header, 4 zero bytes, then UTF-16 text in a 256-byte area (so the text is at payload +4, at most 127
  characters). Item pickup by the local hero is `0x141` {item id, float} followed by `0x13F` {item id, 0x8F, 1}.
  `0x12` right after joining carries the UTF-16 text "1234" (probably the join password).
- **Grants:** a client changes its own hero's gold/inventory locally and the engine broadcasts it; Gear's
  `add_gold`, inventory calls and spawn work this way but are refused in network games (`world.can_change()`).
- **Gear today:** only runs in `sacred.exe` (refuses `gameserver.exe`); mods have no multiplayer support, and
  there is a planned handshake that checks that all peers run the same mod set (`mod set` hash).

## Wire facts from SacredEngineRemake

[SacredEngineRemake](https://github.com/Aytackydln/SacredEngineRemake) (`Sacred.Network`, C#, no licence: facts only,
no copied code) has a tested catalog of Sacred Gold's message ids and layouts
(`Sacred.Network.Tests/Contracts/PacketCatalog.cs`, codecs in `Sacred.Network/Gameplay`, framing in
`Protocol/GoldMessageCodec.cs`). Useful for us: chat `0x003/0x004/0x005/0x032`, death `0x17D`, experience `0x127`,
health `0x128`, damage `0x17A`, attack `0x17C`, item pickup `0x143`, local inventory insert `0x145`, trade `0x198`,
counter HUD message `0x19E`, quest progress/insert `0x1B8/0x1B9`, game settings `0x00C`, session notice `0x002`.
Its ids use the same numbering as the server's dispatch table (message type at header +0xE).

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
- Player key: hero name + class lower-cased is fine for now.
- Later idea: hook into the game's "online play" register/login and add a mod that lets players choose a "Realm" at
  runtime. Not designed yet; the player key would then become a realm account id, so keep `GearPlayer.key` an
  opaque string that mods must not parse.

## Open questions for John (answered ones above)

1. Is "everyone installs the mod" acceptable for phase 1 (client-reported kills), or must it work with unmodded
   clients (then only phase 2)?
2. Player identity: name + class is weak. Should the host assign and remember a GUID per hero in a file the
   client keeps, or is name + class enough for now?
3. Grants through the owning client need the engine's sync path; if that proves unreliable in tests, is
   "host spawns the loot on the ground next to the player" an acceptable fallback?

## Built: custom net messages (gear.net), 2026-11-17

The chat carrier above stays (it works with every server). On top of it, two message types above the stock range carry
named channels directly between Gear clients and the server's Lua scripts. Reference: `gear/meta/net.lua` (`docs/GearApi.md`),
server side: SacredEngineRemake `Sacred.GameServer/README.md` ("Custom net messages for scripts") and `docs/protocol.md`.

- **Stock client delivers them.** The dispatcher's range check (`cmp ecx, 0x1D4 / ja`) sends every type above 0x1D5 to "next
  message"; `NetMsg` redirects that `ja` to a stub that queues frames of the two configured types (default 0x1E0 control,
  0x1E1 data). Sending is `cNet_sendMessage` with the type. Verified in a real LAN game: the original GameServer.exe accepts
  and silently drops both (a 4032 byte message too), the connection, the hero and the chat carrier stay fine; the remake
  server's HELLO_ACK and PEERS reach the hook.
- **Handshake.** HELLO after the hero joined (before admission the server would drop the connection); HELLO_ACK carries the
  limits (fragment 4000, message 256 KB, 64 KB/s, 8 messages in flight) and the data type; PEERS lists the capable players.
  No answer in 3 s = unsupported (original server): `gear.net.supported()` stays false, `gear.mp.send` keeps using chat.
- **API.** `gear.net.channel(name):on(fn)` / `:send(payload, to)`, `supported()`, `peer_supported(slot)`, `limits()`,
  `configure({control, data})`, event `net_caps`. `gear.mp.send` uses an internal channel (`gear.mp`) for capable players.
- **Other opcode ids.** Server: `NETOPS_CONTROL` / `NETOPS_DATA` (cfg, command line, environment). Client: `gear.net.configure`
  before connecting, or `GEAR_NETOPS="0x1E0,0x1E1"`; the data id is also taken from HELLO_ACK.
- **Data frame:** `[u32 FNV-1a(channel)][u8 flags: 1 first, 2 last, 4 JSON][u8 target: 0 server, 1 host, 0xFF all, 0x10+slot][u16 id][bytes]`.
- **Self test:** `gear/examples/netopstest` (`expect = 'stock'` or `'remake'`; the remake run needs `tools/gs-launcher` and
  the server's `scripts-examples/net_echo.lua`); kernel tests in `gear/tests/kernel_test.lua`.

### Net channels on chat first (2026-11-17, follow-up)

`gear.net` channels no longer need the custom message types. Their messages ride on the chat carrier (private chat frames, kinds
`n` bytes / `k` JSON, channel names of 1 to 64 printable ASCII characters) to any player, and to the game server as private chat to
slot 0, once the server has shown it speaks them. The custom types (above) are an upgrade negotiated **on chat**:

1. The client announces itself to the server on entering the game: chat frame `h` (name `gear.net`, body `[proto]`), repeated every
   5 s up to 6 times until the server queries. A server that does not know it ignores it (the original GameServer.exe: checked, no
   effect); a server never writes chat frames to a player that did not announce itself, because a plain client would show them.
2. The server queries (`q`): `[proto][u16 control][u16 data]`, the numbers it offers (its `NETOPS_CONTROL` / `NETOPS_DATA`).
3. The client answers (`a`): `[proto][u16 control][u16 data][u8 flags: 1 accepts]`. It accepts valid numbers (above 0x1D5, below
   0x400, different), unless `gear.net.configure` restricted them to others; it never sends an unsolicited custom type.
4. After an accept the client sends HELLO on the agreed control type once its answer has left; the server ignores a HELLO without
   an agreement. HELLO_ACK (limits) and PEERS complete it; from then on channels to capable players use data frames.
5. Failure, decline, timeout (3 s) or a server that never queries: channels stay on chat. Messages to players always have the chat
   path; messages to `'server'` need a server that queried.

`gear.net.transport(to)` / `channel:transport(to)` say which way a message would go (`'custom'`, `'chat'`, `'mixed'`, nil). Chat caps
(client and server): 64 KB per message, about 8 KB/s from the client, 16 KB/s from the server (`NETOPS_CHAT_RATE`), 127 units per
frame. `server_only` channels are not relayed between players on chat either (the server reads the channel name from the first
frame). With the server's flood protection on, chat lines are limited per player (`FLOOD_CHAT_PER_SECOND`), which also limits the
chat carrier. Self test: `netopstest` with `chat_only = true` (the client declines the offered numbers).
- **Example: achievements and login streak.** Server `scripts-examples/achievements.lua` (SacredEngineRemake) tracks achievements and a daily
  login streak per hero and pays rewards with `server.drop_gold` / `server.drop_item`; `gear/examples/achievements` shows toasts and a panel (F7).
  Wire: channel `achievements:sync`; server to client JSON `{op='state', ach={{id,name,desc,have,need,done?,reward?}...}, streak, best, days, claimed, next}`,
  `{op='unlock', id, name, desc, reward}`, `{op='streak', streak, best, reward}`; client to server `{op='get'}`.
## Statistics mods and the server feed

Five Lua mods of the SacredEngineRemake game server (`Sacred.GameServer/scripts-examples`) keep statistics in their own
storage and serve a page and JSON endpoints each (public ones under `/public/mods/<mod>/` with `WebUi:PublicApi`):
`server_stats` (players, peak, uptime, totals, 24 h charts), `hall_of_fame` (records, firsts), `seasons` (ladders per season with
an archive), `rare_drops` (live feed of rare and unique drops) and `graveyard` (recent deaths with killer names). The server got
one new script event for the feed, `server.on("drop", function(item, reporter) ... end)`: the game has no drop message, so it
fires when a client reports a new item with a position on the ground (see the server's README for what that does and does not
cover). `rare_drops` and `graveyard` also send each new drop or death on the net channels `rare_drops:feed` and `graveyard:feed`
(table payloads), and answer any message on them with the latest five (`kind = 'recent'`).

`gear/examples/serverfeed` is the client side: it listens on both channels with `gear.net.channel(name):on`, shows each live
message as a coloured line (item rarity colours as in the game) for a few seconds, keeps the latest twelve in a list (F7) and
asks for the latest five when the handshake is done. Settings: drops on/off, deaths on/off, seconds on screen, position. A
server without the scripts never sends on those channels, and a stock `GameServer.exe` drops the HELLO, so the mod stays idle.
Not tested in a running game (see the commit message); the file syntax and the handlers were run against a stub of `gear`.
