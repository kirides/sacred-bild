# How Sacred builds, draws, and routes input for its UI

Analysis of `Sacred.exe` (Gold, GOG, English, 11,919,360 bytes, image base 0x400000). All addresses are virtual
addresses in that build. Names are the ones used in `src/sacred/ui.h` / `docs/RE_NOTES.md` where they exist, else the
class names from the exe's RTTI, else Ghidra's `FUN_<va>`.

Goal of this document: (a) tell Sacred Gear mods where to hook the existing UI and how to line their own UI up with it;
(b) give Sacred Bild a source of truth for "which UI is open, drawn, interactive, focused" so it can drop heuristics.

Confidence markers: **[V]** read directly in the decompile, **[I]** inferred from several call sites, **[?]** not
verified (listed again in the last section).

## 0. Method and a gap in the existing decompile

`B:\DevDrive\sources\decomp-sacred` was exported with Ghidra's default analysis. Ghidra never turned **code that is only
reached through a vtable** into functions, nor about 1 MB of aligned code in gaps between functions. For the UI this
meant that the manager's `receiveEvent` (0x757840), almost every window's `render`/`receiveEvent`/`show`, the escape
handler, etc. were absent from `src/`.

What was added (additive, the Ghidra project was opened `-readOnly`):

| Path in decomp-sacred | Content |
|---|---|
| `tools/ExportExtra.java` | Creates functions at vtable targets and 16-aligned code in gaps, decompiles them |
| `src_extra/<va>>16>/<va>_FUN_<va>.c` | 4,269 more functions (1,082 vtable targets, 3,187 gap functions), 0 failures |
| `index/functions_extra.csv` | Index of those (`name,va,size,reason,status,source_file`) |
| `index/vtables.csv` | Every RTTI class: `class, vtable_va, this_offset, slot, target, in_original_export` (2,977 slots) |
| `index/vtable_targets_missing.txt`, `index/code_gaps_original.txt` | Inputs for the above |

Look a function up in `src/` first, then `src_extra/`. Decompiler output for the new functions is thiscall-annotated
but, as everywhere in this decompile, the `this` pointer is often shown as an unnamed first parameter or missing.

## 1. Architecture in one page

* **Event bus** (`cKernel`-style singleton at `DAT_0182ed44`, 0x70 bytes, getter `0x808E50`, post `0x8092F0`). Every
  input and UI command is a `cEvent` object posted to it. `cEngine_receiveEvent` (`0x618090`) hands mouse events to
  the UI manager first, then to the world mouse handler [V per RE_NOTES].
* **`cUI_Manager`** (single instance, pointer at `0x17EEBBC`, vtable `0x8977D0`, allocated 0x2F4 bytes, ctor
  `0x756A90`). It owns the windows, the popups and a dialog, and has **three modes** (see flags): cinematic, menus,
  in game. vtable slot 0 **is** `receiveEvent` (`0x757840`), slot 1 `0x75E470`, slot 2 `0x7577E0`.
* **Windows** are `cUI_Window2` subclasses (`cUI_Control2` base, vtable `0x897488`). The in-game ones live in a fixed
  array of 23 pointers at manager `+0x80..+0xD8`, created once per game by `createGameWindows` (`0x759AF0`) and
  destroyed on return to the menus (`0x75A250`, called from the manager event with subtype 3 via `0x7598E0`). Menu
  screens live at `+0x130..+0x140` (built by `0x75A480`).
* **Window state is a flag word at control `+0x10`**, changed by named **UI events** (kind 5, section 5). A window is
  "open" when bit 0 is set. There is no z-order list: **draw order and event order are hard-coded call sequences** in
  `render` (`0x758ED0`) and `receiveEvent` (`0x757840`), different per mode, and some windows are drawn *alone*.
* Everything is laid out in a fixed 1024x768 space. Sacred Bild maps that into a canvas (see `RE_NOTES.md`,
  "SacredBild UI canvas"); nothing below depends on it.

## 2. `cUI_Manager` layout

Offsets from the manager pointer [V unless marked].

