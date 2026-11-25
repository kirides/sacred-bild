# Sacred Gear in network games

How mods work in LAN and online games: what Sacred's multiplayer does, the messages a mod can rely on, the mod
channel (`gear.mp`), named channels (`gear.net`) and hooks between mods (`gear.events`). The calls themselves are in
the [reference](GearApi.md#multiplayer-gearmp). Players' documentation of the mods built on this (bounty board,
achievements, server feed, build sharer, quest helper) is in
[sacred-gear-mods](https://github.com/kirides/sacred-gear-mods/blob/main/docs/ServerMods.md); their server halves are
Lua mods of the SacredEngineRemake game server (`Sacred.GameServer/scripts-examples`, see its `README.md` and
`docs/protocol.md`).

## What Sacred's multiplayer does

Addresses are `GameServer.exe`'s (decompile: `B:\DevDrive\sources\decomp-gameserver`) unless noted.

- **A separate server process.** Hosting a game starts `GameServer.exe` (no window). The host's own `sacred.exe`
  joins it as an ordinary client, like every other player.
- **Players.** 16 slots. Per-player records at `net + 0x1B0 + slot * 0x518` ("active" flag `+9`, connection id
  `+0x90`, data-flow setting). Slot N owns hero object id N (the object manager's ids 1..16 are the heroes), so
  slot = hero object id = a per-session player id. Nothing in the protocol is a stable account id: across sessions a
  player is only the hero (name, class) they join with.
- **The server is not a pure relay.** Its main loop `0x4C3F20` dequeues messages (0x10-byte queue entries; the
  message type is the u16 at `+0xE`) and dispatches through a jump table (`0x4CC9C0`) on the type (3..0x1D5): 183
  handlers and one shared default (relay or ignore) for the other 238 types. It keeps real creature objects for the
  players' heroes (hero death at `0x4C3F20`, the `*puVar13 == 7` case: state `+0xFC = 9`, `+0x150 = 6`, health 0;
  experience, gold and level at `+0x3B4 / +0x3EE / +0x3FE`) and tracks object ownership (`setObjectOwner` `0x4D6950`,
  message 0x171).
- **Who simulates what.** Creatures are simulated by an owner client (owner slot at `+0x39`; ISDN players never own
  ambient NPCs). Experience, gold and death are decided by the owner's `sacred.exe` and sent as messages; the server
  mirrors them. A kill is visible on the killer's client, on the owner's client and in the server's object copy.
- **Grants.** A client changes its own hero's gold and inventory and the engine broadcasts it. Gear's world and
  inventory calls follow that: in a network game only the local hero's gold, backpack and the chest may be changed
  (see [RE notes](RE_NOTES.md#items-inventories-and-pickup)); spawning stays single player.

## Messages

Message ids use the server's numbering (the type at header `+0xE`). The first dword of a message header holds the
sender slot in bits 27..31 (1..16 a player, 0x11 the server). Confirmed with net traces of the client
(`GEAR_NETTRACE=1`):

| Message | Layout |
|---|---|
| `0x005` public chat (`0x004` team, `0x003` private) | 16-byte header, 4 zero bytes, UTF-16 text in a 256-byte area (at most 127 characters); the speaker is the header's slot, slot 0 is the server (a system message) |
| `0x032` long chat | u32 speaker, i32 recipient, 256-byte UTF-16 name area at +8, text at +264 |
| `0x17C` attack, then `0x127` and `0x17D` | a kill: `0x127` `{hero id, experience award}` and `0x17D` `{victim id, 1, 1}`, both sent by the killer's client within a millisecond. The award's hero is the killer; a creature killed by a creature produces no award |
| `0x128` | health `{id, signed hp}` |
| `0x141` then `0x13F` | the local hero picks up an item: `{item id, float}`, then `{item id, 0x8F, 1}` |
| `0x12` | right after joining: UTF-16 text (probably the join password) |

[SacredEngineRemake](https://github.com/Aytackydln/SacredEngineRemake) (`Sacred.Network`; no licence: facts only, no
copied code) has a tested catalog of the ids and layouts (`Sacred.Network.Tests/Contracts/PacketCatalog.cs`, codecs in
`Sacred.Network/Gameplay`, framing in `Protocol/GoldMessageCodec.cs`), among them damage `0x17A`, item pickup `0x143`,
local inventory insert `0x145`, trade `0x198`, the counter HUD message `0x19E`, quest progress and insert
`0x1B8` / `0x1B9`, game settings `0x00C` and the session notice `0x002`.

## The mod channel (gear.mp)

A mod runs in every player's `sacred.exe`; a mod meant for network games has to be installed on every client. A
client sees its own hero and what the engine shows it, so kills are reported by the killer's client (the `kill`
event runs there) rather than observed by the host. The host's copy of a mod is the authority by convention. This is
cooperative, not cheat-proof: a modified client can report anything, so a mod checks what it receives. A game server
that runs its own Lua mods (SacredEngineRemake) can verify, keep per-player data and grant items and gold
(`server.drop_gold`, `server.drop_item`: the item drops at the player's feet).

`gear.mp.send` messages travel as frames of the game's private chat, which every game server relays and players
never see. The player's identity for storage is `GearPlayer.key` (the hero's name and class); treat it as opaque, it
will become a realm account id.

## Named channels (gear.net)

Channels carry messages between Gear clients and the game server's scripts. They always work on the chat carrier,
and upgrade to two message types of their own where the server agrees.

**On chat.** Private chat frames (kinds `n` bytes, `k` JSON; channel names of 1 to 64 printable ASCII characters) to
any player, and to the game server as private chat to slot 0 once the server has shown it speaks them. Limits: 64 KB
per message, about 8 KB/s from the client and 16 KB/s from the server (`NETOPS_CHAT_RATE`), 127 units per frame. With
the server's flood protection on, chat lines are limited per player (`FLOOD_CHAT_PER_SECOND`), which limits the chat
carrier too. `server_only` channels are not relayed between players.

**The upgrade, negotiated on chat:**

1. On entering the game the client announces itself to the server: chat frame `h` (name `gear.net`, body `[proto]`),
   every 5 s up to 6 times until the server queries. A server that does not know it ignores it (the original
   `GameServer.exe` does); a server never writes chat frames to a player that did not announce itself, because a plain
   client would show them.
2. The server queries (`q`): `[proto][u16 control][u16 data]`, the numbers it offers (`NETOPS_CONTROL` /
   `NETOPS_DATA`).
3. The client answers (`a`): `[proto][u16 control][u16 data][u8 flags: 1 accepts]`. It accepts valid numbers (above
   0x1D5, below 0x400, different) unless `gear.net.configure` restricted them, and never sends an unsolicited custom
   type.
4. After an accept the client sends HELLO on the agreed control type; HELLO_ACK carries the limits (fragment 4000
   bytes, message 256 KB, 64 KB/s, 8 messages in flight) and the data type, PEERS lists the capable players. From then
   on channels to capable players use data frames.
5. Failure, decline, a 3 s timeout or a server that never queries: channels stay on chat. Messages to players always
   have the chat path; messages to `'server'` need a server that queried.

**Custom message types.** Default 0x1E0 (control) and 0x1E1 (data). The client's dispatcher sends every type above
0x1D5 to "next message" (`cmp ecx, 0x1D4 / ja`); `NetMsg` (`src/gear/netmsg.*`) redirects that `ja` to a stub that queues
frames of the two configured types. Sending is `cNet_sendMessage` with the type. The original `GameServer.exe` accepts
and silently drops both (a 4032-byte message too); the connection, the hero and the chat carrier are unaffected.
Other ids: server `NETOPS_CONTROL` / `NETOPS_DATA` (cfg, command line, environment); client `gear.net.configure` before
connecting or `GEAR_NETOPS="0x1E0,0x1E1"` (the data id is also taken from HELLO_ACK).

**Data frame:** `[u32 FNV-1a(channel)][u8 flags: 1 first, 2 last, 4 JSON][u8 target: 0 server, 1 host, 0xFF all,
0x10+slot][u16 id][bytes]`.

`gear.net.transport(to)` / `channel:transport(to)` say which way a message would go (`'custom'`, `'chat'`, `'mixed'`,
nil).

**Self test:** `src/gear/examples/netopstest` (`expect = 'stock'` or `'remake'`; the remake run needs `tools/gs-launcher`
and the server's `scripts-examples/net_echo.lua`; `chat_only = true` makes the client decline the offered numbers);
kernel tests in `src/gear/tests/kernel_test.lua`.

## Hooks between mods (gear.events)

Mods publish hooks (`bounty.completed`, `worldevent.started`) with `gear.emit` after declaring them with
`gear.events.declare`; other mods subscribe with `gear.on(name, fn)` or `gear.on('bounty.*', fn)`
([reference](GearApi.md#hooks-between-mods-gearevents)).

- **Clients** (single player and network): every computer's mods emit for what happens on it, for its own hero.
  Nothing crosses the network by itself; a mod that wants the host or another player to hear a hook forwards it on its
  own channel.
- **Game server:** its Lua host offers the same `gear.emit`, `gear.on` and `gear.events.declare` / `list`, with the same
  name rules and payload shapes, so a server-side mod and a client mod publish identical events. Server events carry an
  extra `player` field (the slot's hero name), because the server sees every player. The kernel's code for this (`EV` in
  `src/gear/lua/kernel.lua`, about 50 lines of plain Lua) ports unchanged.

## What a server can know about a hero

For server mods that show hero data (the build sharer, the quest helper): the server fills class, level, skills,
combat arts and equipment from its own view of the hero (`server.hero`, which needs the web UI or `--authoritative`);
what a client reports is marked as reported. A hero file's
only quest state is a list of `[key][value]` records (the "quest pairs" of the inventory stream); the quest journal,
active quests, their texts, map markers and progress are state of the game's script interpreter: not in a hero file,
on no message the server sees, and not in Gear's API. Which key is which quest is not mapped.
