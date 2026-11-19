# Custom classes: engine facts and the portability tiers

A custom class is a profile on top of one of the 8 hero bodies (creature types 1..9): its skills, the arts it may learn and
the motion clips it plays. Sacred Gear's part is the set of hooks the mod needs; this page records what the engine does and
what was measured (English GOG `Sacred.exe`, test: `gear/examples/classspike`, a copy of the game folder).

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
  (id >= 1000) is in flight; the handler returns the hero class whose code runs the art, or nil.
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