| Offset | Meaning |
|---|---|
| `+0x00` | vtable (`0x8977D0`) |
| `+0x04` | **cursor tooltip control** (created by `0x72FA00`, 0x158 bytes). Rendered last, with control flag `0x8000` forced on around the call (`0x758ED0` tail) **[I]** |
| `+0x08` | **mode/state flags**, table below |
| `+0x0C` | UI thread handle (menu thread `0x756A50`, `_beginthread`-style via `0x84BB9D`) |
| `+0x28` (byte) | thread control: bit 0 running, bit 1 stopped |
| `+0x54` | u16 index into `popups` (a reserved popup) **[I]** |
| `+0x56` | u16[16] indices into `popups` used by the help screen |
| `+0x78` | float, time of the last cinematic fade step |
| `+0x7C` | float, set by event subtype `0x28` |
| `+0x80..+0xD8` | in-game windows, section 3 |
| `+0x110..+0x120` | pending-trade drag rect (`+0x110` object ptr, `+0x114/+0x118` x/y, `+0x11C/+0x11E` w/h); event path logs `executeAction(TRADE_ACCEPT)` when the cursor leaves it **[I]** |
| `+0x124/+0x128/+0x12C` | `std::vector<cUI_Popup*>` (tooltips, hints, NPC dialog). Entry type is `cUI_Tooltip` (vtable `0x896B3C`) |
| `+0x130..+0x140` | menu screens: `[0]` `cUI_MainMenu` ("UI_MAINMENU", ctor `0x7119B0`), `[1]` `cUI_Character` (character select, `0x70C1D0`), `[2]` `cUI_Options` (`0x716D30`), `[3]` `cUI_Savegame` (`0x71BD40`, load; `+0x154` byte = 0), `[4]` `cUI_Network` (`0x70B1D0`, size 0x178) |
| `+0x144` | **modal message box** `cUI_BusyDlg` (ctor `0x720540`, 0x644 bytes), exists in every mode |
| `+0x148` | **keyboard focus**: pointer to the focused control, written by the FOCUS event (section 8). Not in `ui.h` today |
| `+0x14C` | **embedded** `cUI_TextLines` ("UI_WND_TEXTLINES", ctor `0x722D80`, rect 0,0,1024,768): the scrolling game/system message lines. It is an object *inside* the manager, not a pointer. `ui.h` calls this `other` and types it as a pointer; its first dword is really the vtable |
| `+0x2E0` | dword, set to 1 by event subtype `0x3E` |

### Manager flags (`+0x08`)

| Bit | Meaning |
|---|---|
| `0x001` | menus mode (menu screens are drawn and get events) |
| `0x002` | menu windows (`+0x130..`) have been created |
| `0x004` | in game |
| `0x008` | in-game windows exist (set at the end of `createGameWindows`, cleared by `0x75A250`; **window pointers are null/dangling while it is clear**) |
| `0x010` | cinematic mode (letterbox bars, only the game menu gets events) |
| `0x020` | cinematic fade-out phase (bar height ramps back down) |
| `0x040` | game UI active. Set at the *start* of `createGameWindows`; the in-game branch of `render` and the dialog draw require it **[I]** |
| `0x080` | input blocked: mouse and key events (kinds 1-4) are swallowed |
| `0x100` | popups are not drawn |
| `0x200` | help screen (H) is shown. Esc clears it first |
| `0x400`, `0x800` | the two hint overlays ("[H] shows ...", "[TAB] ..."); `0x400` also makes the popup pass draw only the 16 help popups. Cleared by the next key or mouse-down |
| `0x1000` | toggled by the **T** key. Consumer not traced **[?]** |
| `0x808` | value OR-ed in by `createGameWindows` (so `0x800` is also a "created" marker) |

Two globals gate things around it: `DAT_009EC530` (>0: the manager swallows **every** event and `render` decrements it
once per frame, i.e. a frame-counted input hold-off after state changes) and `DAT_0182EE38 & 0x20000` (engine paused:
`render` returns immediately, set by `WM_ACTIVATEAPP`).

## 3. The in-game window table

Created in this order by `0x759AF0`. `rect` is the constructor argument (x, y, w, h in the 1024x768 space) as read from
the call; several constructors move the window afterwards (marked *moved*, values from `RE_NOTES.md`/`ui.h`).
The `Window` enum in `ui.h` has two unnamed slots; they are **purchase** (`+0xB4`) and **horse** (`+0xC4`).

