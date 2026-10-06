# SacredBild

> **AI disclosure:** SacredBild is developed with an AI coding assistant (Anthropic's Claude). Most of the code,
> the reverse engineering notes and this documentation were written with it, at the maintainer's direction; the
> maintainer decides what goes in and tests the changes in the game. The source is kept to what the features need
> and documented so that people can review it: see [Reading the code](#reading-the-code).

A `ddraw.dll` hook for **Sacred Gold** that runs the game at modern resolutions and instruments its
renderer, as groundwork for replacing the slow parts. It runs the game's DirectDraw / Direct3D 7 on Direct3D 9Ex
itself (or loads [DDrawCompat](https://github.com/narzoul/DDrawCompat) behind itself, `Backend=chain`), and patches
the game's internals (the approach of [GD3D11](https://github.com/kirides/GD3D11) for Gothic).

Supported executables: the **English** GOG `Sacred.exe` (PE timestamp `0x452F85C7`) and the **German**
`sacred.exe` 2.0 (`0x451BBE74`), with their `gameserver.exe`. SacredBild finds what it patches by byte signatures, not
fixed addresses, so other builds with the same code should work too; if a signature isn't found, it only passes
ddraw calls through and logs which one.

## What it does

- **Any resolution** (default: desktop resolution) instead of the hard-coded 1024x768:
  - back buffer and window at the target size: borderless when it fills the screen, otherwise with a frame
    (caption and system menu: it can be moved and minimized) around a client area of that size (`[Display] Borderless`);
    its close button is disabled so a stray click doesn't end the game (Alt+F4 still quits);
  - world view: orthographic projection (including its depth range), visible area, culling and
    overhead-label layout scaled so the world keeps its original pixel density and simply shows more
    (including the 0.5x-2.0x zoom);
  - world view centered on the screen (the game assumed a 512/384 screen center in 17 places);
  - UI: the game's 1024x768 UI (menus, HUD, cursor, intro videos) is drawn into a centered canvas,
    scaled to fit the screen height (`[UI] Scale`, `ScaleMode`, `LinearFilter`); the mouse is mapped into that canvas
    for the UI while world picking keeps physical screen coordinates;
  - in game, the HUD windows are placed on the screen (`[UI] Anchor`, positions in `[UI.Layout]`); by default
    at the edges they had in the 1024x768 layout: taskbar and chat at the bottom, inventory bottom-left, minimap,
    character stats and equipment top-right, party portraits and the shop / chest / cube / trade windows
    top-left. Each keeps its own layout and runs in a shifted copy of the 1024x768 space (drawing, cursor, clicks,
    tooltips), so the game's code for it is unchanged; menus and full-screen windows (map, options, save) stay
    centered. Tooltips and the item on the cursor use the whole screen, the help screen's texts (H key) stay next
    to the windows they explain, and the escape menu and message boxes dim the whole screen;
  - loading screen (GDI) drawn into a 1024x768 surface and scaled like the menus, splash centered; savegame
    thumbnails taken from the screen center;
  - always a 32-bit display mode (`GFX32 : 0` in `Settings.cfg` is ignored);
  - screenshots (Print Screen) of the whole screen as `Capture\shotNNNN.png` (or `.jpg`, `[Screenshot] Format`),
    saved on a worker thread; the game's own wrote the top-left 1024x768 as TGA + JPEG, sheared at other widths.
- **Direct3D 9Ex backend** (`src/ddraw9/`): DirectDraw 7 and Direct3D 7 implemented on Direct3D 9Ex, for the
  subset Sacred uses (windowed swap chain, one render target with z-buffer, managed textures, system memory surfaces
  for GDI text, the fixed-function device). Both APIs are fixed-function with the same vertex formats and render
  states, so device calls translate almost one to one. Frames are presented with the flip model (the blit model
  with `VSync=0`, which is not held to the refresh rate; `MaxFrameLatency`), and the per-call layers of Windows' Direct3D 7 runtime and DDrawCompat are gone. Anything the
  backend doesn't implement is logged once (`Direct3D 9 backend: not supported: ...`).
- **Movies through Media Foundation** (`[DDraw] MediaFoundation`, always with the Direct3D 9 backend): the game
  decodes its WMV movies with DirectShow into DirectDraw surfaces, which needs a real DirectDraw, and its loop stops
  processing window messages. SacredBild plays them with the Media Engine instead (audio included), draws them over
  the whole screen with their aspect ratio kept, and keeps the window responsive; ESC, space and mouse buttons skip
  as before. Where the Media Engine is missing (Windows 7, N editions) or can't play a movie (Wine), they are
  decoded with the game's DirectShow streams into a system memory surface of Windows' own DirectDraw instead, and
  drawn the same way (`[Debug] MovieFallback`).
- **Batched world rendering**: the world view issues thousands of small sprite and ground draws per frame
  (almost 10,000 zoomed out at 1920x1200). SacredBild records state changes instead of applying them and
  merges consecutive draws that end up with the same state into one call; small textures are copied into
  shared atlas pages so draws with different textures merge as well.
- **Characters animated on the GPU**: Granny skinned every character and its shadow on the CPU each frame (a fifth
  of the render thread zoomed out). With the Direct3D 9 backend, a vertex shader does that from meshes kept on the GPU
  and lights them as Direct3D 7 did (2560x1440 zoomed out: 94 -> 132 fps).
- **LAN games over VPNs**: Sacred finds LAN games through broadcasts that Windows sends on one network adapter
  only and that many VPNs don't carry, and the announced address is one Sacred picked from the first three
  adapters it found. When Sacred starts the gameserver for a hosted game, SacredBild goes along: every
  announcement goes out on every adapter with that adapter's own address, and to players who list the host in
  `[Net] Hosts` (see below).
- **Game connection over UDP** (`[Net] Udp`, optional): Sacred's game connection is TCP, which waits at least
  300 ms (doubling on every further loss) before it resends a lost packet, and after a loss holds everything behind it
  back; distant and wireless connections show that as stalls and rubber banding. With SacredBild on both sides, the
  connection runs over UDP instead (the same byte stream, kept in order by [KCP](https://github.com/skywind3000/kcp)):
  a lost packet is resent after about one round trip, the connection picks up within a round trip after an outage,
  and it survives the player's address changing (Wi-Fi to mobile, a new address from the provider). Joining a host
  without it connects over TCP as before. Every game connection logs its round trip and resends when it closes.
- **Matchmaker** (`[Net] Matchmaker`, `matchmaker/`): a small server (Go; Linux, Windows, macOS) that lists the games
  of everyone using it in Sacred's own LAN list, so that LAN mode becomes a global lobby. Joining a game gets the
  player introduced to the host, and both open their way to each other (UDP hole punching), so hosts don't need port
  forwarding with the UDP connection. Over IPv6 as well as IPv4, so hosts without a public IPv4 address (CGNAT,
  DS-Lite) can host too. Its web page shows the games (name, players), never addresses, and it logs no addresses
  unless told to (see [Online games](#online-games-matchmaker)).
- **Languages**: the GOG builds ignore the game's `LANGUAGE` for text and speech (the text is built into the exe,
  the speech is always `PAK\sound.pak`). SacredBild loads the language's own files when they are there, so one
  install runs every language you have the files for (see below).
- **Input only in the foreground**: the game polls keys and mouse buttons whether or not it has the focus, moves
  the cursor, and its low-level keyboard hook swallowed the Windows keys, Alt+Tab and Alt+Esc system-wide. All of
  that now only happens while the game is in the foreground (its cursor still follows the mouse over the window, so
  you see where a click will land), so typing in another window doesn't move your character, and Alt+Tab / Alt+Esc work in
  game too (the Windows keys and Ctrl+Esc stay blocked while it has the focus). The mouse is confined to the
  game window while it is in the foreground (`[Display] ClipCursor`), e.g. a window on one side of a 32:9 screen
  or a borderless game next to a second monitor; it is let go while the window is moved (Alt+Space, Move) or a
  menu of it is open, and while Alt is held; it is only taken once the cursor is over the window, so a click on the
  title bar still drags.
- **Rendering in the background**: the game stopped drawing when it lost the focus, which often left the main menu
  black. It now keeps drawing at `[Display] FpsLimitInactive` frames per second.
- **Frame limiter without a busy core**: the game's limiter (60 fps in game and in the menus) spun on `Sleep(0)`
  for the rest of every frame. SacredBild sleeps on a high-resolution timer instead.
- **Controller** (`[Controller]`, through [SDL3](https://www.libsdl.org): Xbox, PlayStation and Switch pads; under
  Wine / Proton through Wine's own controller support), in the
  manner of Diablo 2 Resurrected: the left stick walks, the attack and combat art buttons hit the nearest enemy in
  the stick's direction, potions, weapon slots and windows are buttons, and the menus and windows get a cursor the
  sticks move and the D-pad jumps from button to button (with the game's own hover effect). Whatever you used last,
  controller or keyboard and mouse, is in charge. Opening the Options with the controller shows them as
  SacredBild's own screen (Direct3D 9 backend), laid out for the pad, with a page for the controller's buttons and
  settings (see [Controller](#controller)).
- **Diagnostics**: optional per-second frame stats (`[Debug] D3DStats`: draw calls, texture switches, unique
  textures, time spent in the world renderer, UI, flip and inside Direct3D, what ended each batch, texture memory)
  and an optional sampling profiler (`[Debug] Profiler`).
  When the game or its gameserver crashes, a minidump goes next to the exe (`SacredBild-crash-*.dmp`,
  `SacredBild-server-crash-*.dmp`; `[Debug] CrashDump`) and the log names the exception and where it happened.

## Install

From a [release](../../releases) (tagged versions, and a nightly prerelease of every push to `main`): extract the
zip into the game folder. It replaces the `ddraw.dll` there (if that is DDrawCompat, move it to
`SacredBild\DDrawCompat.dll` first) and an existing `SacredBild.ini`.

The Steam version works the same way: its `Sacred.exe` is the English GOG exe inside Steam's DRM wrapper
(SteamStub), and SacredBild starts once the wrapper has decoded the game's code.

From a build (see [Build](#build)):

```powershell
.\install.ps1 -GameDir "B:\Spiele\GOG Games\Sacred Gold"
```

This backs up the current `ddraw.dll`, moves an existing DDrawCompat to `SacredBild\DDrawCompat.dll`,
copies SacredBild's `ddraw.dll` and creates `SacredBild.ini` if there is none. `uninstall.ps1` restores the backup.
DDrawCompat is only used with `[DDraw] Backend=chain`; without it, that falls back to Windows' own `ddraw.dll`
(whose Direct3D 7 runtime refuses render targets over 2048 pixels; SacredBild works around that).

The Direct3D 9 backend runs on any `d3d9.dll`: `[DDraw] D3D9`, else a `d3d9.dll` next to the game exe (e.g.
[DXVK](https://github.com/doitsujin/dxvk)'s 32-bit one), else Windows' own. `SacredBild.log` names the one in use
(`Direct3D 9: using ...`). Under Wine / Proton, SacredBild's `ddraw.dll` only loads with a native override:
`WINEDLLOVERRIDES="ddraw=n,b"`.

## Configuration (`SacredBild.ini`)

Before the game starts, a settings window offers the settings below with tooltips: General (resolution, window frame,
frame limit, VSync, UI size, renderer), Advanced (everything else; the controller's buttons are set in game) and HUD layout (`[UI.Layout]`). Play saves the
changed ones to `SacredBild.ini`; Exit quits. "Don't show this window
again" sets `[Launcher] HideSettingsWindow=1`; holding Shift as the game starts shows it anyway.

| Section | Key | Default | Meaning |
|---|---|---|---|
| Launcher | HideSettingsWindow | 0 | 1 = no settings window before the game starts; holding Shift as the game starts shows it anyway. |
| Display | Width, Height | 0 | Render resolution; 0 = desktop. 1024x768 = unpatched game. |
| Display | Borderless | auto | Main window frame: `auto` = a frame when the window is smaller than the screen, `1` = never (the game's frameless window), `0` = always. The client area is `Width` x `Height` either way. |
| Display | ClipCursor | 1 | Confine the mouse to the game window while it is in the foreground; hold Alt to move it out. |
| Display | FpsLimit | 60 | The game's own in-game frame limit; 0 = off. |
| Display | FpsLimitInactive | 20 | Frame limit while the game is in the background, in game and in the menus; 0 = off. |
| Display | VSync | 1 | Direct3D 9 backend: present on the display's refresh (flip model); 0 = right away, not limited by the refresh rate (blit model). |
| Display | MaxFrameLatency | 1 | Direct3D 9 backend: frames the CPU may queue ahead of the GPU. |
| UI | Scale | 0 | UI scale; 0 = as large as fits the screen height, otherwise a factor (1 = native pixels), capped at that. |
| UI | ScaleMode | InGame | Where `Scale` applies: `InGame` = the in-game UI only; the menus, the full-screen windows in game (options, save/load, character export, map) and the loading screen always fill the screen height. `Full` = those too. |
| UI | LinearFilter | 1 | Bilinear filtering for the scaled UI instead of the game's point sampling. |
| UI | Anchor | 1 | In game, HUD windows placed by `[UI.Layout]`; 0 = all of the UI in the centered canvas. |
| UI.Layout | Taskbar, Chat | 2048,4096 | Where a window's 1024x768 layout goes: X,Y in 0..4096 of the room the screen leaves around it (0 = left/top edge, 2048 = centered, 4096 = right/bottom edge). |
| UI.Layout | Inventory | 0,4096 | |
| UI.Layout | Stats, Equipment, Minimap | 4096,0 | |
| UI.Layout | Portraits, Shops | 0,0 | Party portraits; blacksmith, merchant, combat art master, chest, cube and trade. |
| Render | TextureBudgetMB | 0 | Texture memory the game may keep loaded; 0 = the game's value, at least 256. |
| Render | Batch | 1 | Merge the world view's draw calls. |
| Render | BatchNoClip | 1 | Merged draws skip Direct3D 7's software clipping; the GPU clips. |
| Render | BatchVertexBuffer | 1 | Merged draws go through vertex buffers instead of user memory. |
| Render | BatchModels | 1 | 3D model draws go through the batcher too (vertex buffers, merged where possible). |
| Render | GpuSkinning | 1 | Characters and their shadows skinned (animated) in a vertex shader instead of by Granny on the CPU; `Backend=d3d9` only. |
| Render | OffscreenPoses | 4 | Skeletons of characters not drawn in the last frames are posed every Nth frame (staggered) instead of every frame; one drawn after all is posed before it is drawn. 1 = every frame. Needs `GpuSkinning`. |
| Render | BatchGround | 1 | The ground's quad batcher hands its textures and quads to the batcher in one call instead of three device calls per quad. |
| Render | RecordIndex | 1 | Hash index (gtl::flat_hash_map) in front of the game's tile/object record caches. |
| Render | AsyncAnimation | 1 | Advance Granny animations on a worker thread, overlapping the start of the frame. |
| Render | Atlas | 1 | Copy small textures into shared pages so more draws merge. |
| Render | AtlasPageSize | 8192 | Atlas page size in texels (clamped to the GPU limit, halved if the GPU refuses it). |
| Render | AtlasPages | 2 | Pages per texture format; the least recently used one is reused when full. |
| Render | AtlasMaxTextureSize | 512 | Larger textures are used directly. |
| Screenshot | Format | png | Print Screen saves `Capture\shotNNNN.png`; `jpg` = JPEG (quality 95) instead. |
| Net | Relay | 1 | Hosted games: the gameserver announces on every adapter and answers `Hosts` subscriptions. |
| Net | Port | 2105 | Host side: UDP port of the relay, the UDP game connection and the matchmaker. |
| Net | Hosts | | Hosts whose games are listed even without broadcasts: IPv4 addresses or names, comma-separated, optional `:port`. |
| Net | NoDelay | 1 | Game connection without Nagle's algorithm in both data flow modes (the game: LAN only). |
| Net | JoinTimeout | 30 | Seconds a joining player has to send its first message to a gameserver Sacred started (the game: 5). |
| Net | Udp | 0 | Game connection over UDP when the other side has it on too (hosting: accept it); otherwise TCP. |
| Net | Prefer | IPv6 | Joining over UDP a host with IPv4 and IPv6 addresses: `IPv6` or `IPv4` is tried first, the other one as well after a second without an answer. |
| Net | Matchmaker | | Matchmaking server, name or address with optional `:port` (default 2107); empty = none. |
| Net | Publish | 1 | Hosting with a `Matchmaker`: the game is listed there. |
| DDraw | Backend | d3d9 | `d3d9` = SacredBild's own Direct3D 9Ex backend; `chain` = the ddraw below. |
| DDraw | Chain | `SacredBild\DDrawCompat.dll` | `Backend=chain`: ddraw loaded behind SacredBild; empty = system ddraw. |
| DDraw | D3D9 | | `Backend=d3d9`: `d3d9.dll` to use (e.g. DXVK), relative to the game folder or absolute. Empty or not loadable: a `d3d9.dll` next to the exe, then Windows' own. |
| DDraw | MediaFoundation | 1 | Movies through Media Foundation; always on with `Backend=d3d9`, 0 = the game's own player with `Backend=chain`. |
| Debug | D3DStats | 0 | Frame statistics, hitches and texture memory in `SacredBild.log`, and the world view's time by pass (`passes:` lines). |
| Debug | Profiler | 0 | Sampling profiler; writes `SacredBild-profile.txt` every 15 s. |
| Debug | ProfilerIntervalUs | 500 | Sampling interval. |
| Debug | UiTrace | 0 | Scroll Lock logs one UI frame's draws (`UiTrace:` lines: position, UI frame, calling game code) and popups set during the next 5 s. |
| Debug | CrashDump | 1 | Minidump next to the exe on a crash: 0 = off, 1 = stacks and the memory they point to (small), 2 = all memory (for the game's globals; hundreds of MB). |
| Debug | SkinCheck | 0 | Compares Granny's CPU skinning of characters with per-vertex bone weights rebuilt from its meshes (`Skin check:` lines every 10 s): groundwork for skinning on the GPU. |
| Debug | MovieFallback | 0 | Movies always through the fallback (DirectShow into a system memory surface), as without the Media Engine. |
| Controller | Enabled | 1 | Play with a controller. |
| Controller | Deadzone | 24 | Percent of a stick's travel ignored around its center. |
| Controller | CursorSpeed | 900 | Cursor speed in menus and windows at full deflection, in 1024x768 pixels per second. |
| Controller | MoveRadius | 160 | How far ahead of the hero (screen pixels) the left stick walks to. |
| Controller | AimRange | 450 | Aim assist: enemies within this many pixels of the hero. |
| Controller | AimCone | 90 | Aim assist: enemies within this angle (degrees) around the stick's direction come first. |
| Controller | ArtClick | 1 | Combat art slot buttons select the art (keys 6-0) and right-click the target; 0 = select only. |
| Controller | Walk | 1 | The left stick pushed less than halfway walks (Shift) instead of running. |
| Controller.Bindings | *action* | see below | Button for each action: `A B X Y LB RB LT RT Back Start L3 R3 Up Down Left Right`, or `Modifier+Button` (e.g. `LT+A`); empty = none. |

## Controller

With `[Controller] Enabled=1` (the default) a controller plays the game; the first input from it takes over
from the keyboard and mouse, moving the mouse a few pixels or pressing a key hands it back.

In game:

- **Left stick**: walk (pushed less than halfway: walk instead of run, `Walk`). Walking clicks the ground ahead of the
  hero with the world pick turned off, so it never attacks, talks or picks something up on the way.
- **Attack** (A): attacks the nearest enemy in the left stick's direction (the right stick aims instead when pushed),
  as long as the button is held, and moves on to the next one when it dies. Without an enemy it talks to, opens or
  picks up what is nearest; with nothing there it attacks in place (Ctrl).
- **Combat art** (X): right click at the nearest enemy (the active combat art). The **art slot** buttons select
  slot 1-5 (keys 6-0) and do the same (`ArtClick`).
- Everything else presses the game's key: weapon slots 1-5, the potions, windows, quick save / load, zoom.

Default layout ("LT+" = with LT held):

| Button | Action | With LT |
|---|---|---|
| A | Attack / interact | Combat art slot 5 |
| X | Combat art (right click) | Weapon slot 1 |
| Y, B, RB, RT | Combat art slots 1, 2, 3, 4 | Weapon slots 2, 3, 4, 5 |
| LB | Stand still (Ctrl) | |
| D-pad up / down / left / right | Healing potion / heal hirelings / antidote / concentration | Undead Death / Mentor / zoom out / zoom in |
| Start / Back | Game menu (Esc) / inventory | Options / world map |
| L3 / R3 | Show names (Alt) / overview map (Tab) | Collect all / cursor mode on and off |

In menus and windows (and in game in cursor mode, e.g. for a conversation the game shows as a window SacredBild doesn't
recognize): the left stick moves the cursor, the D-pad jumps to the next button, menu entry, list or check box that
way, A and X are the left and right mouse button (A twice quickly: a double click, e.g. to load a savegame), B is Esc,
the right stick scrolls; Start and Back keep their window keys; LB / RB switch the inventory's pages (backpack, combat
arts, combos). While a message box or the game menu is open, the D-pad stays in it and the cursor starts on its OK or
first entry; in a message box B is its Cancel (or its only button). In an NPC dialog with answers, A is Enter (the
first answer) and B clicks the second one. R3 shows or hides the overview map (one press each, where Tab on the
keyboard shows it while held).

The log book (L) has no cursor: LB / RB switch the tabs across the top, the D-pad (up / down) the tabs down the left
edge; the left stick picks an entry of the list (up / down) and turns the list's pages (left / right), the right stick
turns the right page's (right or down: the next); B closes it. In cursor mode it works with the cursor instead.

Opening the Options with the controller shows SacredBild's own screen: the game's settings (with the game's own
texts), and a Controller page with the settings above and every action's button: select one and press a button, or
hold one and press another for a two-button binding; X clears one. LB / RB switch pages, Start accepts (the game
saves and applies its settings as from its own window; the controller's go to `SacredBild.ini`), B cancels. With
the keyboard or mouse the game's own Options window opens.

## LAN games over a VPN

Everyone installs SacredBild. The host creates the LAN game as usual; Windows Firewall has to let
`gameserver.exe` receive on the VPN adapter (the prompt on first start, or a rule for its TCP port and UDP 2105;
VPN adapters are often in the "Public" profile).

- VPNs that carry broadcasts (ZeroTier, Hamachi, Radmin VPN, OpenVPN TAP): the game shows up in the LAN list.
- VPNs without broadcasts (WireGuard, Tailscale, OpenVPN TUN): joining players add the host's VPN address,
  e.g. `Hosts=10.8.0.2` or a Tailscale name. A shared list of all players works, a PC skips its own addresses.

The host's relay logs to `SacredBild-server.log`, the LAN list to `SacredBild.log` (lines starting with `LAN`).

## Online games (matchmaker)

Everyone sets `[Net] Matchmaker` to the same server and `Udp=1` (the settings window: Network tab). The host
creates a LAN game as usual; it shows up in everyone's LAN list, with the host's public address. Joining it:

1. the player asks the matchmaker to introduce it to the host; both send each other a few UDP packets, which opens
   their routers (most home routers; two players behind the strictest kind, e.g. some mobile networks on both
   sides, still need port forwarding);
2. the game connection runs over UDP to the host's `[Net] Port`: over the family `[Net] Prefer` names (IPv6 by
   default) first, and over the other one as well if that has no answer within a second (whichever answers first
   then); if neither answers within 3 s (4 s with both), the player connects over TCP to the game's port, which then
   needs to be forwarded on the host.

IPv6 needs no address translation, so step 1 almost always works there, and it reaches hosts that have no public
IPv4 address of their own (mobile networks' CGNAT, DS-Lite cable and fibre connections). Both sides need IPv6 for
that, and the matchmaker an IPv6 address (an AAAA record). Sacred itself only knows IPv4: a game the matchmaker
reaches over IPv6 only is listed with a stand-in address from 198.18.0.0/15 (Sacred has 4 bytes for a game's address,
and its LAN list tells games apart by address and name; the address is not shown in game). Such a game can only be
joined over UDP: if that fails, joining fails right away. `SacredBild.log` names the game behind each stand-in.

In the LAN list, matchmaker games on the UDP connection whose host IPv6 reaches show that in front of their name:
`[IPv4+6]`, or `[IPv6]` (only over IPv6). All other games keep their name.

Players behind the same router as the host see the game twice; the LAN one is the one to join. The host's
gameserver logs the matchmaker and its players to `SacredBild-server.log`, a player's joins go to `SacredBild.log`
(lines starting with `Matchmaker` and `UDP`).

Running a matchmaker: see [`matchmaker/README.md`](matchmaker/README.md) (one executable, UDP 2107 and a web page on
8080). The protocol is described in [`docs/UDP_PROTOCOL.md`](docs/UDP_PROTOCOL.md).

## Languages

Sacred takes its language from `LANGUAGE` in `settings.cfg`, one of `US DE FR SP IT PL HU JP VC RU CZ` in upper
case (anything else quietly means `DE`), or from the code in lower case on the command line (`sacred.exe de`).
Both GOG installs ship with `LANGUAGE : US`, whatever their language. SacredBild uses the language's files if they
are there, and otherwise what the build ships with:

- text: `scripts\<code>\global.res`, else the text built into the exe;
- speech: `PAK\sound.<code>.pak`, else `PAK\sound.pak`.

`import-language.ps1` takes the text out of another install's exe, copies its `PAK\sound.pak` and stores both
under a code, e.g. German into the English install:

```powershell
.\import-language.ps1 -From "B:\Spiele\GOG Games\Sacred Gold" -Language DE -GameDir "B:\Spiele\GOG Games\sacred gold GOG"
```

Then set `LANGUAGE : DE`. `-HardLink` links `sound.pak` instead of copying it (400-470 MB, same drive only),
`-NoSpeech` takes only the text. A GOG install's `scripts\us\global.res` is a copy of its built-in text, so
`LANGUAGE : US` keeps the install's language until you import another one as `US`. To add English to the German
install, import it as `US`; `LANGUAGE : DE` then still gives German (no `scripts\de` and no `sound.de.pak`, so the
built-in text and `PAK\sound.pak`). `SacredBild.log` names the files in use (lines starting with `Language:`).

The code also changes a few things in the game: keyboard input for `PL`, IME and line breaking for `JP` and `VC`,
no status text on the loading screen for `SP`. Savegames store it; loading one saved under another code only logs
the difference.

## Reading the code

SacredBild is one DLL, `ddraw.dll`, which the game loads from its folder in place of Windows' own. Start in
`src/main.cpp`: `DllMain` reads `SacredBild.ini` (`src/config.*`); with the settings window, the rest waits for the
exe's entry point, after the window (`src/settings_window.*`). Then it loads the DirectDraw implementation the game
will get (`src/proxy.*`), finds the game's code by byte signature (`Sacred::resolveAddresses`) and installs the
patches (`Sacred::installHooks` in `src/game/build.cpp`, which lists every module in order). Each module patches
the game in its `install()` with [Detours](https://github.com/microsoft/Detours) hooks or checked byte patches
(`src/patch.*`: a write happens only if the bytes there are the expected ones).

What the game's code at those addresses does is in `docs/RE_NOTES.md` and in the comments of
`src/game/sacred_addr.h`; the addresses refer to the English GOG build (Ghidra program `/Sacred.exe (GOG)`).

### Source files

| Path | What it is |
|---|---|
| `src/main.cpp` | DLL entry: game (`sacred.exe`) or LAN gameserver (`gameserver.exe`) setup |
| `src/proxy.*`, `src/exports.def` | The `ddraw.dll` exports, to the Direct3D 9 backend or the chain-loaded ddraw |
| `src/system_ddraw.*` | `Backend=chain` on Windows' own ddraw: render targets over 2048 pixels |
| `src/config.*` | `SacredBild.ini` |
| `src/settings_window.*` | The settings window before the game starts (`[Launcher] HideSettingsWindow`): what it shows, declared control by control |
| `src/ui/form.*` | `Ui::Form`: settings windows declared in code (pages, groups, rows of controls bound to ini keys), laid out and run as dialogs |
| `src/log.*`, `src/crash_dump.*` | `SacredBild.log`, minidumps on crashes |
| `src/patch.*`, `src/sig.*` | Code patches and hooks; byte signature search |
| `src/spin_lock.h` | Locks for the render thread's hot paths |
| `src/profiler.*` | `[Debug] Profiler` |
| `src/ddraw9/` | The Direct3D 9Ex backend: `directdraw.*` (IDirectDraw7, IDirect3D7, the exports), `surface.*` (IDirectDrawSurface7), `device.*` (IDirect3DDevice7), `vertex_buffer.*`, `gpu.*` (the Direct3D 9 device, presentation), `format.*` (pixel formats); `d3d9_api.h` puts Direct3D 9 into namespace `d9`, its headers clash with Direct3D 7's |
| `src/game/build.*` | Which exe this is, address lookup, the list of game modules |
| `src/game/sacred_addr.h`, `sacred_sigs.inc` | The sacred.exe addresses and struct offsets SacredBild uses; their signatures (generated) |
| `src/game/gameserver*` | The same for gameserver.exe, and its patches |
| `src/game/resolution.*`, `resolution_sites.inc` | Any resolution: window, back buffer, world view; the 1024x768 constants patched (generated) |
| `src/game/ui_canvas.*` | The 1024x768 UI in a scaled, centered canvas; cursor mapping |
| `src/game/ui_anchor.*` | HUD windows placed at the screen edges (`[UI] Anchor`, `[UI.Layout]`) |
| `src/game/device_proxy.*` | The IDirect3DDevice7 the game gets: UI draws into the canvas, world draws to the batcher |
| `src/game/frame_hooks.*` | Device creation, flip, world and UI render; frame limits |
| `src/game/focus.*` | Input only in the foreground, `ClipCursor`, close button |
| `src/game/movie.*` | Intro and cutscene movies through Media Foundation |
| `src/game/screenshot.*` | Print Screen as PNG / JPEG |
| `src/game/language.*` | Text and speech files of the game's language |
| `src/game/granny_async.*` | Animation update on a worker thread (`[Render] AsyncAnimation`) |
| `src/game/granny_mesh.*` | Granny's meshes, bone bindings and influence lists as its deform routine sees them |
| `src/game/gpu_skin.*` | `[Render] GpuSkinning`: Granny's deform reduced to the bone matrices, characters drawn by the backend's skinning shader |
| `src/game/skin_check.*` | `[Debug] SkinCheck`: Granny's skinning data read back and checked |
| `src/game/ground_quads.*` | The ground's quad batcher straight to the batcher (`[Render] BatchGround`) |
| `src/game/map_cache.*` | Hash index in front of the map record caches (`[Render] RecordIndex`) |
| `src/game/d3d_stats.*` | `[Debug] D3DStats` frame statistics |
| `src/game/controller.*` | `[Controller]`: the pad driving the game through injected input and its own cursor (walking, attacks, windows) |
| `src/game/aim_assist.*` | Controller targets from the game's list of what can be picked on the screen; the world pick hooked for walking and aiming |
| `src/game/ui_nav.*` | D-pad navigation: the controls of the open windows, told apart by the exe's RTTI |
| `src/game/options_screen.*` | The game's Options window as SacredBild's own screen while the controller is in use, with the controller's settings and bindings |
| `src/input/` | `gamepad.*` (SDL3 on a thread of its own), `inject.*` (keys and mouse buttons handed to the game's window procedure and polls), `input_mode.*` (controller or keyboard and mouse), `bindings.*` (`[Controller.Bindings]`) |
| `src/overlay/` | Dear ImGui drawn into the back buffer before each present (Direct3D 9 backend) |
| `src/game/world_passes.*` | `[Debug] D3DStats`: the world view's passes timed separately |
| `src/render/batcher.*`, `atlas.*`, `fvf.h` | Draw merging, the texture atlas, vertex format layout |
| `src/net/` | LAN games over VPNs: `lan_client.*` (sacred.exe), `lan_server.*` (gameserver.exe), `lan_protocol.*` (announcements), `adapters.*` (network adapters), `connection.*` (`NoDelay`, one send per message, TCP statistics) |
| `src/net/udp_*`, `tincat_shim.*`, `address.*` | `[Net] Udp`: `udp_endpoint.*` (the IPv4 and IPv6 sockets and their I/O thread), `address.*` (either family), `udp_transport.*` (KCP sessions), `tincat_shim.*` (TinCat's sockets on those sessions), `udp_protocol.*` (wire formats) |
| `src/net/matchmaker.*` | `[Net] Matchmaker` client: publishing (gameserver), listing and joining (sacred.exe) |
| `matchmaker/` | The matchmaking server (Go) |
| `tools/nettest/` | Test bench for the UDP connection and the matchmaker without the game: loss, delay, outages, address changes (`-DSACREDBILD_NETTEST=ON`) |

Every header starts with what its module does and why.

### Tools

Python 3 with `pefile` and `capstone`. They work on the English GOG exes, set with the environment variables
`SACRED_ENG` and `GAMESERVER_ENG` (defaults in `tools/gen_sigs.py`). With `SACRED_DE` and `GAMESERVER_DE` pointing
at the German ones, the generators and `check_hooks.py` check those as well; without them they skip them with a
warning, and the generated files name only the builds they were checked against.

Generated files in the repository, and the checks:

| Tool | Does |
|---|---|
| `tools/gen_sigs.py` | Writes `src/game/sacred_sigs.inc`, `gameserver_sigs.inc` and `tools/data/addresses.json`: a signature for every address, taken from the English build and accepted only if it matches exactly once in it and in the German one |
| `tools/gen_res_sites.py` | Writes `src/game/resolution_sites.inc`: the 1024x768 constants to patch, each verified and with a signature |
| `tools/check_hooks.py` | Checks that every hook declares as many stack arguments as the hooked function pops |
| `tools/gen_shaders.py` | Writes `src/ddraw9/skin_shaders.inc`: `src/ddraw9/skin.hlsl` compiled to vs_2_0 with `fxc` from the Windows SDK |
| `tools/sigs.py`, `funcs.py`, `eng.py` | Shared code: signature search, function boundaries and names (`tools/data/functions_eng.tsv`, exported from Ghidra), the English exe |

Reverse engineering helpers (how addresses and offsets were found):

| Tool | Does |
|---|---|
| `tools/eng.py <address> [count]` | Disassembles the English exe |
| `tools/rtti.py [regex]` | Class names -> vtables, from the RTTI |
| `tools/vt.py <vtable>...` | Vtable slots |
| `tools/scan_res.py` | Every instruction that uses a 1024x768-related constant |
| `tools/check_rets.py <address>...` | `ret` sizes, i.e. stack argument counts |
| `tools/profile_report.py <SacredBild-profile.txt>` | Names the hot spots of a `[Debug] Profiler` profile |

### Build

Requires Visual Studio 2026 (MSVC, Win32 toolset), CMake 3.25+ and vcpkg with `VCPKG_ROOT` set
(vcpkg provides [Detours](https://github.com/microsoft/Detours), [gtl](https://github.com/greg7mdp/gtl),
[Dear ImGui](https://github.com/ocornut/imgui), [SDL3](https://www.libsdl.org) and
[KCP](https://github.com/skywind3000/kcp)). The matchmaker needs Go 1.27 (`cd matchmaker && go build`).

```sh
cmake --preset msvc-x86
cmake --build --preset release      # -> out/build/msvc-x86/RelWithDebInfo/ddraw.dll
python tools/check_hooks.py         # checks the hooks against the exe (English, and German if set)
```

The DLL is statically linked against the CRT and imports only Windows system DLLs (`d3d9.dll` is loaded at run time).
vcpkg builds SDL3 with its gamepad support only (`triplets/x86-windows-static.cmake`, an overlay of vcpkg's triplet).
For development, symlink `ddraw.dll`/`ddraw.pdb` in the game folder to the build output instead of installing.
`.github/workflows/` builds every push and pull request; pushes to `main` publish a nightly prerelease, `v*` tags a
release.
