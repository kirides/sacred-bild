# Custom classes: engine facts and the portability tiers

A custom class is a profile on top of one of the 8 hero bodies (creature types 1..9): its skills, the arts it may learn and
the motion clips it plays. Sacred Gear's part is the set of hooks the mod needs; this page records what the engine does and
what was measured (English GOG `Sacred.exe`, test: `src/gear/examples/classspike`, a copy of the game folder).

## How the engine binds a class

| What | Where |
|---|---|
| Class of a creature | `classOf` 0x5500D0 (fastcall, type at creature +0x10) -> 1..9. Action ids per class: `[0, 9, 8, 12, 10, 11, 13, 13, 14, 15][class]` in the tables 0x9555AC, 0x95583C, 0x956C48 |
| Where it matters | 14 call sites compare the creature's action word (+0x150) with that id, and write it when an art starts; the action jump table 0x539E20 then runs the class handler (Seraphim 0x5038F0, Gladiator 0x4FF0B0, Battle Mage 0x50CD10, Dark Elf 0x506770, Wood Elf 0x50AC00, Vampiress 0x50E6C0, Dwarf 0x514790, Daemon 0x5192E0) |
| Art in flight | word at creature +0x120 (ids from 1000 are combat arts) |
| Learned abilities | vector at stats +0x102 (creature +0x4AA), 0x16-byte entries {u32 kind, u16 id, u8 level, u8 bonus, ...}; appended by 0x580750 (thiscall, entry by value) |
| Spell motion slot | 0x522030 picks the slot (e.g. fireball 155); only caller starts the motion |
| Motion player | 0x593320 (thiscall: creature, slot, three floats, ret 0x10) -> model manager getMotion 0x4118B0; a slot whose record is 0 has no clip (slots 0x4F..0x99 fall back to 0x59/0x54/0x4F, the rest play nothing) |
| Slot tables | model manager (global 0xAA4538): model records at +0x48, 0x4AA bytes each (name at +0, 256 u32 motion record indices at +0x70), motion name records of 0x100 bytes at +0x54 |

## What Gear adds (sacred-bild)