| Off | `Window` enum | Control name (event target) | Class | Ctor | rect | Draws | Notes |
|---|---|---|---|---|---|---|---|
| `+0x80` | taskbar | `UI_WND_TASKBAR` | `cUI_Taskbar2` | `6DF590` | 0,676,1024,92 | `render` | always visible in HUD mode |
| `+0x84` | inventory | `UI_WND_INVENTORY` | `cUI_Inventory3` | `6C5930` | *moved* 0,388,640,256 | `render` | pages: backpack / combat arts / combos |
| `+0x88` | equipment | `UI_WND_EQUIPMENT` | `cUI_Equipment` | `6B6410` | *moved* 656,388,256,256 | `render` | |
| `+0x8C` | blacksmith | `UI_WND_BLACKSMITH` | `cUI_Blacksmith` | `6A1FD0` | 0,0,640,352 | `render2(dev,0)` | |
| `+0x90` | merchant | `UI_WND_MERCHANT` | `cUI_Merchant` | `6DB9B0` | 0,0,640,336 | `render2(dev,0,0)` | |
| `+0x94` | megamap | `UI_WND_MEGAMAP` | `cUI_Megamap` | `6C7540` | 0,0,1024,768 | **alone** | key M |
| `+0x98` | overviewMap | `UI_WND_OVERVIEWMAP` | `cUI_OverviewMap` | `6C6AA0` | 162,120,700,525 | `render` | key Tab, overlay on the world |
| `+0x9C` | minimap | `UI_WND_MERC` | `cUI_Mercenary` | `6D6430` | 932,0,92,676 | `render` | minimap + party portraits |
| `+0xA0` | stats | `UI_WND_STATS_CHAR` | `cUI_StatisticsChar` | `6A5D70` | *moved* 656,0,256,420 | `render` | key C |
| `+0xA4` | console | `UI_WND_CONSOLE` | `cUI_Console` | `6AD9A0` | 256,640,512,128 | `render` | chat/debug input, **text focus** |
| `+0xA8` | questbook | `UI_WND_QUESTBOOK` | `cUI_Diary` | `6AE880` | 64,64,832,576 | `render2(dev,0,0)` | key L |
| `+0xAC` | savegame | `UI_WND_SAVEGAME` | `cUI_Savegame` | `71BD40` | 0,0,1023,767 | **alone** | key S via `0x75ADB0`; `+0x154`=1 in game |
| `+0xB0` | escapeMenu | `UI_WND_ESCMENU` | `cUI_EscMenu` | `6BB160` | 256,240,512,272 | always, after the rest | Esc |
| `+0xB4` | **purchase** | `UI_WND_PURCHASE` | `cUI_Purchase` | `6DCDD0` | ? | `render2(dev,0,0)` | buy/sell confirmation |
| `+0xB8` | master | `UI_WND_MASTER` | `cUI_Master` | `6D1070` | 0,0,640,336 | `render` | combat art master; also rune exchange |
| `+0xBC` | options | `UI_WND_OPTIONS` | `cUI_Options` | `716D30` | 0,0,1023,767 | **alone** | key O |
| `+0xC0` | chest | `UI_WND_CHEST` | `cUI_Chest` | `6ACB40` | 0,0,640,320 | `render` | |
| `+0xC4` | **horse** | `UI_WND_HORSE` | `cUI_Horse` | `6BD2B0` | ? | `render2(dev,0,0)` | stable/horse window |
| `+0xC8` | networkInfo | `UI_WND_NETINFO` | `cUI_NetworkInfo` | `6F6CC0` | ? | `render` | key N (needs the add-on flag `0x182EBEC`) |
| `+0xCC` | netPortraits | `UI_WND_NETPORTRAIT` | `cUI_NetPortrait` | `6D86C0` | 0,0,896,64 | `render` | |
| `+0xD0` | character | `UI_CHARACTER` | `cUI_Character` | `70C1D0` | full screen | **alone** | character export in game |
| `+0xD4` | cube | `UI_WND_CUBE` | `cUI_Cube` | `6E89D0` | 0,0,640,320 | `render` | |
| `+0xD8` | trade | `UI_WND_TRADE` | `cUI_Trade` | `6E90F0` | 0,0,640,320 | `render` | |

Control name = `char[0x20]` at control `+0x30`. These are the strings the game's own events use (section 5), so they
are the stable identifiers for a mod: `UI_WND_INVENTORY`, `UI_WND_MEGAMAP`, ...

### Common control layout (`cUI_Control2`, 0x78 bytes; windows add a child vector at `+0x78`)

`+0x10` flags, `+0x24/+0x28` x/y (relative to `parent` at `+0x50`; windows are top-level, so absolute), `+0x2C/+0x2E`
width/height, `+0x30` name. Flag bits seen in the UI code:

| Bit | Meaning |
|---|---|
| `0x00000001` | **visible/open** |
| `0x00000004` | enabled (takes clicks); event subtype `0x11` with bit 4 in its mask is even accepted by a disabled control |
| `0x00000010`, `0x00000020` | "exclusive/modal" class bits. Setting/clearing either also clears `0x00020000` and `0x20000000` and (if visible) calls `show(false)` first. Meaning **[?]** |
| `0x00008000` | **has keyboard focus** (section 8) |
| `0x00020000`, `0x20000000` | paired with the above **[?]** |
| `0x00400000` | blink (level-up button) |
| `0x00800000` | read-only edit control |

Window vtable (slot = byte offset / 4), common to nearly every window class (same functions shared) [V from
`index/vtables.csv`]:

