#pragma once
#include <cstddef>

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

    // combat.constant: the engine loads a pooled float constant inside its own code (hit handlers, difficulty factors ...); the mods
    // may answer with another number (event `combat.constant`: name, vanilla value, index).
    void setConstantActive(bool active);

    // Self test of the constant stubs, for automation: builds two small functions in memory (one with an `fmul dword [c]`, one with a
    // `fld dword [c]`), hooks them like a constant site named `selftest.fmul` / `selftest.fld` and calls them from a worker thread
    // (Lua is not running then, so the mods may answer): the results are st0 after the instruction, and whether eax, ecx and edx
    // survived.
    struct SelfTest
    {
        double fmul = 0, fld = 0;
        bool registersKept = false;
    };
    bool selfTestStart();
    bool selfTestResult(SelfTest& out);
    Stats constantStats();
}
