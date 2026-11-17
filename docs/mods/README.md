# Example mods: install, hotkeys, settings

Eight client mods in `gear/examples/` are documented here. Five belong to the SacredEngineRemake game server's Lua mods and show
their data in the game; three work alone.

| Mod (`id`) | Needs | What | Hotkey (default) | Doc |
|---|---|---|---|---|
| `bounty` | server `bounty_board.lua` | Bounty board panel and a small tracker | **F8** | [server-mods.md](server-mods.md#bounty--bounty-board) |
| `achievements` | server `achievements.lua` | Toasts and a panel for achievements and the login streak | **F7** | [server-mods.md](server-mods.md#achievements--achievements-and-login-streak) |
| `serverfeed` | server `rare_drops.lua`, `graveyard.lua` | Rare drops and hero deaths as lines on screen, a list of the latest | **F7** | [server-mods.md](server-mods.md#serverfeed--server-feed) |
| `buildsharer` | server `build_sharer.lua` | Publish and browse hero builds | **F7** | [server-mods.md](server-mods.md#buildsharer--build-sharer) |
| `questhelper` | server `quest_helper.lua` (for records) | Quest items near you and the server's quest records | **F8** | [server-mods.md](server-mods.md#questhelper--quest-helper) |
| `sessiontracker` | nothing | Session time, kills, gold, experience, rates | **F2** | [GearSessionMods.md](../GearSessionMods.md) |
| `lootlog` | nothing | Log of drops and pickups | **F3** | [GearSessionMods.md](../GearSessionMods.md) |
| `damagemeter` | nothing | Damage dealt and taken, DPS | **F7** | [GearSessionMods.md](../GearSessionMods.md) |

The server side of the first five (what the scripts do, their pages and chat commands, with screenshots) is documented in the
SacredEngineRemake repository, `docs/mods/README.md`.

## Install

1. Install Sacred Gear itself (see [Gear.md](../Gear.md#install)).
2. Copy the mod's folder from `gear/examples/<id>/` (it holds `mod.lua` and `main.lua`) into the game's **`mods`** folder, so that
   you get `mods\<id>\mod.lua`. One folder per mod. Do not copy mods you do not want: every mod in `mods` loads.
3. Start the game. Press **F11** (or the **Mods** button under the escape menu) to open the Mods screen: it lists the mod, an
   On/Off switch, its **Settings** page and its **Keys** page. Every choice is saved at once in `mods\gear_mods.json`.
4. For the five server mods: play on a server that runs the matching script (and the Sacred Gear game server's custom net
   messages, which are on by default). See below.

## Hotkeys, and the clashes in the defaults

All hotkeys are rebindable in the Mods screen (Keys page of each mod; **Default** restores it). The defaults of these examples
overlap. This is how they stand today:

| Key | Used by (default) |
|---|---|
| F2 | `sessiontracker` |
| F3 | `lootlog` |
| **F7** | `damagemeter`, `achievements`, `serverfeed`, `buildsharer` (four mods) |
| **F8** | `bounty`, `questhelper` (two mods); F8 is also the game's own **quick load** |

What happens with a clash: the Mods screen marks the key as a conflict on both mods' rows and on the Keys page, **both mods still
fire**, so one key opens two panels at once. F8 additionally collides with the game's quick load key (the screen marks binds
that equal one of the game's own hotkeys as a conflict with "The game"). Nothing is changed for you: if you run several of
these mods, rebind all but one on each clashing key to a key that no mod uses and that is not one of the game's own (the game
reacts to its hotkeys with any modifiers held, so `ctrl+b` is still B). The Mods screen shows a conflict marker while a
choice is taken; other examples use F4 (`uidemo`), F5 (`lootfilter`), F6 and F10 (`sacred-gear-mods`) and F11 (the Mods screen).

The game's own hotkeys (digits, Q W E R B, A, I, F, C, L, M, Tab, O, S, H, N, P, Space, Esc, + and -, F8, F9) are listed in
[Gear.md](../Gear.md#mod-manager).

## Settings

Declared in each mod's `mod.lua` and shown on its Settings page of the Mods screen (stored in `mods\gear_mods.json`); values
without a screen (positions, refresh seconds) are in `config` of `mod.lua` and can be edited in that file. See the doc of each
mod.

## When there is no server

The five server mods need a server that talks custom net messages (`gear.net`): the SacredEngineRemake game server with the
script. In single player, on the original `GameServer.exe` (it drops the handshake) or on a server without the script they stay
idle; the panels of `bounty`, `achievements`, `buildsharer` and `questhelper` say so ("This server has no ..."). `serverfeed` shows
nothing.

## Screenshots

The client panels are drawn inside the game, so they need the game running; **no screenshots of them exist yet**, and none are
faked here. Each mod's doc describes the layout from its code. The matching web pages of the server are pictured in the server
repository's docs.