| Slot (off) | Role | Typical target |
|---|---|---|
| 0 (`+0x00`) | deleting dtor | per class |
| 4 (`+0x10`) | `receiveEvent(cEvent*) -> bool` | per class; default control `0x731CE0` |
| 5 (`+0x14`) | `render(device)` | `0x7277B0` (draw children if visible) or per class |
| 7 (`+0x1C`) | `isInside(x,y)` | `0x732420` |
| 9 (`+0x24`) | **`show(bool)`** | generic `0x726E90` (on hide: posts a FOCUS-release event); `6A41F0` etc. per class |
| 12 (`+0x30`) | per-frame update | `0x727110` (no-op) or per class |
| 13/14 (`+0x34/+0x38`) | Esc / Enter | `0x724B60` / `0x724B50` default |
| 17 (`+0x44`) | `render2(device, a, b)` | blacksmith, merchant, questbook, purchase, horse, options, character, ... |

## 4. Modes and the three event/draw paths

The mode is read from manager flags in this priority: **cinematic (`0x10`) > menus (`0x01`) > in game (`0x04`)**
(both in `render` and in `receiveEvent`).

## 5. Opening and closing windows: named UI events [V]

All window visibility changes go through **kind 5 events** (`cEventUI2`, vtable `0x890DD0`, built on a `cEvent`
`0x890890`) posted to the bus (or passed straight to `manager->receiveEvent`):

```
+0x00 vtable      cEventUI2 0x890DD0
+0x04 subtype     0x11 set flags | 0x12 clear flags | 0x13 toggle flags | 0x10 focus | others (below)
+0x08 mask        control flags to set/clear/toggle (5 = visible|enabled for a normal open)
+0x0C             FOCUS: pointer of the new focus control
+0x28 char[0x20]  sender name   (the game uses "<engineevent>", "<WindowProc_engine>" or "")
+0x48 char[0x20]  TARGET control name, e.g. "UI_WND_MEGAMAP"
```

The receiving side is the default `cUI_Control2::receiveEvent` (`0x731CE0`): if the event's target name equals the
control's own `+0x30` name, it applies the subtype to `+0x10`, calls the vtable `show(bool)` when bit 0 changes, and
returns true. Each window's own `receiveEvent` falls back to it. `setFlags` = `0x732550`, `clearFlags` = `0x7325C0`
(both call `show` on a visibility change). Evidence of use: the hotkey code in `0x757840` builds exactly these events
for `UI_WND_QUESTBOOK`, `UI_WND_MEGAMAP`, `UI_WND_NETINFO`, `UI_WND_OPTIONS`, `UI_WND_SAVEGAME`, `UI_WND_CONSOLE`;
`0x7572C0` does it for `UI_WND_ESCMENU`, `UI_WND_INVENTORY`, `UI_WND_QUESTBOOK`, `UI_WND_MEGAMAP`.

Helpers: `0x756710` (open inventory, subtype `0x11`) and `0x756870` (close inventory, subtype `0x12`; first calls
`0x756F40` and `0x75AA30`), exposed as vtable slot 1 of `cCommand_onInvOpen` / `cCommand_onInvClose` (`0x896B2C` /
`0x896320`) so scripts/other code can call them as commands.

**Hooking consequences:**

* To open/close a window from a mod: build a `cEventUI2` with subtype `0x11`/`0x12`, target name, mask 5, and call
  `cUI_Manager` vtable slot 0 (or post it). This is what the hotkeys do, so it stays consistent with game state.
* To be told when windows open/close: patch slot 9 (`show`) of the window vtables (the same technique `ui_anchor.cpp`
  already uses for slots 4/5/7/9/17), or hook `0x732550`/`0x7325C0`/`0x731CE0`. Caveat: for *open*, `show(1)` is called
  **before** bit 0 is set (`0x731CE0`, subtype `0x11`); for *close* it is called after. Do not read the flag inside the
  open callback; use it as a "state changed, re-read next frame" signal.

Other manager-level event subtypes handled directly by `0x757840` (kind `0x10` events): `1` stop UI thread, `2` start
it, `3` go to menus (`0x7598E0`: clears `0x04`, destroys the game windows, builds the menu windows), `4` recreate game
windows (clears `0x04`, `0x75A690`, `0x759AF0`, then sets `0x04`), `5` cinematic fade begin/end (sets `0x10`/`0x20`),
`7` clears `0x200`, `0x10` set focus (section 8), `0x28`, `0x3E`.

Kind 5 subtypes `0x21`, `0x22`, `0x2B` are routed to the overview map (`+0x98`) and the taskbar (`+0x80`) before
anything else **[V routing, semantics ?]**.

## 6. Event kinds and keyboard/mouse routing

`cEvent` virtual slot 1 returns the **kind**, slot 2 the class category [V]:

| Kind | Class | Vtable | Fields |
|---|---|---|---|
| 1 | `cEvent_mouseMove` | `0x899264` | `+4..+0x10` four ints (position/state) |
| 2 | `cEvent_mouseButton_down` | `0x89704C` | x at `+8`, y at `+0xC` |
| 3 | `cEvent_mouseButton_up` | `0x899248` | same |
| 4 | `cEvent_key` | `0x897568` | `+4` u16 flags (bit 0 = char/"text" event), `+6` u16 character, `+8` u16 virtual key, `+0xA` u16 |
| 5 | `cEventUI2` / `cEventUI_string` | `0x890DD0` / `0x8911D8` | named UI event, above |
| 0xC | `cEventIME` | `0x89929C` | IME composition |