* `ability.route(type, art, vanilla_class, action, id)` : hook on `classOf`. Asked only for hero types (1..9) while an art
  (id >= 1000) or a spell copy (100..199) is in flight (the game's own spells 1..99 never ask); the handler returns the hero class whose code runs the art, or nil.
* `motion.select(type, slot, id)` : hook on the motion player (hero types only); the handler returns another slot or nil.
* `gear.models.has_motion(body, slot)`, `motion(body, slot)`, `set_motion(body, slot, record)`, `restore()` : the slot tables.
* `creature:learn(id, level)`, `creature:forget(id)`.

## Measured (stage 0 spike)

Test hero bodies were recruited as companions (`gear.party.recruit`); results from `SacredGear.log` of the spike run.

| Case | Result | Tier |
|---|---|---|
| Seraphim learns Battle Mage spells 5, 8, 34 (`creature:learn`) and casts them | Works at once; the spell engine picks the Seraphim's own cast slots 155 / 160 / 157; cast takes 1.5-2.7 s | A |
| Gladiator casts spell 5, body has no cast clips (slot 155 empty) | Cast is aborted after 0.4 s, no effect | needs remap |
| same, `set_motion(2, 155, <Seraphim's record>)` (foreign clip) | Cast plays 1.5 s like the Seraphim's; skeleton is shared (`Bip01`) so Granny binds the clip | A |
| same, `motion.select` 155 -> 146 (the Gladiator's own special clip) | Cast plays 1.5 s | A |
| Seraphim uses Gladiator arts 1058-1060 **without** routing | Art id unknown to the Seraphim handler: no or generic action (1059 loops as an ability action with the Seraphim's slot 146; 1058 and 1060 do little) | needs routing |
| same **with** `ability.route` -> 2 | The Gladiator handler runs: action word becomes 8, slots 201 / 210 are chosen, the art hits (a foe lost 27 hp in one run with only that hero acting) | B |
| same with routing and `motion.select` remap of the weapon-art slots the Seraphim lacks (201-204 -> 207, 210 -> 211) | Art runs and ends normally in 0.3-0.4 s | B |

Notes: arts that need a weapon (1057 Rundumschlag on an unarmed recruit) do nothing in any case; a class profile must
give an eligible weapon. Arts tied to class state stay tier C (Vampiress forms 1022/1023/1030-1037, Daemon forms
1097-1100, Dwarf firearms and stance arts) and monster-only actions tier D. The editor shows the tier per body from this
table; arts that were not run in the spike are rated from the home class' handler and the clips the body has, and stay
"untested" until a profile run confirms them.

## The API (stage 1)

Reference: [GearApi.md](GearApi.md) (`gear.classes`, `gear.models`, `creature:learn`, `gear.text.add`, the events).

* `rune.usable(type, ability, vanilla)` hooks the two class checks of a rune read (0x4D1B20 for arts, 0x4D24C0 for spells; the mask is at
  +0x70 / +0x8C of the ability record). A handler answers true or false per hero type and ability.
* `gear.classes.set_hero_skills(body, start, available)` writes the two start skills (+0x14) and the 16 available skills (+0x16) of the
  Creature.pak hero record of a body (u8 ids). The record belongs to the body, so it applies to every hero of that body at the same time;
  the `custom-classes` mod switches it when the controlled hero changes. This replaces the planned per-hero `hero.skills` event: the
  record reads are spread over many functions, one record write is exact.
* Ability copies (`gear.classes.copy_ability`). The class handlers `switch` on the first word of the ability record that the lookups 0x4D1AF0
  (arts) and 0x4D2310 (spells) return. Gear hooks both: for a copy's id they return a copy of the base record with the base id left in the
  first word, so the base ability's code runs and the copy has its own numbers, name and description. Limits found on the way: the engine
  refuses an art in flight with an id above 5000 (0x4FD0F0), so art copies are 2000..4999. Copies of spells (100..199) were tried: the engine accepts the order and does nothing, so
  `copy_ability` refuses them. The engine's text
  table only changes texts that exist, so `gear.text.add` serves new texts (ids from 0x100000) from the text lookup (0x672C90).
* `creature:learn` calls the engine's own append (0x580750); `forget` removes the entry from the abilities vector.

Tested: `src/gear/examples/classtest` (26 checks, one run in a copy of the game folder): catalog with tiers and rune items, hero skills, a copy of
Gladiator art 1059 with changed numbers learned and used by a recruited Gladiator, copy of a spell, text, forget.

## Notes from the mod's runs

* A Gladiator art ordered for a Seraphim with the class's routing runs as an `ability` action (the plain attack without it). Whether
  it hits was not isolated: arts that need a weapon do nothing for an unarmed hero in any case, so treat tier B as "the class's code runs".
* Ability copies (`gear.classes.copy_ability`) were run by a Gladiator (copy of 1059 with new numbers and its own name), and a chain
  built on them (a copy of its first step, the next steps ordered by the mod).
* The ability regeneration is `regen_base + level * regen_per_level` of the record (T1 f32 at +0x40 / +0x44, T2 u16 at +0x3E / +0x40).

## Icons, skills and texts (UI)

* The icons of abilities are textures of `PAK	exture.pak` named in the ability record (`+0x0C`, e.g. `GUI_MOVE_RUNDUM.TGA`, 64 x 64 grey discs).
  `PAK\Texture.TMP` lists every texture; a record's index is the texture manager's handle, so the engine loads and decodes it.
  `gear.ui.texture_image(name)` gives an image id for `gear.ui.image`; `gear.classes.art_icon(id)` does it for an ability and also
  returns a tint (arts gold, spells pale blue). Copies have their base's icon.
* Skills have no table in the exe: the skills window computes the name text id as `0x24B7 + skill id` and the description as
  `0x24E9 + skill id`. `gear.classes.skill(id)` / `skills()` return them in the game's language. The game has no skill icons.

* `ability.hit(attacker, victim, art, damage)` (an event of the damage hook): a hit of a creature that has an art or spell (or copy) in flight, read
  from the word at creature +0x120. The `custom-classes` mod builds the buffs of art copies on it and on `gear.buffs`.

## Ability record fields (reverse engineered)

Every byte of the ability records (T1 combat arts, 0x78 bytes; T2 spells, 0x94 bytes) has a name in `gear.data.abilities[id]` now; the old
`unk_<offset>` names still address the same bytes. The full per-offset table with proof addresses, the per-art meaning of the fields
(damage factor, radius, duration, push distance ...), the enums and the coverage figures are in the class editor repo:
`docs/DataFormats.md`, section "Ability field map", and `docs/ability-re/t1.md`, `t2.md` (the editor's `Data/ability-fields.json` holds
labels, units and formulas per art).

* Both tables are produced by the game's balancing tool (FUN_005E6550). A value at level L is `base + L * per_level` for every pair;
  the regeneration seconds are `regen_base + L * regen_per_level` divided by the hero's recovery speed. There is no mana.
* `target_mode` (both tables): `gear.classes.target_modes` = free 0, hostile_creature 1, self_or_friend 2, ground_position 3, corpse 4,
  self_centered 5 (the loader's ECF_ON* names; consumer FUN_0051D840). `flags` bits: `gear.classes.ability_flags`. Class mask bits:
  `gear.classes.ability_classes`.
* T1: `effect_base/_per_level`, `effect2_*`, `param_a_*`, `param_b_*` mean different things per art (weapon damage factor, radius, push
  distance, duration, percent). T2: `value0..3_*` = physical / fire / magic / poison damage (min = max), `attack_mod_*` ... buff percentages,
  `param0..2_*` + caps = duration / radius / quantity per spell.
* Arts 1018, 1019, 1044, 1049..1053 take their numbers from a spell record (engine map 0x4D1E10, `gear.classes.linked_spell(art)`):
  their own effect fields are ignored. Poison Mist (1052) = spell 12: physical and poison 4 + 5L, duration 24 + L seconds.

### Copies that carry their spell

`copy_ability(alias, base, {spell = {value3_base = 12, param0_base = 40}})` gives an art copy of such an art its own copy of the spell
record (a spell alias 100..199 that only the art copy uses; `remove_ability` removes both). The handler of Poison Mist asks the spell
lookup for the constant id 12; a thunk on that lookup answers with the spell alias while a copy is in flight (hero + 0x120 holds the
art alias), so only the copy sees the edited numbers. Measured in game (Poison Mist, one tick per second): original 85..89 per tick and
unchanged after the copy exists; copy with value0 and value3 doubled 175..185 per tick; T1 `effect_base` set to 50 changes nothing.
Hard Hit copy (routed to the Gladiator class) with `effect_base` 1.9 / 3.8: mean damage 204.6 / 385.4 (ratio 1.88 for factor ratio 2).
Only Poison Mist was run in game; the other spell-linked arts use the same lookup. Details: `docs/ability-re/ingame.md`.

## Rune items for ability copies

* The game teaches an art from an item type whose record names it: u16 at +0x6A of the 0x80-byte type record (`FUN_0043AF10`). The type table has 0x7E60
  records, most of them empty in `Items.pak`; `gear.classes.define_rune(alias)` copies the record of the base art's rune into the empty record
  `6200 + alias - 2000` (copies 2000..3799), sets +0x6A to the alias and +0x20 (the name's text resource) to the copy's name. The item record the
  type points to (+0x18) stays the base's. The range is below 0x1FFF so that `drops.select` can name the type.
* Reading is the game's item use (`FUN_0054AA80`, a click on the item: potions, scrolls, runes): `creature:use_item(item)`. The class check is
  the ability record's mask test (`FUN_004D1B20`), which `rune.usable` hooks.
* A savegame with such an item loads without Gear (tested): the type record is empty there.
* The combo master's window is `UI_WND_MASTER`; the custom-classes mod replaces it (`gear.ui.replace_window`) when art files exist. Combos with custom
  arts work in the engine (`creature:set_combo` accepts them; both steps ran).
