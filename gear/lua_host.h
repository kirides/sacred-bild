#pragma once
#include <windows.h>
#include <string>

// The LuaJIT state that runs the mods. In game, everything Lua happens on the engine's thread (the in-game loop,
// whose per-frame update calls frame()): the state is created there on the first frame, the mods load then, and
// every event is dispatched from there, so mods never race the game's update. With automation on (automation.h),
// idle() also runs it from the automation thread while no in-game frame runs (the menus); the two never overlap. The trusted side (lua/kernel.lua, embedded as a resource) does the mod handling in Lua; this file
// gives it the few native functions it needs and keeps errors and runaway handlers away from the engine.
namespace LuaHost
{
    // Before the first frame: the module (for the embedded kernel) and the game folder (mods\ is in it).
    void configure(HMODULE module, const std::wstring& gameDir);

    // One in-game frame on the engine thread; `dt` in seconds. Starts the state and loads the mods the first time.
    void frame(float dt);

    // Automation only: a tick while no in-game frame runs (the menus). Starts the state the first time.
    void idle();

    // The result of asking the mods for a value from a site hook (sitehook.h): Value (the integer is set), None (no mod
    // answered or the mod's answer was no number), Busy (Lua is running on this or another thread: the engine goes on with its
    // own value; this is also how a hook that Lua itself triggers stays out of a loop).
    enum class Answer { None, Value, Busy };

    // event `level.factor`: the factor for `level` from the mods; `site` is the hook site's index (tools/gear_sites.py).
    Answer levelFactor(double level, int site, int& out);

    // event `combat.constant`: the value for the engine's constant `name` (`index` among its sites) from the mods.
    Answer combatConstant(const char* name, double vanilla, int index, double& out);

    // event `creature.ability`: the ability id for a creature of `type` (the AI's vanilla id, -1 where the site has none), at site `index` / `address`.
    Answer creatureAbility(int type, int vanilla, int index, int address, int& out);

    // GetTickCount64() of the last in-game frame, 0 before the first.
    unsigned long long lastFrameTime();
}