(`RE_NOTES.md` says "there is no mouse move event"; a `cEvent_mouseMove` class does exist with a kind-1 id, but the UI
reads hover from `cMouse`. Whether the engine ever posts it was not checked.)

### Order inside `cUI_Manager::receiveEvent` (`0x757840`) [V]

1. `DAT_009EC530 != 0` -> return true (everything swallowed).
2. Category-0x10 events: handle the manager subtypes above; everything else falls to the dispatch below.
3. Flag `0x80` set and kind in {1,2,3,4} -> swallowed.
4. IME event: forwarded to the focused edit control (`+0x148`) if it is a `cUI_EditControl` (RTTI dynamic cast).
5. **Modal message box** (`+0x144`) first: its `receiveEvent` runs; **if it is visible, every mouse/key event
   (kinds 1-4) counts as handled even if the box ignored it.** This is the one real modal in the game.
6. Mouse-down/key clears the hint bits `0x400/0x800`.
7. By mode:
   * **cinematic** (`0x10`, not handled): only the game menu (`+0xB0`) gets the event (and, if it is not in game, nothing).
   * **menus** (`0x01`): `menus[0]..menus[4]` in order, first to return true wins; if none and the key is Esc ->
     `handleEsc` (`0x7572C0`).
   * **in game** (`0x04`): key hotkeys (below), then mouse/other events through the window chain (below).

**Hotkeys (in game)**, case-insensitive virtual key via `0x84BCE6`, all via named events; gated so that nothing fires
while the game menu, megamap, savegame or options window is visible or `DAT_017EA344` is set (that global is only
referenced from network-player-list code, so: network trade/player-list context **[?]**). While the gated state holds,
only `M` (when the map is open: closes it), `O` (when options are open: runs the OK handler `0x717C20`) and `H` still work.

