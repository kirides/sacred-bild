# Client mods for the Sacred Gear game server's Lua mods

Five mods in `gear/examples/` show what the SacredEngineRemake `Sacred.GameServer` Lua mods (`scripts-examples/*.lua`) know.
Install and the hotkey clashes of the defaults: [README.md](README.md). Their server side (script settings, chat commands, web
pages, screenshots) is documented in the server repository, `docs/mods/`.

**Common to all five.** They talk to the server over named `gear.net` channels with small JSON tables (see
[GearServerMods.md](../GearServerMods.md)). Only the server (slot 0) is listened to; every payload is checked and every text is cut
to a length before it is drawn. If the server has no custom net support (single player, the original `GameServer.exe`, a
server without the script) the mod stays idle and, where it has a panel, says so. They are not tested in a running game by
this documentation (see "Screenshots"); the layouts below are read from the code, in the 1024x768 layout, and panels can be
dragged.

**Screenshots: none of the panels below could be captured** (they are drawn inside the running game, which was not
available). Nothing here is mocked up.

## bounty - Bounty board

Shows the bounties of `bounty_board.lua`: what to hunt, your progress, the time left and the reward.

- **Install:** `gear/examples/bounty` into `mods\bounty`. Server: `bounty_board.lua`.
- **Hotkey:** `panel`, default **F8** (also the game's quick-load key; rebind it).
- **Settings:** *Tracker* (on by default): a few lines with the progress of your open bounties. `mod.lua` `config`:
  `tracker_x`/`tracker_y` (780, 150 in the 1024x768 layout), `refresh` (60 s between asking the server; the server also pushes
  every change).
- **Channel:** `bounty:state`. The server sends `{ v = 1, next_in, bounties = {{ id, title, text, region, target, n, done, left,
  reward, claims, level }} }` after the handshake and on every change of the board or your progress; the mod also asks with
  `{ req = 1 }`. `left` is relative seconds, so the clocks need not agree.
- **Layout:** panel "Bounty board (F8)", 400 x 320 at (300, 70), resizable (minimum 300 x 160), closed until F8. A scrolling list;
  each bounty is a 58 px row: the title in gold (bold) with the time left at the right (green "done" when finished), the
  text ("Defeat 32 orc creatures", with "(level N+)" when it has a minimum level), a progress bar with `n / target` centered in
  it (gold, green when done) and the reward line. Texts instead of the list: "Waiting for the server...", "The board is empty.", or
  the explanation when the server has no bounty board. The tracker is "Bounties" in gold at the top right, one line per open bounty
  as `title  n/target`, shown in the game (not while a game window is up) while a board is known.
- **Limits:** needs the Sacred Gear game server with `bounty_board.lua`. Progress shown is what the server counted.

## achievements - Achievements and login streak

Toasts for every unlock and for the daily login reward of `achievements.lua`, and a panel listing all achievements with progress.

- **Install:** `gear/examples/achievements` into `mods\achievements`. Server: `achievements.lua`.
- **Hotkey:** `panel`, default **F7** (clashes with `damagemeter`, `serverfeed`, `buildsharer`).
- **Settings:** *Show toasts* (on), *Toast time* (6 s, 2 to 15). `config`: `panel_x`/`panel_y` (590, 40).
- **Channel:** `achievements:sync`. Server to client: `{ op='state', ach={{id,name,desc,have,need,done,reward}...}, streak, best, days,
  claimed, next }`, `{ op='unlock', id, name, desc, reward }`, `{ op='streak', streak, best, reward }`. Client to server: `{ op='get' }`,
  sent once the handshake is done, when the panel opens and every 30 s while it is open.
- **Layout:** toasts: up to four boxes stacked from y = 14, centered at the top (340 px wide, black with a gold frame, fading in
  over 0.2 s and out over the last 0.6 s): a gold title ("Achievement unlocked" or "Daily login: day N"), then the name, the
  description and the reward, or the reward and the best streak. Panel "Achievements (F7)", 410 x 480, resizable: a gold line "Login streak:
  N days   best N   N days visited", a grey line with today's or tomorrow's reward, "N of 31 unlocked", then a scrolling list of
  40 px rows, one per achievement: name (bold) and description, at the right "Done" in gold, or `have / need` with a small progress
  bar. Unlocked rows have a gold-brown background.
- **Limits:** the server unlocks from what it sees (see the server doc: no gold, exploration, quests, items found). Without the
  script the panel says "This server has no achievements."

## serverfeed - Server feed

Rare and unique drops (`rare_drops.lua`) and hero deaths (`graveyard.lua`) as lines on the screen, and a list of the latest.

- **Install:** `gear/examples/serverfeed` into `mods\serverfeed`. Server: `rare_drops.lua` and/or `graveyard.lua`.
- **Hotkey:** `history` (show or hide the list), default **F7** (clashes with three others).
- **Settings:** *Rare drops* (on), *Hero deaths* (on), *Seconds on screen* (8, 3 to 30), *Position: right edge* (1008) and *top* (180).
- **Channels:** `rare_drops:feed` (`{kind='drop', drop={item,tier,rarity,by,from,time}}` or `{kind='recent', drops}`) and
  `graveyard:feed` (`{kind='death', name, level, by, killer_kind, time}` or `{kind='recent', deaths}`). When the handshake is done the mod
  sends an empty request on each; the server answers with the latest five, which only fill the list (no lines on screen).
- **Layout:** no panel. Live messages are right-aligned text lines, 420 px wide, 20 px apart, on a translucent black background
  at the right edge from the top position: drops in the game's rarity colours (unique gold-brown, rare yellow, magic blue),
  deaths in red; at most five at once. The list (key on) comes below them: a heading "Latest from the server (F7 hides)" and
  the latest twelve, or "nothing yet". Examples: "Lyra Dawnbrook found a unique Stormcaller's Helm, dropped by Skeleton Lord";
  "Sir Plinth (level 34) was slain by Orc Warrior" (fictional names).
- **Limits:** a server without the scripts never sends; a stock `GameServer.exe` drops the handshake, so the mod stays idle.

## buildsharer - Build sharer

Publish your hero build to the server and browse the builds other players shared.

- **Install:** `gear/examples/buildsharer` into `mods\buildsharer`. Server: `build_sharer.lua`.
- **Hotkey:** `toggle`, default **F7** (clashes with three others).
- **Settings** (section "My build"): *Build title* (40 characters), *Note* (120), *Tags* (comma separated, up to 5 words, 80 characters),
  *Share carried items* (on: the names of the best up to 12 items in your backpack; the server lists them as "reported"). `config`:
  `page_size` 12.
- **Channel:** `buildsharer`. Requests `list {q,class,sort,page,per}`, `get {id}`, `mine`, `publish {title,note,tags,carried,level}`,
  `unpublish`; answers `list`, `build`, `mine`, `published`, `unpublished`, `error`. At most one request per 0.4 s (the server limits
  bursts too).
- **Layout:** panel "Build sharer (F7)", 560 x 430 at (80, 60), with four tabs: **Browse** (a Class dropdown, a Sort radio
  "Newest / Highest level", Refresh, `<` and `>` buttons, a scrolling list with one button per build such as
  `Lyra Dawnbrook  Seraphim  L87  [verified]  - Smite and shield`, and "N builds, page X of Y"); **My build** (what you publish: hero,
  class, level, your title, note and tags, the number of carried items, buttons *Publish*, *Remove mine*, *Check server*, and what the
  server holds of yours); **Saved** (builds you kept, in `saved_builds.json` in the mod's folder, at most 50); **Detail** (the picked build:
  badge "server-verified" or "reported by the player", title, note, a *Save to Saved* button, then Skills, Combat arts, Equipment with tier
  and item level, Carried, Tags). A status line in the bottom shows errors such as "wait a few seconds before publishing again".
  Opening the panel asks the server for the first page and for your own build.
- **Limits:** Gear cannot read a hero's skills or worn equipment, so only a server with hero data (web UI or `--authoritative`) can
  give a "server-verified" build; otherwise a build has class, level and what the mod reports. No ratings, comments or import.

## questhelper - Quest helper

Quest items near you and in your backpack (read by the mod) and the quest records of your hero from the server.

- **Install:** `gear/examples/questhelper` into `mods\questhelper`. Server: `quest_helper.lua` with hero data (web UI or `--authoritative`).
- **Hotkey:** `toggle`, default **F8** (also the game's quick-load key; rebind it; clashes with `bounty`).
- **Settings:** *Show change notices* (on): a line on the screen when a record is new or changed; *Refresh while open* (30 s, 5 to 120).
- **Channel:** `questhelper`: `state { watch }` and `unwatch` to the server; `state` and `error` back. While the panel is open the
  mod asks to be watched and the server pushes when a record changes.
- **Layout:** panel "Quest helper (F8)", 520 x 400 at (80, 60), three tabs. **Quest items**: "In your backpack" (name and size in
  cells, or "no quest items") and "On the ground within 40 tiles" (name, distance in tiles and position, nearest first). **Records**: a
  line "N records, N with a catalog entry (the catalog has N)", a *Show unnamed records* check box, *Refresh*, and a scrolling list; a
  record is `name = value - label` (gold when the owner named it, `record <key>` otherwise) with "Where:" (area), the hint wrapped, and
  "Also:" (up to 6 other online heroes who have it). Without data the tab shows the server's reason. **About**: a text on what the panel can
  and cannot know. The change notice is a gold line at the top center ("Quest record: <name> (new)" or "(changed)") for 8 seconds.
- **Limits:** Gear cannot read the quest journal and the server cannot either: there is no list of active or available quests, no progress,
  and record keys have names only where the server owner filled the catalog. The panel says so instead of guessing.
