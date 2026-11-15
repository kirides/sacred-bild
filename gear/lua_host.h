#pragma once
#include <windows.h>
#include <string>

// The LuaJIT state that runs the mods. Everything Lua happens on one thread, the engine's (the in-game loop, whose
// per-frame update calls frame()): the state is created there on the first frame, the mods load then, and every
// event is dispatched from there. No other thread touches the state, so mods need no locks and never race the
// game's update. The trusted side (lua/kernel.lua, embedded as a resource) does the mod handling in Lua; this file
// gives it the few native functions it needs and keeps errors and runaway handlers away from the engine.
namespace LuaHost
{
    // Before the first frame: the module (for the embedded kernel) and the game folder (mods\ is in it).
    void configure(HMODULE module, const std::wstring& gameDir);

    // One in-game frame on the engine thread; `dt` in seconds. Starts the state and loads the mods the first time.
    void frame(float dt);
}