| Key | Action |
|---|---|
| Esc | `0x7572C0` ladder: closes the help screen, else the topmost of {map, log book, inventory/shop group, save/options via `clearFlags(5)`}, else opens `UI_WND_ESCMENU` (and posts `UI_TB_OPTIONS` through `0x553210`). If the game menu is open and its `+0x164` byte is 0, Esc is ignored; otherwise it closes it. Trade (`+0xD8`) open -> not handled here |
| I / F / C | open inventory pages 1 / 2 / 3 (`0x756710`) or, if that page is already shown, close it (`0x756870`). Page test: `0x6C6470` |
| H | toggle `0x200` (help screen) |
| L | toggle `UI_WND_QUESTBOOK` (not while the trade window is open) |
| M | open `UI_WND_MEGAMAP` unless `DAT_017EA2E4` is set or trade is open |
| N | toggle `UI_WND_NETINFO` (add-on only, `DAT_0182EBEC`) |
| O | open `UI_WND_OPTIONS` |
| S | quick save/save window, guarded by `0x75ADB0(1)` |
| T | toggle manager flag `0x1000` |
| `` ` `` / `^` (0x60, 0xDC), Esc | when the console (`+0xA4`) is visible these toggle it; all other characters (< 0x102 or 0x10F) go to the console |

**Mouse/other dispatch, in game** (after hotkeys; first window that returns true consumes the event) [V order, exact
exclusivity rules I]:

* If savegame is visible: savegame, then escape menu. Else if options or character is visible: that window, then the
  escape menu. Else if the escape menu is visible: escape menu then megamap/others. Else if purchase (`+0xB4`) is
  visible: purchase then escape menu. Else if horse (`+0xC4`) is visible: horse then escape menu.
* Otherwise the general chain:
  questbook, stats, networkInfo, trade, taskbar, inventory, equipment, minimap, overviewMap, console, merchant,
  blacksmith, cube, purchase, master, escapeMenu, megamap, savegame, options, character, chest, horse, netPortraits,
  with one exception first: when a megamap is open it is handled **before** the chain and the chain only continues for
  events it declines.
* If nothing consumes a mouse event the world gets it via `cEngine_worldMouse`.

`cUI_Manager_isCursorOverUi` (`0x75AA90`, called by `cEngine_updateWorldCursor` before picking) answers "is the cursor
over UI": only when flag `0x08` is set, it asks `isInside` (slot 7) of the **visible** windows in the order
inventory, escapeMenu, equipment, stats, inventory, merchant, blacksmith, master, taskbar, questbook, escapeMenu, chest,
horse, networkInfo, cube, trade; then returns true unconditionally if megamap, savegame, options or character is
visible; then asks every visible popup. Noteworthy: **not asked**: console, minimap, overviewMap, netPortraits,
purchase (so the world still receives clicks through them).

## 7. Draw order: `cUI_Manager::render` (`0x758ED0`, arg: IDirect3DDevice7*) [V]

Start: return if engine paused (`DAT_0182EE38 & 0x20000`), decrement `DAT_009EC530`, lock `0x66FA40`, reset render flags
(`0x643430` bits 0x800, 0x10, 8, 1, 4).

**Cinematic (`0x10`):** if also in game, escape menu `render`; popups; four black letterbox bars (strips 0..h and
768-h..768, h up to 64, ramped by `+0x78`).

**Menus (`0x01`):** `menus[4]` update (`+0x30`) if visible, `menus[0]` render, `menus[1]` render2, `menus[2]` render2,
`menus[3]` render, `menus[4]` render, popups (arg 1), then `dialog` render. A version string (`"%S%c ASE"`) is printed
at (969, 752).

**In game (`0x04`)**, requires flag `0x40` for the HUD part:

1. `TextLines` (`+0x14C`) update (slot 12). If `0x40`: `0x6D01F0` (party arrows at the screen edges).
2. **Exclusive windows, first visible wins and nothing else is drawn:** savegame `render`; else options `render2`;
   else character `render2`; else megamap `render` (+ help screen 6).
3. **Otherwise the HUD**, in this order (later = on top): TextLines `render`, netPortraits, blacksmith `render2`,
   merchant `render2`, master, chest, cube, trade, inventory, taskbar, stats, equipment, minimap, overviewMap, networkInfo,
   console, questbook `render2`, horse `render2`, purchase `render2`.
4. Help screen id passed to `showHelp` (`0x75B4F0`): 4 combo master / 7 rune exchange (master, by `0x6D1440`/`0x6D1420`),
   else 5 merchant, 3 blacksmith, 2 inventory, 1 none. Then popups (`0x758DC0`, arg `flags>>10 & 1`).
   Exclusive cases (1-2) skip this popup pass except the megamap.
5. `0x75C310` / `0x75C920` (late overlays, not analysed **[?]**), then the **escape menu** `render` (always).
6. If `0x40`: the **message box** (`+0x144`) renders on top.
7. `+0x04` tooltip control: forced visible-flag `0x8000`, rendered, flag restored.

Important consequences for Sacred Bild:

* **"visible" is not "drawn" is not "interactive".** While the megamap, savegame, options or character window is up,
  the HUD windows (taskbar, inventory, stats, ...) **keep their visible bit set but are neither drawn nor given
  events.** Any "is the inventory open?" test that only reads bit 0 is wrong in those states. The decision tree above
  is the single place that decides, and it can be mirrored exactly.
* The held item (`cMouse +0x68`) is drawn by whichever item-taking window is drawn (`cInventoryEntry_render` with
  flag 1), not by the manager.
* Window geometry cannot be trusted for "is it open": parked windows keep their rect (RE_NOTES), and in game
  constructors move windows after creation.

## 8. Focus

Two notions exist:

* **Mouse capture/hover** is not stored in the UI at all: controls read `cMouse` (`0x654F60`, x/y at `+4/+8`) every
  frame and the manager asks windows in the fixed order above.
* **Keyboard focus** is `manager +0x148` plus control flag `0x8000`. A control takes focus with `0x732630(this, 1)`:
  it sets its own `0x8000` and posts kind-5 subtype `0x10` named `"FOCUS"` with `+0x0C = this`. `0x757840` stores the
  pointer into `+0x148`; every other control that sees a subtype-`0x10` event for a different pointer clears its
  `0x8000` (`0x731CE0` tail, `0x732630(this,0)`). Hiding a visible window (`show(false)`, `0x726E90`) posts a FOCUS event
  with a null pointer, which clears `+0x148`. So: focus pointer null = nobody is typing. Edit controls (`cUI_EditControl`,
  vtable `0x897534`, receiveEvent `0x737AC0`, also `cUI_DigitEdit`) are what hold it; the console's input line,
  savegame names, character names and the net screens are the usual holders.
* The game has no generic "focused window". Keyboard hotkeys go through the manager hot-key switch above regardless
  of which window the mouse is over; text typed into the console is routed by the *console visible* test, not by focus.

Reading it reliably: `focus = *(cUI_Control2**)(manager + 0x148)`; treat as text-input only if non-null **and**
`focus->flags & 0x8000` **and** the object's RTTI is `cUI_EditControl`/`cUI_DigitEdit`/`cUI_Console`'s edit. Sacred
Bild's own `Overlay::isOpen()` handling in `focus.cpp` is about the OS window focus and is unrelated.

## 9. Popups, NPC dialog, tooltips, help

* `popups` vector at `+0x124`: `cUI_Tooltip` objects (`cUI_Popup` in `ui.h`); drawn by `0x758DC0` after the windows,
  **not** part of the event chain; asked by `isCursorOverUi`. Layout (`0x6E7AF0`): centered on x/y, clamped to
  16..1008 x 16..752, or centered on screen by flag.
* NPC dialog = a popup whose `answers[]` (`+0x217C`) are set; Sacred Bild's `npcDialog()` finds it by scanning the
  vector for a visible `cUI_Tooltip` with an answer. That is correct but linear and class-name based; a hook on the
  setter (`0x6E6D20`) would give open/close for free.
* Help screen: flag `0x200` + `showHelp(0x75B4F0)`; the screen id follows the windows (section 7).

## 10. Menus

`flags & 1`: `menus[0..4]` (main menu, character select, options, load game, network). Within `cUI_MainMenu` the
`screen` field picks the entry vector (0 main, 1 multiplayer, 2 extras, 3 credits). `cUI_Network` shows one of eight
sub-screens (`screen` at `+0x158`, section `ui.h`). The message box (`+0x144`) works the same in menus (draw: last;
events: first).

## 11. Where Sacred Bild is brittle today, and the fix

Observed in `src/game` (all `visible()`-bit based unless noted):

| Code | Heuristic | Failure mode |
|---|---|---|
| `ui_canvas.cpp fullScreenWindowOpen` | any `windows[]` entry visible with rect covering 1023x767 | Correct set of windows, but ignores that the *manager* draws only the first exclusive one; fine in practice, would break if a mod window is added |
| `controller.cpp windowOpen` | visible bit of every window except a hand-picked skip list | Counts HUD windows that are visible but hidden behind an exclusive window; the skip list must be kept in sync by hand |
| `ui_nav.cpp controls()` | hand-written copy of the draw order (`savegame, options, character, megamap` alone, else all but overviewMap/console) | Drift from the engine; misses that horse/purchase are `render2` windows and that netPortraits/console are not hit-tested |
| `UiNav::modal()` | message box via `messageBox()` + escape menu visible | Misses the real rule (any visible message box swallows all mouse/key events) only if `messageBox()` differs from `+0x144` |
| `UiNav::npcDialog()` | scan popups by class name | Works; O(n) per query and relies on RTTI string compare |
| `controller.cpp detect()` | flags + `engine->flags & loading` + the above | No notion of "hold-off" frames (`DAT_009EC530`), `0x80` input-block, cinematic fade phase `0x20` |

### Proposal: one `UiState` snapshot, taken once per frame

Take it at the entry of `cUI_Manager_render` (already hooked for the canvas) and expose `UiState::get()`:

```cpp
struct UiState {
    enum class Mode { None, Cinematic, Menus, Game } mode;     // 0x10 > 0x01 > 0x04, else None
    bool inputBlocked;        // flags & 0x80, or *(int*)0x9EC530 != 0
    bool windowsExist;        // flags & 0x08
    // In game: what the manager will draw this frame (mirrors 0x758ED0)
    enum class Exclusive { None, Savegame, Options, Character, Megamap } exclusive;
    uint32_t hudOpen;         // bit per Window enum: visible bit AND not hidden by 'exclusive'
    bool escapeMenu;          // window(escapeMenu) visible
    bool messageBox;          // *(cUI_Window2**)(m+0x144) visible  -> modal, swallows kinds 1-4
    bool helpScreen;          // flags & 0x200
    cUI_Control2* focus;      // *(m + 0x148)
    bool textInput;           // focus && (focus->flags & 0x8000) && RTTI is edit control || console visible
    int  helpScreenId;        // 1..7 as computed in 0x758ED0
    bool cursorOverUi;        // manager->isCursorOverUi(cursor) -- or the order list in section 6
    bool heldItem;            // cMouse +0x68 != 0
};
```

Rules to implement, all taken from the code above:

1. `windowsExist = flags & 8`; **never dereference `windows[]` unless it is set** (`0x75A250` nulls them on return
   to the menu; the pointers are valid again only after `0x759AF0`).
2. `exclusive` = first visible of savegame, options, character, megamap (in that order, as the render does).
3. `hudOpen[w]` = visible bit && `exclusive == None`.
4. `modal` = messageBox visible; escapeMenu visible is *semi*-modal (it is checked first among windows for
   the savegame/options/character cases, but HUD windows below it still draw).
5. Replace `windowOpen()` / `controls()` / `fullScreenWindowOpen()` with queries on the snapshot, delete the skip lists.

To avoid polling and to react to the exact frame a window changes, hook the `show` slot (vtable slot 9) of the
classes in the table (or `0x732550`/`0x7325C0`/`0x731CE0`) and bump a generation counter; the render-entry snapshot
then recomputes only when the counter moved or flags changed.

## 12. Hook points for Sacred Gear (mods)

| Need | Hook / API | Notes |
|---|---|---|
| Know whether a window is open | `UiState` above; stable id = control name (`UI_WND_*`) at `+0x30` | |
| Open/close a window | build `cEventUI2` (subtype `0x11`/`0x12`/`0x13`, mask 5, target name) and call `manager->vtable[0]` | Same path as the hotkeys |
| React to open/close | patch vtable slot 9 (`show`) per class, or `0x732550`/`0x7325C0`/`0x731CE0` | Called before the visible bit is set when opening |
| Intercept all mouse/key input before the UI | `cEngine_receiveEvent` (`0x618090`, already hooked by `ui_canvas`) or `manager->vtable[0]` (`0x757840`) | Return true to swallow. Respect `messageBox` modal and `inputBlocked` |
| Stop world clicks through a mod panel | hook `cUI_Manager_isCursorOverUi` (`0x75AA90`) and OR in the panel's rect | Already redirected once by Sacred Bild (`0x611EE3`) |
| Draw a mod panel *under* tooltips, over windows | hook the popup pass `0x758DC0` (draw first, then original) | Skip when `exclusive != None` if the panel is HUD-like |
| Draw *over everything* in the manager's UI space | after `cUI_Manager_render` (`0x758ED0`) returns | Drawn inside Sacred Bild's UI scope so it follows the canvas |
| Draw a text/popup-like element with game styling | push a `cUI_Tooltip` into the popups vector (`+0x124`) and set its text with `0x6E6EA0`/`0x6E6FB0` | Draw-only; no events (not in the dispatch chain) |
| Align to existing UI | read the window's `x,y,width,height` (absolute for top-level windows) from the table in section 3, anchor to an edge; if Sacred Bild's HUD anchoring is on, use `UiAnchor::frame(window, frame)` so the mod panel moves with the window's frame | Do not hard-code the rects: the constructors move several windows |
| Add a *new interactive window* | not possible as a first-class window: the `windows[]` array and the draw/event sequences are fixed. Use the event hook + draw hook above and keep the mod window's state in the mod | A permanent solution would patch the dispatch sequences, which is large |
| Text focus awareness | `manager+0x148` + flag `0x8000` (section 8) | Needed so mod hotkeys do not fire while the player types in the console |
| Console | `UI_WND_CONSOLE` (`+0xA4`), toggled by ` or ^; characters go to it only while visible | A mod command line could reuse it by wrapping its `receiveEvent` |

