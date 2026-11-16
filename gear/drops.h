#pragma once
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

// Item drops: what a creature leaves behind when it dies, and how mods can tune it (docs/GearDrops.md).
//
// The creature loot function (ENG 00559BF0) decides in one place whether a drop attempt happens (a chance of
// `budget / 1000` per attempt, six attempts per pass, one pass per 1000 of budget), what kind of thing it is (potion, rune,
// gold, weapon, armour, jewellery) and, for equipment, its rarity (a roll of 1 .. 10000 against a table of weights, ENG
// 00AD4438) and the item type (the equipment picker, ENG 005C7FA0). All of it runs on the CRT `rand` (a 15 bit LCG).
// Gear leaves the code alone and works at these places:
//
//   * the loot function and the type picker are detoured: events `drops.roll` (veto, change the rarity bonus) and `drops.select` (change
//     the category, level, rarity, or replace the item type), and counters;
//   * the loot function's own 0.01 constants (the chance scale at 00559F08 and the rarity bonus scale) are answered natively
//     (SiteHook's constant sites call chanceScale() and findScale());
//   * the rarity table's weights are rewritten in place (per rarity multipliers): no cost per drop;
//   * the calls of `rand` inside the loot function and the five rarity draws are redirected to a generator the mods choose: the
//     game's own (default, nothing changes), a better PRNG without modulo bias ("fair"), or a Lua function.
//
// With no mod using any of it every redirect calls straight through to the engine's own code.
namespace Drops
{
    // Inside the Patch transaction: detours the loot function and the type picker.
    bool install();
    // After the transaction: redirects the calls of `rand` and of the rarity draws. Returns how many were redirected.
    size_t installSites();

    // From the engine thread's frame hook (outside Lua): runs the queued self test rolls.
    void frame();

    // --- Lua thread: configuration (the kernel merges what the mods ask for and pushes the result) ---------------------------

    // Which events the mods handle (the detours skip the Lua call when none does).
    void setHandlers(bool roll, bool select, bool random);

    // Multipliers of the loot function's chance (budget) and of the scale that turns the rarity bonus into a weight multiplier.
    void setScales(double chance, double find);
    double chanceScale();
    double findScale();

    // The generator for the loot function's rolls: 0 the game's own, 1 fair (PCG32, no modulo bias), 2 a Lua function (events
    // `drops.random`). `seed` is used when `seeded` (reproducible rolls), otherwise the generator seeds itself.
    void setRng(int mode, bool seeded, uint64_t seed);

    // Multipliers of the rarity table's weights by table id (2 .. 15: the rarity 16 - id; 16: normal), the others 1. An empty list is vanilla.
    void setRarityMultipliers(const std::vector<std::pair<int, double>>& multipliers);

    struct RarityEntry
    {
        int id = 0;
        int weight = 0;         // now
        int vanilla = 0;        // as the game loaded it
    };
    // The table now; empty if the game has not loaded it (or the address was not found).
    std::vector<RarityEntry> rarityTable();

    struct Stats
    {
        uint64_t rolls = 0;             // loot function calls
        uint64_t forcedRolls = 0;       // of them with a forced type (an authored drop)
        uint64_t vetoed = 0;            // cancelled by a mod
        uint64_t picks = 0;             // equipment type picks inside loot rolls
        uint64_t overridden = 0;        // picks a mod replaced
        uint64_t rarityDraws = 0;       // draws of the rarity roll
        uint64_t randCalls = 0;         // redirected `rand` calls the generator answered
        uint64_t luaDraws = 0;          // of them answered by a Lua function
        uint64_t quality[16] = {};      // picks by quality (0 normal .. 14, the engine's 16 - id), after the mods
        uint64_t category[32] = {};     // picks by category
    };
    Stats stats();
    void resetStats();

    // Self test (automation): `count` loot rolls of the creature `objectId` at the next frames, outside Lua, so that the events work. The
    // items they make stay in the world. Returns false if one is already running or something is missing.
    bool testRoll(uint32_t objectId, int count, uint32_t bonus);
    // Rolls done / queued.
    std::pair<int, int> testProgress();

    // The generator's draw of the redirected calls, for the kernel tests of the numbers (0 .. 0x7FFF; below(n) in 0 .. n - 1).
    int randomFifteen();
    uint32_t randomBelow(uint32_t n);
}
