## v0.11.0 (2026-11-25)

- New: `creature:set_level(level)` sets a creature's level, stats and maximum health, keeping its share of health (not heroes).
- New: `creature:set_pet{scale, untargetable}` draws a creature larger and/or leaves it out of world clicks and hover; needs SacredBild 0.2.14 or newer.
- Improved: `creature:order_move` and `creature:order_stop` also work on any creature in single player, not only party heroes.
- Fixed: a companion you play no longer fights back or steps away on its own, also after loading a savegame.

## v0.10.0 (2026-11-25)

- New: `gear.buffs` `speeds` and `speed_factors` lift the engine's 220 cap on attack, cast and move speed; read them with `gear.buffs.speeds(target)`
- New: `gear.game.map_markers()` lists the world map's service symbols: merchant, blacksmith, combo master and horse trader
- Fixed: the Paste button shows only over an editable box of an open window, not on the savegame quicksave box or while loading

## v0.9.0 (2026-11-25)

- New: `creature:copy_stats(source)` gives a creature the level, stats and health share of another creature
- New: `gear.world.spawn` option `like` also copies the source's level, stats and health share, not only its allegiance
- New: RmlUi text fields take Ctrl+C, Ctrl+X, Ctrl+V and Ctrl+A, mouse selection, Shift+arrows and double-click word selection
- New: Ctrl+V pastes the clipboard into the game's own text boxes, as the Paste button does
- Fixed: an RmlUi document that stops being drawn gives up its focus and keyboard capture, so typing reaches the game again
- Under the hood: new `clipboardtest` example; kernel tests for `spawn { like }`; engine signatures for setting a creature's level

## v0.8.0 (2026-11-24)

- New: icon font in every RmlUi document, `<span class="icon icon-refresh">` with 85 icons, and `gear.kit.icon(name)`
- New: `gear.kit.dropdown(id, label)` builds a drop-down button in the game's look; the list it opens is the mod's
- New: `data-close-key="<key>"` on a document button closes its window through `gear.keys.trigger(id)`; the pad's B does too
- New: `?rot=90`, `?rot=180` and `?rot=270` on game image sources turn the picture
- Improved: Mods screen choice settings are drop-downs with a list of the options, not an arrow stepper
- Improved: the plain base style gets compact and icon buttons, drop-down classes and small utility classes
- Improved: the Paste button over the game's text boxes is a grey paste icon
- Fixed: CSS transforms in documents (hover scale, turned arrows) were placed off by the document's origin
- Fixed: plain-style drop-downs and the Mods screen slider, check box and drop-down edges line up
- Under the hood: `gear.auto.rml_docs(mod_id)` lists open documents so tests can walk another mod's pages

## v0.7.0 (2026-11-24)

- New: `gear.party.experience_scale(factor)` multiplies the experience heroes gain from kills and quests (2 = double, single player)
- New: `Creature:casting()` returns the art or spell a creature has in flight, nil when none
- New: `xptune` example mod: scales experience and creature health and damage from its config
- Fixed: RmlUi screen text left of or above the 1024x768 layout (on wide or tall screens) no longer disappears

## v0.6.0 (2026-11-24)

- New: RmlUi screens accept web-style CSS: `@font-face` with `url()` fonts, and `::before { content: ... }` for icons
- New: `gear.kit`, ready-made RmlUi markup for windows, tabs, buttons, check boxes, sliders, rows, lists and confirm dialogs
- New: RmlUi documents share templates, styles, images and fonts between mods via `exports` in mod.lua and `@mod/path` links
- New: hot reload of RmlUi documents for developers (an empty `mods\.rml-hot` file or `GEAR_RML_HOT`), with an `on_reload` callback
- Improved: first start without `gear_mods.json` switches on only sacred-theme, key-bindings and better-saveload
- Under the hood: new examples rmlcss, rmlkit, rmlshare and rmlshare-lib; kernel tests for the kit and shared resources

## v0.5.0 (2026-11-24)

- New: Paste button on the game's focused text boxes (savegame and character names, chat, search) types the clipboard
- Fixed: `creature:use_weapon_slot` refuses while the hero wears weapons outside the active slot instead of losing them
- Fixed: importing a hero from a file no longer moves other heroes, or the one you play, to the file's saved spot
- Under the hood: native UI hooks for the focused text field and typing into it, used by the Paste button

## v0.4.4 (2026-11-24)

- Fixed: Mods screen sliders no longer treat the screen's own value updates as the player's changes
- New: slider change events in mod UI carry user = true only for the player's own moves (mouse, drag, D-pad)
- Under the hood: slidertest automation example, and mods.query attr and text lookups for slider values

## v0.4.3 (2026-11-23)

- Fixed: Mods screen slider settings were reset on every show of the screen; they keep their values now

## v0.4.2 (2026-11-22)

