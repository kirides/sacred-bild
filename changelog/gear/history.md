## v0.14.1 (2026-11-25)

- Fixed: windows made with `gear.kit` that have no width of their own (the on-screen keyboard) shrank to nothing: a document's body is only as wide as its content, so an absolutely positioned window found a containing block 0 wide. `kit.page` now gives the body the full width, so such windows size themselves from their content.

## v0.14.0 (2026-11-25)

- New: `gear.npc`, custom NPCs for mods. A kind (`npc.define`) takes the model of any object type of the game, a name of its own, dialog pages, a daily routine and a tint; `npc.spawn` makes a friendly creature of it. The creature shows its name over its head, and the game keeps the name with the savegame, so the NPC is found again after a load (`npc.list`, `npc.of`).
- New: dialog pages like the game's: click an NPC and the hero walks up to it, then a wide frame in the game's own dialog style shows the NPC's portrait, name and text with a button per option along the bottom (green, yellow under the cursor). A mod reacts to a button with the option's `on`, the kind's `on_talk` or `npc.on('button')`, for example to open a window of its own ("buy a pet", "trade in rare materials"); `e.go(page)` and `e.close()` steer the dialog.
- New: routines. An NPC stands at a point from a time of the game's clock on; when the next point's time comes it walks there if the hero is near, is put there if something blocks it, and is put there at once when the hero is far away. `gear.world.clock()` reads the clock the day and night cycle runs on.
- New: `gear.text.key(key, text)` makes a text of Gear's own whose id comes from a key, the same in every session; `creature:set_name_text(id)`, `name_text()` and `name_key()` give any creature (not a hero) a name of its own, which the savegame keeps. `creature:display_name()` now returns it.
- New: `creature:set_tint(spec)` multiplies a colour over a creature's model, whole or per mesh (for example the cloth); needs SacredBild 0.3.4 or later.
- New: `creature:set_texture(spec)` draws a creature's model with other textures, whole or per mesh: an image file of the mod (PNG, TGA, JPG or DDS, sides a power of two) through `gear.ui.texture.load(path).picture`, or a texture of the game's Texture.pak (`gear.ui.texture_image`). NPC kinds take `texture = 'skin.png'` or `{pak = 'NAME.TGA'}` and apply it again after every load. `gear.ui.texture` handles have a `picture` field.
- New: hostile NPCs, for bosses. An NPC can be spawned `stance = 'neutral'` (an enemy of nobody) or `'friendly'` (default), and `npc.hostile(creature, opts)` or the `hostile` option of a dialog button turns it into an enemy of the hero that attacks (the library orders the attack: created creatures do not notice heroes). Options: `level` (health, damage and attributes follow from the type and the level), boss modifiers (`health`, `damage`, `taken`, `deflect`, `reflect`, `regen`), a `weapon` it wields (or `{main, off}`) and a `tint`; `npc.calm` takes it back, the `hostile` and `calm` events tell mods. Modifiers and tint are set again after a load.
- New: `creature:allegiance()` and `creature:set_allegiance{faction, aggressive}` read and change a creature's faction and aggression; `creature:carry(slot, item)` puts a spawned item into a creature's equipment slot (a weapon in slot 12 is wielded, as the model shows).
- Fixed: clicking an NPC far away walks the hero up again: the engine ends the hero's walk after the next waypoint without a held mouse button, so the order is sent again every few tenths of a second. An NPC on a routine whose first walk order the engine dropped gets it again (three orders over 4.5 seconds) before it is called blocked and put at its point.
- New: `gear/examples/npctest` and `npcsavetest`, in-game tests of the above.
- Under the hood: `World::setNameText` writes the creature's name text id (+0x3C), `gear.world.clock` reads the game's calendar (hour at +0x20, minute at +0x22 of the object 0x417560 returns).
- New: `gear.kit.osk`, an on-screen keyboard for mod screens, and `kit.ibtn` (the keyboard icon button). A small window of its own with bordered keys, a number pad (`digits`), a longest text (`max`), Paste with clean-up rules and the pad and the mouse on every key; `kit.field` makes a text input whose keyboard button shows only while the controller is in use (A on the field opens it); the same widget works in the plain base style and in the sacred-theme look. The mod manager's text settings use it, and `gear/examples/rmlosk` is a demo and in-game test.

## v0.13.0 (2026-11-25)

- New: `gear.shop` and the `shop.stock` event let a mod add wares to merchants and take them out again. The game fills a merchant's three stock pages when another merchant is opened and only swaps a few wares when the same one is opened again; the event fires right after the game did that (once per page, with the reason `new`, `refresh` or `restock`), so wares a mod adds stay until the game resets the page and a shop is never rerolled just because it was opened again.
- New: `gear.shop.add{category, quality, level}` stocks a ware through the game's own stocking code (the item picker for the category, room in the grid, the item's random properties for the level and quality you ask for); `type` stocks an exact item type. `gear.shop.info()`, `items()`, `container()`, `pick()` and `remove()` read and edit the pages.
- New: `gear.shop.CATEGORY` names the equipment categories (`WEAPON`, `CHEST_ARMOUR`, `RING`, `SHIELD`, `HELMET` and so on) so mods need no bare numbers for `gear.shop.add{category = ...}`.
- New: `gear/examples/shoptest`, an in-game test that opens a merchant, adds and removes wares and checks that a reopened merchant keeps its stock.
- Under the hood: `gear/shop.cpp` wraps the merchant window's event handler (0x6DAD90) and its clear, restock, one-item stocker and item generator, found by signature.

## v0.12.0 (2026-11-25)

- New: DDS images in mod documents (`<img src="x.dds">`, `image(x.dds)`): BC1, BC2 and BC3 (DXT1/3/5, also as DX10 files) go to the GPU as they are, with no decoding and a quarter to an eighth of the memory of a PNG; BC4, BC5, BC6H, BC7 and the usual uncompressed formats are decoded once on load. Only the top mip is read.
- New: packs and layers. A mod lists ZIP packs in `mod.lua` (`vfs = {{file = 'resources/data01.pak', mount = '/'}, ...}`); a later layer wins over an earlier one, a file on disk wins over all, and the layers are merged into one ordered index when the mod loads. Documents, style sheets, templates and images are read from it with no file open per file, and `gear.vfs.read/exists/list` give Lua the same view. Without `vfs`, every `*.gpak` in the mod's folder is mounted at `/`. One file to copy and download instead of thousands (the minimap's map tiles).
- New: `gear.ui.texture.load(path)`: an image file of the mod as a reference counted texture, shared by every mod that asks for the same file (`tex.src` is its image source in documents, `tex:release()` lets go; a stopped mod's textures are released). The last release frees the texture: RmlUi's copy is purged and the GPU resource is destroyed once the frames that may still draw it are done (`Ui::setFramesInFlight`, 2 by default).
- Fixed: image files of mod documents stayed in memory (and on the GPU) for as long as the game ran. One that no frame has drawn for 8 seconds is released and loaded again if it is needed later (the minimap's map tiles, which piled up as the hero walked).
- Under the hood: `gear/dds.cpp` (with bcdec, public domain) reads the files and `gear/tests/dds_test.cpp` checks every format against texconv output.

## v0.11.1 (2026-11-25)

- New: mod manifests can list `recommended` mods; Gear ignores them, the mod manager offers the missing ones on install

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
