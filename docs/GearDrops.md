# Sacred Gear: item drops

Mods can tune what creatures drop: how often, how rare, which item, and which random numbers the rolls use. With no mod
using any of it the game's own code runs unchanged. The Lua API is `gear.drops` ([reference](GearApi.md#drops-geardrops));
this page says what the engine does, what each setting touches, and what was tested. An example mod is
`drop-tuning` in the [sacred-gear-mods](https://github.com/kirides/sacred-gear-mods) repository.

Addresses are the English GOG `Sacred.exe` (the signatures in `src/gear/engine/gear_sigs.inc` find the same code in other
builds). They were read from the decompile (`B:\DevDrive\sources\decomp-sacred`) and checked against
[SacredEngineRemake](https://github.com/Aytackydln/SacredEngineRemake), whose "Gold" addresses are the same as ours; its
`Sacred.Core/Loot` classes are a faithful description of the same code.

## Where drops are decided

**In `sacred.exe`, not in `GameServer.exe`.** The server process loads `Balance.bin`, `Items.pak`, `Weapon.pak` and friends for its
own bookkeeping, but contains no loot code: it has no function that rolls an item (its `% 10000` and `% 1000` uses are unrelated), and
it only relays the objects. A creature is simulated by its owner client (the host's own `sacred.exe` for ambient creatures
in a game the host started), and that client runs the loot function when the creature dies and sends the new item objects to the others.
So the hooks live in `SacredGear.dll` inside `sacred.exe` and the mod must be on the machine that owns the creature (the host, for
most monsters); a mod on a pure client changes nothing for monsters it does not own. GameServer mods (see
[Sacred Gear in network games](GearMultiplayer.md)) are not needed for drops.

## The engine's drop code (ENG addresses)

`0x559BF0` is the **creature loot function**: thiscall (creature, forced item type, rarity bonus, survival value, killer id), `ret 0x10`.
Its callers are the death handler (`0x5340xx`..`0x5346xx`: one call per entry of the creature's authored drop list with the item type as
`forced`, others with 0 for a random drop, repeated by the creature's roll count), `0x4ACDAB`, `0x4D10B9`, `0x537099` and `0x5516E8`.

1. **Drop decision.** The chance budget is `(survival curve + 100) * base chance * 0.01` (Balance.bin: base chance at global +0x700,
   scaled by the party size and for boosted sources). A pass makes up to six attempts: each draws `rand() % 1000` and continues only while
   it is below the budget; after a pass the budget drops by 1000 (above 1000 the creature gets more passes). Forced drops skip the chance.
2. **Kind of thing**, sequential chance checks with their own `rand() % 1000`: potion (Balance chance), rune (a threshold, once per source),
   gold (and its 1/13, 1/7, 1/4 steps to the bigger piles), weapon, armour, jewellery.
3. **Rarity** for equipment: a roll of 1 .. 10000 (`FUN_00443B20(1, 10000)` = `rand() % 10000 + 1`) is compared with the **rarity table**
   (`0x00AD4438`, a vector of 8 byte rows `{dword id, word weight}` loaded from Balance.bin: 18 rows, ids 0 .. 17). Row increment = `weight * level
   factor * 10 * (rarity bonus factor, except for row 16)`; the rows are tried in order and the first whose running sum exceeds the roll
   wins, a roll no row catches is plain (row 17). Row ids 2 .. 15 give the equipment **quality** `16 - id` (14 .. 1; weights 0, 3, 5, 6, 8, 10, 12, 13, 14, 15 x5), rows 16 and 17 are plain (quality 0, weights
   300 and 544), rows 0 and 1 (weights 7 and 9) hand the roll to the special resource selection. Measured by making them overwhelming: row 0
   yields rare (rarity 14) and some unique (rarity 15) items, row 1 yields **set items** and rares. The rarity bonus is the loot function's argument (capped at 700,
   `1 + bonus * 0.01`; it is also rescaled by the creature's experience field). The same table is used by creature loot (`0x559BF0`,
   twice), `0x485C10`, `0x4AC940` and containers (`0x5D57C0`).
4. **Item type**: `0x5C7FA0` (thiscall, type manager `[0xAAB5E4]`, category, level, 0, 0xF, hero preference, flags, 1, quality;
   `ret 0x20`) picks an item type id of the category and level from `Items.pak`. Categories: 5 weapon, 6 chest armour, 8 ring, 13 shield,
   17 helmet, 18 boots, 19 belt, 20 amulet, 21 .. 25 shoulders, arms, legs, gloves, wings, 29 horse equipment (the armour list the loot code draws from is 6, 13, 17 .. 19, 21 .. 25,
   29; jewellery is 8 / 20). The result goes through the usual checks (`0 < type < 0x2000`) and is made into an item object.
5. **The generator.** Everything above uses the CRT `rand` (`0x849B40`: `state = state * 214013 + 2531011; return (state >> 16) & 0x7FFF`, one
   state per thread, 15 bits). The loot function has 52 `call rand` sites and the other rarity draws are `0x55A0F5`, `0x55A1F8`, `0x487DA8`
   (`rand() % 10000 + 1`) and `0x4AD4A4`, `0x5D5B45` (`rand() % 10000`).
   **Modulo bias:** 32768 is not a multiple of 10000, so the values 0 .. 2767 come up four times in 32768 and the rest three times: a rarity roll below 2768
   is 22% more likely than a uniform draw, and every rarer row sits there (rows 0 .. 15 add up to 162 x 10 x factor, at most 1620), so the odds of every
   set, unique and magic quality are about 22% higher in the game than the table's weights say. The `% 1000` chances (drop attempts, kind of thing) have a
   3% bias towards 0 .. 767. The fair generator removes both.

## What Gear changes

`src/gear/items/drops.cpp`: the loot function and the type picker are detoured (always, with a flag check: the detour is a call-through when no mod
handles the events); the loot function's `rand` calls and the five rarity draws are redirected to wrappers
(`Drops: 57 of 57 call sites redirected` in `SacredGear.log`); the loot function's `* 0.01` constants at `0x559F08` (the chance scale) and
`0x55A149` / `0x55A248` (the rarity bonus scale) go through the existing `scale.559BF0` constant sites (`SiteHook`), which now multiply by
`Drops::chanceScale()` / `findScale()` (1 without a mod: the engine's own float). The rarity table's weights are rewritten in
place; the table is re-read before every rarity draw, and a table the game loaded again (a new game) becomes the new vanilla, so the
multipliers apply to it.

| Setting | Effect | Cost |
|---|---|---|
| `chance_scale(m)` | budget `* m` (random creature drops; authored drops, the rolls with a forced item type, are not affected, also by 0; 0 = no random drops) | none per drop |
| `find_scale(m)` | the rarity bonus's `0.01` becomes `0.01 * m`: magic find matters more or less | none |
| `rarity_weights({...})` | multiplies table rows (quality 1 .. 14, `special0`, `special1`), written once per change | none |
| `rng('fair')` / `{seed=n}` | PCG32 for the loot function's draws: `% 1000` draws come from 0 .. 31999 (exact for 1000, 100, 10, 2 ...), the rarity rolls are exact `1 .. 10000` | a mutex per draw (a few dozen per loot roll) |
| `rng(fn)` | a Lua function returning [0, 1) per draw (`lua_draws` counts them; if Lua is busy the fair generator answers) | a Lua call per draw |
| event `drops.roll` | per loot function call: change `bonus`, or return `false` to cancel (also tells authored from random drops via `forced`, and names the source's object `id`: with `gear.world.object(id):carried()` a handler sees the gear a dying creature carries in its equipment slots, +0x1A4, the items the Disarming skill takes) | a Lua call per roll |
| event `drops.select` | per item pick: change category / level / quality, or set `type` to replace the item type | a Lua call per pick |

The settings of all mods are merged: multipliers multiply, the first mod in load order that chose a generator wins. They are pushed to
the native side after each tick and dropped when the mod is unloaded (the mod manager's switch). The events, like all of Gear's, run
in the engine's thread; a handler's error is logged and counts toward the mod's ten errors.

**Limits.** `drops.select` applies to picks inside the creature loot function only (the shop's and quest rewards' picker calls are left alone),
and potions, runes and gold are not picked by `0x5C7FA0`, so the event does not see them (the chance and the generator do). The generator choice
covers the loot function's own draws and the five rarity draws; the item *generation* after the pick (bonuses, sockets, names) keeps using the
game's `rand`, so seeded runs repeat the generator's numbers (`gear.drops.random`) but not whole drops (the game also keeps state between rolls, for example the last
source that dropped a rune or jewellery). Container loot (`0x5D57C0`) shares the rarity table and the rarity draw only.

## Tests

* `luajit src/gear/tests/kernel_test.lua` (kernel side with a mocked native side): the merge, the clearing on unload, the generator choice, the events, the numbers.
* `python tools/check_gear_api.py` (annotations vs code) and `tools/check_gear_overlap.py` (no overlap with SacredBild's patches).
* `src/gear/examples/droptest`, an automation test in the real game (`Sacred.exe -gear-auto -gear-load=1 -gear-mods=droptest -gear-instance=NAME`, in a
  copy of the game folder: the items it rolls stay in the world). It finds a creature that drops equipment, has the engine run its loot function
  600 times per phase (`gear.drops.simulate`, outside Lua so that the events work) and checks:
  the vanilla counters (no generator calls, nothing vetoed or replaced, the rarity table as loaded, the rarity draws running through the
  redirects); `chance_scale(2)` roughly doubles the equipment drops (116 -> 228 of 600 rolls in the run), `chance_scale(0)` gives none and neither changes the 100 of 100
  authored potions of a forced roll; weights x25 on the qualities 1 .. 4 move their share from 7% to 89% and back when cleared; the veto, the replaced item type; the fair generator, a Lua
  generator (0.999 always fails every chance roll: no drops), the seed repeating the generator's numbers (and, in a fresh process, the same 600-roll run: 122 picks, 3507 draws, twice);
  the fair generator's uniformity (chi-square of 200000 draws in 100 buckets). With `special0` / `special1` at x1000 the items on the ground afterwards were
  rare (rarity 13 / 14) and unique (rarity 15) for `special0` and set items for `special1`. 31 checks.
* Not tested: a network game (a LAN host rolling for monsters a client killed, two machines), the container and the other two rarity paths
  in play (they are redirected and checked at install: each `call` is verified to target the engine's own `rand` / range function before it is patched),
  Sacred ReBorn.
