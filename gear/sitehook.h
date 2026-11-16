#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// Site hooks: a block of instructions inside a function of sacred.exe that Gear replaces by a jump to a stub, so that
// Lua can supply a value the engine computes inline (no function to detour). The stub runs the block's first instruction
// if the block loads its input itself, passes the input and the site to a handler, and either continues after the block
// with the handler's result or, when no mod answers, runs the displaced instructions unchanged and continues the same
// way. Sites are listed in tools/gear_sites.py (a function found by signature + a fixed offset); a site whose bytes are
// not exactly the expected block is left alone and logged.
//
// level.factor: the engine turns a level into a factor with (34 L + 8) / (c1 L + c2) truncated to an integer, in
// ENG 0051CC30 and five inlined copies; Lua may answer with the integer (event `level.factor`, gear/lua/kernel.lua).
namespace SiteHook
{
    // After the addresses resolved: verifies and patches every site; returns how many hooks are in.
    size_t install();

    // Whether some mod handles `level.factor` (the kernel sets it); without a handler the stubs run the original block.
    void setLevelFactorActive(bool active);

    // Counters for the log and the tests: calls, calls a mod answered, calls the host could not take (Lua busy).
    struct Stats
    {
        unsigned calls = 0, answered = 0, busy = 0;
    };
    Stats levelFactorStats();

    // Every site with the number of times the engine ran it (the hook's name, the index among the sites of that name, the
    // address, the calls).
    struct SiteInfo
    {
        const char* hook;
        int index;
        uintptr_t address;
        unsigned calls;
    };
    std::vector<SiteInfo> sites();

    // combat.constant: the engine loads a pooled float constant inside its own code (hit handlers, difficulty factors ...); the mods
    // may answer with another number (event `combat.constant`: name, vanilla value, index).
    void setConstantActive(bool active);

    // creature.ability: the creature AI stores the ability a creature will use (event `creature.ability`: creature type, vanilla id, index, address).
    void setAbilityActive(bool active);
    Stats abilityStats();

    // damage: the final damage of a hit (event `damage`: the damage, the victim's and the attacker's creature types).
    void setDamageActive(bool active);

    // effect.add: the engine adds a value to a creature's effect entry (event `effect.add`).
    void setEffectActive(bool active);

    // bonus.constant: the numbers pushed as arguments of the item bonus rolls (event `bonus.constant`: index, vanilla value, address).
    void setBonusActive(bool active);

    // skillslot.select: a digit key selected a skill slot (event `skillslot.select`: the place among the five).
    void setSkillSlotActive(bool active);

    // skillslot.key: the character key handler of the skill slot window (event `skillslot.key`: the character) lets the mods route a key.
    void setSkillKeyActive(bool active);
    Stats skillKeyStats();

    // combat.branch: the outcome of some conditional jumps of the hit handlers (event `combat.branch`: name, whether the engine takes the jump, index).
    void setBranchActive(bool active);
    Stats branchStats();
    Stats skillSlotStats();
    Stats bonusStats();
    Stats effectStats();
    Stats damageStats();

    // Self test of the constant stubs, for automation: builds two small functions in memory (one with an `fmul dword [c]`, one with a
    // `fld dword [c]`), hooks them like a constant site named `selftest.fmul` / `selftest.fld` and calls them from a worker thread
    // (Lua is not running then, so the mods may answer): the results are st0 after the instruction, and whether eax, ecx and edx
    // survived.
    struct SelfTest
    {
        double fmul = 0, fld = 0;
        bool registersKept = false;
        // creature.ability stubs on three functions built in memory (a store of 100, an add of 1000, the computed block): what they
        // left (vanilla 100, 1000 and 0x32 when no mod answers), and whether the registers survived.
        int abilityStore = 0, abilityAdd = 0, abilityEdx = 0;
        bool abilityKept = false;
        // damage stub on a function built in memory (damage 12.4 rounds to 12; a handler of the fake attacker of type 77 answers)
        int damage = 0;
        // effect.add stub on a function built in memory (the sum 3.5 for entry 0): the exit taken (1 engine path, 2 next entry, 3 leave),
        // the two entries' values times 100 and the creature's effect counter (4 before).
        int effectExit = 0, effectEntry0 = 0, effectEntry1 = 0, effectCounter = 0;
        // bonus.constant stubs on functions built in memory: `push 5` (vanilla 5) and `push 3.5f`
        int bonusInt = 0;
        float bonusFloat = 0;
        // skillslot.select stub on a function built in memory: the displaced `mov byte [esp+0x13], 1` must still have run (1)
        int slotByte = 0;
        // combat.branch stub on a function built in memory (an equal compare, then `je`): 2 when the jump was taken, 1 when it fell through
        int branchTaken = 0;
        // the same with a `jcc rel8` and a displaced `add eax, -2` (eax = 0x28 at the compare): path (2 jump taken, 1 fell through) and eax afterwards
        int branch2Path = 0, branch2Eax = 0;
    };
    bool selfTestStart();
    bool selfTestResult(SelfTest& out);
    Stats constantStats();
}
