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

    // GetTickCount64() of the last in-game frame, 0 before the first.
    unsigned long long lastFrameTime();
}
