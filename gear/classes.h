#pragma once
#include <cstdint>

// Custom classes (docs/Gear.md "Classes"): what the engine binds to a hero's creature type, made answerable by mods.
//
//   * classOf (0x5500D0) maps a creature type to the hero class whose action handler runs the creature's combat arts; 14 call
//     sites compare the creature's action word with the class's action id. The hook asks the `ability.route` event for a
//     hero that has an art in flight (+0x120), so a Seraphim body can run a Gladiator art with the Gladiator's code.
//   * the motion player (0x593320) takes the motion slot an action wants; `motion.select` may replace it, so a body without
//     a clip for a slot plays another one.
//   * the model manager's slot tables (Models.tmp: 256 motion record indices per model) can be read and written, so a body can
//     be given another model's clip for a slot.
//   * learn / forget edit a hero's abilities vector (stats +0x102) the way the engine does when a rune is read.
namespace Classes
{
    // Installs the two hooks (optional: without them the events never fire).
    bool install();

    // Whether some mod handles `ability.route` / `motion.select` (the hooks are cheap pass-throughs while not).
    void setRouteActive(bool active);
    void setMotionActive(bool active);

    // Adds the art or spell `ability` (kind 1: combat art / skill, 2: spell; by the id: from 1000 an art) at `level` to hero
    // object `id`'s abilities with the engine's own function; false (and `error`) when it is not a live hero or the id is
    // known already (then nothing changes).
    bool learn(uint32_t id, uint32_t ability, uint32_t level, const char*& error);

    // Removes the ability from hero `id`; false if it had none.
    bool forget(uint32_t id, uint32_t ability, const char*& error);

    // The models' slot tables. `type` is a creature type; slots are 0..255. motionRecord is the record index (0: none) or -1 if
    // the model was not found. hasMotion follows the engine's own lookup (fallbacks included): 1 yes, 0 no, -1 unknown.
    int motionRecord(uint32_t type, uint32_t slot);
    bool setMotionRecord(uint32_t type, uint32_t slot, uint32_t record);
    int hasMotion(uint32_t type, uint32_t slot);

    // Alias abilities (copied arts and spells): `alias` gets its own record, a copy of the record of the ability
    // `base`, which a mod may change field by field (tables "ability_a1" / "ability_a2", see tables.cpp). The record keeps the
    // base id in its first word, so the class handlers (they switch on it) run the base ability's code; every lookup by the
    // alias id returns the copy. The hero's abilities, the art window, runes and savegames use the alias id.
    // The engine refuses an art in flight with an id above 5000 (0x4FD0F0): art aliases are 2000..4999 (copies of arts 1000..1999).
    // Spell aliases (100..199, copies of spells) are only useful as the linked spell of an art alias (aliasLink): the engine does not
    // run a spell cast under such an id.
    constexpr uint32_t kAliasArtFirst = 2000, kAliasArtLast = 4999, kAliasSpellFirst = 100, kAliasSpellLast = 199;
    bool aliasCreate(uint32_t alias, uint32_t base, const char*& error);
    bool aliasRemove(uint32_t alias);
    // Makes the art alias take its numbers from the spell alias `spellAlias` (a copy of the spell record the art's base art is linked to
    // by the engine's map 0x4D1E10: Poison Mist, Battle Fog, Energy Bolts, ...); 0 removes the link. linkedSpell(base) is that spell
    // id for the game's own art `base` (0 if none).
    bool aliasLink(uint32_t artAlias, uint32_t spellAlias, const char*& error);
    uint32_t linkedSpell(uint32_t base);
    void aliasClear();
    // The alias table `which` (1: arts, 2: spells) as the tables module sees it (first record, records in use).
    bool aliasPool(int which, uintptr_t& first, size_t& count);
    // The alias id of pool record `index` (0 if none) and its base id.
    uint32_t aliasId(int which, size_t index, uint32_t& base);

    // The world under the mouse cursor: the object id the cursor picks (0 none; not `exclude`), the world point (the units of creature
    // positions) and the layer. False when there is no game view.
    struct Cursor
    {
        uint32_t object = 0;
        int x = 0, y = 0, layer = 0;
        int pixelX = 0, pixelY = 0;
    };
    bool cursor(uint32_t exclude, Cursor& out);

    // Installs the ability lookups and the rune class checks (called by install).
    bool installAliases();

    // The combat art or spell id in flight of a creature object (word at +0x120; 0 none).
    uint32_t artInFlight(uint32_t id);

    // Whether some mod handles `rune.usable` (a hero type may learn an ability).
    void setUsableActive(bool active);

    // A hero record of Creature.pak (0x56 bytes) for a hero type 1..9: read it, write bytes into it. (The start skills and
    // the available skills are in it.)
    bool heroRecord(uint32_t type, uint8_t (&out)[0x56]);
    bool setHeroRecord(uint32_t type, uint32_t offset, const uint8_t* bytes, uint32_t length);

    // The game's text for `id` replaced by `utf8` (the engine takes the buffer); only for ids the game has. Texts of Gear's own,
    // ids kTextFirst .. kTextFirst + 0xFFFF, are served by the text lookup (a hook): addText makes a new one, setText on such an id
    // changes it. They work wherever the game looks a text up by id (names and descriptions of alias abilities).
    constexpr uint32_t kTextFirst = 0x100000;
    bool setText(uint32_t id, const char* utf8, const char*& error);
    uint32_t addText(const char* utf8, const char*& error);
}
