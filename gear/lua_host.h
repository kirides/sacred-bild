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

    // A file next to the mod manager's: mods\<file> (the run's own folder in an automation run).
    std::wstring stateFilePath(const wchar_t* file);

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

    // event `effect.add`: the engine adds a value to a creature's effect entry. `info` is what it is about to do, `answer` what the mod wants.
    struct EffectInfo
    {
        int creature = 0, ability = 0, effect = 0, flags = 0, counter = 0;
        double value = 0;
        float max = 0, current = 0;
    };
    struct EffectAnswer
    {
        bool hasValue = false, clamp = true, finish = false;
        double value = 0;
        int effect = -1;        // -1: the entry the engine is at
        int stack = 0;          // 1 the creature's effect counter becomes 1, 2 it grows by one
    };
    Answer effectAdd(const EffectInfo& info, EffectAnswer& answer);

    // event `bonus.constant`: the number the item bonus code pushes at site `index` (`vanilla` the engine's own).
    Answer bonusConstant(int index, double vanilla, int address, double& out);

    // event `skillslot.select`: a digit key selected a skill slot (`index` the place in the handler); Value when a mod ran.
    Answer skillSlot(int index);

    // event `skillslot.key`: the routing of a character key: slot 0 .. 4 and mode 0 (the whole case: page, COMBINE_SLOTS check, selection) or 1 (the
    // selection only), slot -1 ignores the key. Value when a mod answered.
    Answer skillSlotKey(int character, int& slot, int& mode);

    // gear.game.cursor_click(down): the UI gets a mouse event at the cursor (ReBorn's quick cast sends a button down). False if the UI or the cursor is not there.
    bool cursorClick(bool down);

    // event `combat.branch`: whether a conditional jump is taken (`vanilla`: the engine's own condition).
    Answer branch(const char* name, int index, bool vanilla, int eax, bool& take);

    // event `damage`: the final damage of a hit; `victimType` / `attackerType` -1 for none. The answer is not yet clamped.
    Answer damage(int damage, int victimType, int attackerType, int index, int& out);

    // GetTickCount64() of the last in-game frame, 0 before the first.
    unsigned long long lastFrameTime();
}
