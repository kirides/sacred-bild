# Session mods: session tracker, loot log, damage meter

Three client-side example mods in `gear/examples`, built only on the public Gear API (no new native code). Each shows
a panel (F2, F3, F7; rebind in the Mods screen) and keeps its data in its own folder. All three guard every API call
and handler, so missing data (no hero, an item without a name, a failing setting) is skipped instead of raising.

Tests (no game needed; the `gear` global is faked): `luajit gear/tests/session_mods_test.lua`.

## sessiontracker (F2)

Time played, kills, gold gained / spent, experience gained, levels gained, with per-hour rates, next to the lifetime
totals and the last three sessions (`session_totals.json`). A session is one game with a hero (`session.start`) or
the "New session" button.

- Time counts only with the game screen up (not in the escape menu or a full screen window).
- Kills use the `kill` event (the local hero's kills). It is fed by the net messages the game sends and is not
  confirmed in single player: if it never fires, kills stay 0 and the rest works.
- Gold and experience are polled twice a second; "gained" is the sum of the rises (a sale counts as a gain, a purchase
  as "spent").

## lootlog (F3)

Entries: time into the session, name, quality colour, kind (dropped, picked up, acquired), the creature when the
loot function ran in the last 3 seconds (`drops.roll`). Filters: lowest quality, category, kind. Kept in
`lootlog.json` (at most `max_entries`).

There is no pickup or drop event in the API, so it is inferred:

- *dropped*: an item newly lying within `radius` tiles of the hero (`gear.world.items_near`, twice a second). Items
  the player drops, or that walk into range, look the same.
- *picked up*: a new backpack item (`gear.inv.container('backpack')`) that was logged as lying on the ground.
  *acquired*: a new backpack item with no logged drop (bought, from equipment). Items that were in the chest a moment
  before are ignored.
- Items already around or in the backpack when a game starts are the baseline. Gold piles are logged only as drops
  ("Log gold piles"); their amount is not exposed. Plain (normal quality) items are skipped unless "Log plain items".

## damagemeter (F7)

Uses the `damage` event, which gives the final damage of each hit and the creature TYPES of attacker and victim (no
ids, no skill, no overkill clipping). Dealt = attacker type is the hero's type, taken = victim type is the hero's type.
It shows fight DPS (a fight ends after 5 s without damage), DPS of the last 10 s, totals, biggest hits, average
hit, fighting time, kills and kills per minute of fighting, a 30 second bar graph, the biggest targets and damage
sources.

Limits: in a network game another hero of the same class counts as the local hero; pets, traps and effects are not
"dealt" (the hook cannot say whose they are); the number is the engine's computed damage, not the hit points removed;
damage that does not pass the hook (if there is any) is missing. Whether every skill passes the hook has not been
checked in a running game.