- Fixed: SacredGear.dll's file properties showed version 0.1.0; they now show the release and git revision
- Under the hood: the DLL's version resource is generated from the version file and the git revision

## v0.4.1 (2026-11-22)

- New: text fields in the Mods screen's settings are pill-shaped with a blinking underscore cursor
- New: theme_dp manifest key gives screen-pixel mod UI its own stylesheet with dp lengths
- New: `?dp` picture sources take image sizes in layout units for documents laid out in screen pixels
- New: caret-style property (block or underscore) for RmlUi text carets, with the patched RmlUi
- Improved: mod UI text is rasterized at the exact scaled font size; SACRED_FONT_HINT=0 turns hinting off
- Improved: gear.pfx colour changes also apply to ground discs and magic circles
- Fixed: skill effects made by another effect now find their caster, so per-caster rules apply
- Under the hood: fxframes automation example and a GEAR_PFXTRACE=1 research trace for gear.pfx

## v0.4.0 (2026-11-22)

- New: gear.classes.art_icon(id, state) returns the disabled, cooldown or available icon of an art or spell
- New: mod UI documents can use origin = 'screen' to start at the screen's top left corner, not the layout's
- Fixed: a bad pointer read during the game's UI or object walks no longer kills the game
- Improved: icon_frame fields in the ability data are documented as the disabled, cooldown and available icons
- Under the hood: shared memory-fault filter used by the speculative reads in ui, tables and classes

## v0.3.0 (2026-11-22)

- New: gear.pfx recolours, retextures and speeds up the particle effects of arts and spells
- New: gear.pfx rules pick an effect variant by art or spell, effect type and cast level
- New: pfx.select event lets a mod choose the effect variant for each effect in code
- New: gear.classes.define_rune makes rune items for ability copies that can be spawned, traded and read
- New: gear.classes.rune_art_of_type and rune_type_of look up the art a rune teaches and a copy's rune
- New: creature:use_item(item) makes a creature drink a potion or read a rune or scroll from its backpack
- New: pfxtest example mod and a skill effects guide with screenshots
- Under the hood: effect setup and particle manager hooks for gear.pfx

## v0.2.8 (2026-11-21)

- New: `master.price`, `master.layout`, `master.exchange`, `master.describe` and `master.maximum` events change the combo master's rune exchange page.
- New: `item.use` event: return `'consume'` or `'keep'` to change what a drunk potion does; drinks a mod starts keep the engine's effect.
- New: `potion.pick` event lets a mod choose the item type of random potions, such as merchant stock and loot.
- New: `kill.recorded` event fires for every kill by the hero or its minions and summons, also kills that give no experience.
- New: `gear.world.open_teleporter(sub)`, `gear.world.portals(mask)` and `gear.world.show_text(text_id, color)` for portals and HUD text.
- New: `creature:raise(ability, levels)` raises the level of a known art or spell with the game's own function.
- New: `creature:set_combo_charge(slot, charge)`, `creature:in_combat()` and `creature:apply_effect(effect, value)`.
- Improved: RmlUi documents are as large as the screen when it is bigger than 1024x768, so elements at the edge keep their text.
- Improved: the Mods window is centred on the whole screen, and the escape-menu Mods button sits under the menu window.
- Under the hood: engine addresses and signatures for the new hooks, the combo master page and the teleporter list; merge repairs in the Lua host and kernel.

## v0.2.7 (2026-11-21)

- Improved: a button's controller prompt takes room only while the pad is on it, so idle buttons have no gap in the theme and base style.

## v0.2.6 (2026-11-21)

- Fixed: controller prompts showed on every element that contains the focused button; now only on the button or link the pad is on.
- Improved: the D-pad leaves a scrolling list at its top or bottom end and moves on to the next control.
- Under the hood: `gear.auto.nav_kinds()` reports the kind of each controller stop (button, slider, scroll area, list end).

## v0.2.5 (2026-11-21)

- New: every RmlUi box that scrolls is a controller scroll area, not only lists marked `data-nav-scroll`.
- New: `data-nav-home` on a control sets where the controller cursor starts when a document opens, such as the top savegame.
- Under the hood: `gear.auto.nav_first()` reports the first controller stop; the mod manager self test checks that the D-pad scrolls its list.

## v0.2.4 (2026-11-21)

- New: `gear:image/<id>?point` in RmlUi documents samples a picture by the nearest texel, so pictures scaled up stay crisp.
- Fixed: scroll bars of RmlUi lists did not take the mouse; the knob, the groove and the arrows respond now.
- Fixed: the arrows of the theme's scroll bars pointed sideways; they point up and down now.
- Fixed: mod code on the main menu ran only four times a second; it now ticks every frame there.
- Improved: every RmlUi button gets its own controller prompt (A on the focused one); `data-no-padkey` opts out.
- Improved: controller stops follow stacking order, so a dropdown list over rows takes the pad and the mouse first.
- Under the hood: the main-menu tick is split out of the idle tick in the Lua host.

