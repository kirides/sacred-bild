#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Buffs, debuffs and auras of Gear (gear.buffs in docs/GearApi.md): changes to a creature's combat values that last a
// while, on any creature, with the game's own look on the hero while they run.
//
// The game keeps a creature's combat values in its stats block (creature +0x3A8) and works them out anew in
// cStats::recalc (ENG 0x5796A0, thiscall (stats, 0, 1), ret 8): it zeroes the resistances, damage bonuses and factors
// (the float fields at stats +0x66 .. +0xF2) and adds up what the hero's level, items and the engine's own buffs give.
// Gear hooks its end and adds what the creature's Gear buffs give, then calls it whenever a Gear buff starts or ends,
// so the values hold through equipment changes and level ups and go back when the buff ends. Everything runs on the
// engine thread (the mods' tick).
//
// The look: a creature's spell overlay slot (+0x60 the overlay, +0x68 the seconds left as a float) is what
// Steinhaut (7), Flammenhaut (10) and Lichtschild (6) set; the engine counts it down and draws the hero with it.
namespace Buffs
{
    // One change: `factor` multiplies the field, else `value` is added. `offset` is a stats block offset (a float field
    // cStats::recalc works out: kFields).
    struct Mod
    {
        uint16_t offset = 0;
        bool factor = false;
        float value = 0;
    };

    // The float fields of the stats block cStats::recalc works out anew (0 or 1 before it adds up).
    extern const uint16_t kFields[];
    extern const size_t kFieldCount;
    bool isField(uint16_t offset);

    struct Buff
    {
        std::string key;        // one per creature: applying the same key again replaces it
        std::vector<Mod> mods;
        double seconds = 0;     // how long (0: until removed)
        int overlay = 0;        // the spell overlay to show while it runs (0: none)
        uint32_t source = 0;    // who gave it (a creature id, 0: none)
        int ability = 0;        // with which ability (0: none)
        double left = 0;        // (list) the seconds left, 0 when it has no end
    };

    void install();
    // Once per frame: buffs that ran out end, overlays are kept up.
    void frame();
    // A new game or savegame: every buff ends (they are not saved).
    void clear();

    bool apply(uint32_t id, const Buff& buff, const char*& error);
    bool remove(uint32_t id, const std::string& key);
    std::vector<Buff> list(uint32_t id);
    // Keys of the buffs that ended since the last call, with their creature: for the `buff.ended` event.
    std::vector<std::pair<uint32_t, std::string>> takeEnded();

    // The stats block's float fields of a creature as they are now (offset, value), nil when it has none.
    bool fields(uint32_t id, std::vector<std::pair<uint16_t, float>>& out);
    // The spell overlay slot: what shows and for how many seconds more.
    bool overlay(uint32_t id, int& which, float& seconds);
    bool setOverlay(uint32_t id, int which, float seconds);
    // Works the creature's values out anew (cStats::recalc).
    bool recalc(uint32_t id);
}
