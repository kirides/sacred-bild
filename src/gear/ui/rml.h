#pragma once
#include <string>

struct lua_State;

// RmlUi inside Sacred Gear: HTML/CSS-style documents (.rml / .rcss) for mod UI, drawn through the mod overlay (ui.h,
// Kind::Mesh) and controlled from Lua (gear.ui.rml in lua/kernel.lua, on the natives registered here).
//
// What this file is: RmlUi's system, file and render interfaces, a context per document, the element access the Lua
// side needs and the queue that carries RmlUi's events back to Lua. Everything runs on the Lua thread (the engine's
// thread in game); the render thread only replays the recorded meshes. Documents load only from the mod's own folder,
// RmlUi is started with the first document, and nothing here runs for a mod that does not use it.
namespace RmlHost
{
    // The game's mods folder (<game>\mods), before the first document.
    void configure(const std::wstring& modsDir);
    // Where the game's fonts are (default: font\ next to the exe); a host other than the game sets the game's font folder.
    void setFontDir(const std::wstring& dir);

    // Adds the natives (rml_load, rml_update, rml_draw ...) to the table on top of the Lua stack.
    void registerNatives(lua_State* L);
}