## v0.2.3 (2026-11-21)

- New: the Mods screen (F11 or Mods in the escape menu) is an RmlUi document with check orbs, tabs and on-screen keys for a controller.
- New: controller LB / RB switch a mod's Settings, Keys and More tabs and page the Mods list; Select / "-" opens the screen.
- New: `gear.ui.page_pressed()` and `gear.ui.select_pressed()` let a mod read the LB / RB and Select presses.
- New: `gear.emit('mods.open', {mod = 'id', tab = 'settings' | 'keys' | 'page'})` opens the Mods screen on a mod's own tab.
- New: the D-pad walks RmlUi lists marked `data-nav-scroll` card by card and scrolls each card into view.
- New: RmlUi documents get a plain built-in base style when no theme mod runs; `{theme = false}` still opts out.
- New: theme class `arrow` (`<a class="arrow next">`) for steppers of values and pages, with the game's white arrows.
- Fixed: a built-in mod's RmlUi documents failed without the theme mod; they now draw with the base style.
- Improved: the savegame list checks each save's thumbnail once a second instead of every frame.
- Under the hood: the mod manager self test clicks through the RmlUi screen and reads its element positions from a query event.

## v0.2.2 (2026-11-21)

- Fixed: controller messages were lost after a game started, because the key block took over SacredBild's window procedure.
- Fixed: RmlUi text was blurry on screens larger than 1024x768; glyphs are now rasterized at the size they are shown.

## v0.2.1 (2026-11-21)

- New: `creature:copy_allegiance(source)` gives a creature the faction and aggression of another one (single player).
- New: `gear.world.spawn` takes `like = creature` to copy that creature's allegiance; the new creature is removed if the copy fails.
- New: `gear.auto.object_bytes(id, offset, size)` returns an object's raw bytes as hex, for self tests and reverse engineering.
- Improved: `creature:order_attack` works on any creature, such as a monster, in single player, not only on party heroes.
- Under the hood: kernel tests cover the allegiance copy and the `like` option of `gear.world.spawn`.

## v0.2.0 (2026-11-21)

- New: gear.ui.rml loads HTML/CSS documents (.rml) from a mod folder, with rml.load, rml.load_string and rml.load_font.
- New: RmlUi document API: doc:on for events, doc:query and query_all for element handles, doc:draw() each frame.
- New: data binding for RmlUi documents: opts.model, doc.model and data-event-* callbacks.
- New: gear.ui.drag_source, drop_target and world_drop: drag and drop between mods, with drop hints and world drops.
- New: gear.menu API (add_item, remove_item, items, host) lets mods add items to the game's Esc menu or draw it.
- New: gear.ui.menu_entries and menu_click read and click the game menu's entries; page_rml opens a mod page as RmlUi.
- New: the game's own fonts, thick frame style, game-button and game-slider classes for RmlUi documents.
- New: gear.party.shared_purse, give_gold and class_portrait: per-hero gold and gold transfers in single player.
- New: mods load in the player's order from gear_mods.json, and a theme mod's stylesheet applies to every RmlUi document.
- Improved: modal RmlUi documents take the mouse and controller alone, and Esc cancels a drag without opening the game menu.
- Fixed: the thick frame's fill and pieces no longer leave gaps at any UI scale, and RmlUi images load from file URLs.
- Under the hood: per-tick UI tables and RmlUi paths reuse their tables, and RmlUi listeners are delegated.

## v0.1.1 (2026-11-19)

- New: creature.created event for creatures the engine spawns itself (sector fills, spawn tables, scripts, respawns).
- Under the hood: kernel test checks creature.created delivery: once per creature, with its type and position.

## v0.1.0 (2026-11-19)

- New: Sacred Gear, a Lua mod kit (SacredGear.dll): mods are folders with a mod.lua that react to events and timers.
- New: World API (gear.world): spawn, move and read objects, heroes and creatures, plus items and pickup rules.
- New: Mod UI (gear.ui): drawn rectangles, game-font text, images, widgets, scroll areas and controller support.
- New: Layout editor: every mod's panels can be moved, and their place is kept between sessions.
- New: Mod manager (Mods screen) with declared settings, key binds, and switching mods on and off live.
- New: File overlay: mods replace game files and patch its text and word tables from mods/<id>/overlay.txt.
- New: Inventories (gear.inv) and the savegame API (gear.game save, can_save, save_info) with game.saved events.
- New: Combat and creature events (damage, creature.ability, combat.branch, bonus.constant) through site hooks.
- New: Party, buffs, classes, drops and loot filter APIs (gear.party, gear.buffs, gear.classes, gear.drops).
- New: Multiplayer: gear.mp players and kill events, realm choice for online play, parallel -gear-instance runs.