## 13. Not verified / open questions

* Exact semantics of control flags `0x10`, `0x20`, `0x20000`, `0x20000000` (the "exclusive" group set by `0x732550`).
* Constructor rects of purchase, horse, networkInfo (the constructors set them internally, only the `new` size is visible
  in `createGameWindows`).
* The late overlays `0x75C310` / `0x75C920` at the end of the in-game render.
* The exact precedence among the "exclusive" mouse dispatch cases (section 6) when several windows are visible at once,
  only the code shape was read, not every branch.
* Consumers of manager flag `0x1000` (T key), `DAT_017EA344`, `DAT_017EA2E4`, `+0x110..+0x120`.
* Whether `cEvent_mouseMove` events are posted at all (hover appears to be polled from `cMouse`).
* The German build: addresses here are English-build. `sacred_sigs.inc` covers the existing hooks; the new ones
  (`0x757840`, `0x758DC0`, `0x732550`/`0x7325C0`/`0x731CE0`, window `show` slots) would need signatures added.
* Everything in `src_extra/` is auto-created code with default calling conventions fixed to thiscall for vtable
  targets only; gap functions may contain data decoded as code.

## Appendix: function index (English build)

| VA | Role |
|---|---|
| `0x756A90` | `cUI_Manager` ctor |
| `0x757840` | `cUI_Manager::receiveEvent` (vtable slot 0) |
| `0x7572C0` | `handleEsc` |
| `0x758ED0` | `cUI_Manager::render` |
| `0x758DC0` | popup pass |
| `0x75AA90` | `isCursorOverUi` |
| `0x759AF0` / `0x75A250` | create / destroy in-game windows |
| `0x75A480` / `0x7598E0` | create menu windows / switch to menus |
| `0x75B4F0` | help screen |
| `0x75ADB0` | save guard (S hotkey) |
| `0x756710` / `0x756870` | open / close inventory |
| `0x731CE0` | `cUI_Control2::receiveEvent` (named flag events) |
| `0x732550` / `0x7325C0` | `setFlags` / `clearFlags` |
| `0x732630` | take/drop focus |
| `0x726E90` | generic `show(bool)` (posts FOCUS release on hide) |
| `0x7277B0` | generic window `render` |
| `0x808E50` / `0x8092F0` | event bus getter / post |
| `0x416780`, `0x590620` | event name helpers (`0x590620(ev, target, sender)`) |
