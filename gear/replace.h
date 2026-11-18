#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Replaced menus: a mod draws its own screen in place of one of the game's windows (the savegame window, ...). The
// game's window still opens and closes as it always does (hotkeys, the escape menu, its own commands), so the game's
// state is the one it expects; while a mod replaces it, the window is not drawn and gets no mouse or key input (its
// render and receiveEvent virtual functions, slots 5 and 4 of its class's vtable, return at once for input events
// only: the named UI events that show and hide it still reach it). Nothing is patched until a mod claims a window.
//
// Falls back to the game's window by itself: a claim counts only while the mod's Lua ticks publish it (the claims of
// a mod that is off, unloaded or switched off after errors are not published), and once the window is open, the
// mod has to say every tick that it draws the replacement (`beat`): 1.5 s after the window opened without it, or
// 0.5 s after the last one, the game's window shows again until it closes.
namespace Replace
{
    // Whether `name` (upper case) is a window Gear can replace: the in-game windows by control name (UI_WND_SAVEGAME,
    // ...), the menus' screens as UI_MENU_MAIN, UI_MENU_CHARACTER, UI_MENU_OPTIONS, UI_MENU_LOADGAME, UI_MENU_NETWORK.
    bool known(const std::string& name);

    // The claimed windows, once per Lua tick (all mods' together).
    void setClaims(const std::vector<std::string>& names);

    // The claiming mod draws the replacement this tick. True if the window is open and hidden (the mod's screen is
    // what the player sees); false if it is closed or the replacement lapsed.
    bool beat(const std::string& name);

    // Whether the window is open (visible) now.
    bool open(const std::string& name);

    // Closes the window as its own Back / Esc does. False if it is not open or cannot be closed.
    bool close(const std::string& name);

    // The window object for `name` while it exists, 0 if not.
    uintptr_t object(const std::string& name);
}
