# In-game measurements of ability record fields (Gear ability-fields)

Game copy: B:\Spiele\GOG Games\sacred gold GOG-fields (own copy), Gear build of branch ability-fields. Runner E:\tmp\fields\run.ps1,
experiment mods E:\tmp\fields\mods\{exphit,expmist,expring}, logs E:\tmp\fields\out\fld-<mod>\SacredGear.log (lines "EXP ...").
Method: hits are read from Gear's `damage` event (victim/attacker/damage), foes get `set_modifiers{health=40..100, damage=0}`.
Arts of the Dark Elf companion (recruit_hero('save:Hero03.pax'), has a melee weapon) or of the wood elf leader are routed to the home class
of the art with an `ability.route` handler (the bodies are not the class that owns the art), abilities learned at level 5 with `learn`.

## A. Hard Hit 1001: T1 effect_base = weapon damage factor  -- CONFIRMED
Copies of 1001 (alias 2201: effect_base 1.9, per_level 0; 2202: 3.8, 0; 2203: 1.9 and regen 1). Routed to the Gladiator class.
Art hits of one blow (hits >= 100; the small 6..27 hits are the companion's own plain attacks / the leader's arrows, left out), 8 rounds:
* 2201 (factor 1.9): 193 179 211 210 215 218 191 220 -> mean 204.6
* 2202 (factor 3.8): 369 390 388 393 412 364 382 -> mean 385.4   ratio to 2201: 1.88 (factor ratio 2.0)
* 2203 (factor 1.9, regen 1): 207 223 178 205 220 191 185 -> mean 201.3 (= 2201, so regen does not change the damage)
* original 1001 at level 5 (nominal 1.8 + 5 * 0.2 = 2.8): 300 324 319 310 289 321 305 308 -> mean 309.5
A line through 2201 and 2202 predicts 290 for 2.8; the original gave 309 (it matches a factor of 3.0, so the level counts like 6, or the
weapon damage varies, the spread of one art is about +-5 %). Conclusion: the damage follows effect_base (+ per level) monotonically and
about linearly; the factor scales the weapon damage. Without the routing (Dark Elf body running 1001 with its own class code) the hit does
the plain weapon damage (25..27) for all four arts: the factor only works in the owning class's handler.

## B. Poison Mist 1052 with a linked spell copy  -- CONFIRMED (after a fix)
Finding: the first Gear version of the link (hook on FUN_004D1E10, ENG 4D1E10) did NOT work. The handler (FUN_00506770 case 0x41C) does
not ask the art record for its spell: it pushes the constant 0x0C and calls the spell lookup FUN_004D2310 (call at 0x50994E; the same for
spells 8, 6, 45, 46, 48 at the other call sites). 4D1E10 is only used by the tooltip. Measured: copy with doubled spell values hit exactly
like the original (85 per tick). Fix (gear/classes_alias.cpp): the spell lookup hook (naked thunk, passes ESI = hero and the return address)
answers with the spell alias when the call comes from the hero handler code (0x4FF0B0..0x520000), the hero has an art alias in flight
(hero+0x120) that is linked, and the id asked is the spell its base art takes. Also `linked_spell(alias)` now answers for copies.
Results (wood elf leader, level 5 abilities, routed to the Dark Elf class, foe teleported back into the cloud every 0.5 s,
ticks = hits of the hero with damage >= 40, one per second):
* original 1052: 85 85 86 86 87 87 87 88 (all trials; also after copies exist: 85..87)   -> original unchanged (ii) CONFIRMED
* copy 2301 (spell alias 100: value0 and value3 base and per level x2): tick 175, 180, 185   -> x2.1 (damage x2 confirmed)
* copy 2302 (only T1 effect_base 50 / per_level 50, no spell values): tick 84 87 88 88 89   = original -> (iii) T1 fields ignored CONFIRMED
* copy 2303 (spell alias 101: param0_base 48, per_level 2, damage unchanged): tick 85..89 (damage unchanged)
* spell 12 read back after every trial: value0_base 4, value3_base 4, param0_base 24 (never changed) -> the copy does not change spell 12
* duration: original cloud ticked until 28.5 s (24 + 5 = 29 s expected); the 2303 cloud (48 + 2*5 = 58 s) was still ticking at 41 s when
  the test foe died. So param0 is the cloud duration in seconds. The damaged foes disappear (dead, hp nil) 20 to 30 s after their
  first mist tick in many runs (cause unknown; the hp loss before that was only the ticks), so only "longer than 41 s vs 29 s" is
  measured, not 58 s. Confidence: damage factor and original-unchanged: confirmed; duration: confirmed qualitatively only.
* small extra hits (3..8) of the hero come with the mist; they were ~7.5 for 2303 and ~3.5 for the others, i.e. scale with param0
  (not understood, not used for conclusions).
* One run of the game crashed once at start (exit 0x80000001, before the experiment started); not reproduced in 6 later runs.

## C. Multi-hit 1057 hit radius, regen  -- radius CONFIRMED (coarse), regen NOT confirmed
Dark Elf companion (melee), 10 foes (damage 0) on a ring at 40 90 140 190 250 320 400 480 560 650 units, art ordered on the nearest,
art hits = hits >= 100; 2 rounds x 2 uses:
* 1057 original (param_a 160 + 20*5 = 260 % of the reach): foes hit per use 3, 1, 2, 2 (distances up to 250)
* 2401 (param_a 40, per level 0): 0, 1, 1, 1 (only the target at 40..90)
* 2402 (param_a 600, per level 0): 2, 4, 4, 0 (the last use did not fire), up to 250 units
* 2403 (only regen 1): like the original (3-4)
No foe farther than 250 was ever hit: the foes are moved only 0.2 s before the hit and the ring is coarse, so the maximum radius is
not measured; the order small < original < large is. Fields: param_a_base/per_level = hit radius in % of reach: plausible, coarse.
Regen (cooldown): the copy records have the regen the copy was given (ability(2203).regen_max = 1 s vs 20 s of 2201, confirmed read
back by the game), but `creature:ability(id).regen` stayed 0 and a second `order_ability` 4 s later fired for every art (regen 20 s too).
Gear's order_ability does not enforce the cooldown, or the regen only runs for the skill slots; the cooldown effect is NOT confirmed.
target_mode of a copy: not tested.

## Kernel tests
luajit vcpkg_installed\x86-windows-static\tools\luajit\luajit.exe gear/tests/kernel_test.lua: 447 passed, 1 failed ("the conflict is
marked", fails the same on the HEAD kernel; it is a mod-manager text test, unrelated). The test mock lacked scope_of / hero_scope_of
(the run crashed at HEAD already); added. New checks: new field names and the old unk_/value0 names address the right table.
