#pragma once
// Keeps the keyboard from the game while a Gear screen that types or binds keys is open (the mod manager).
//
// While blocked: the game window's key and character messages are dropped (and the release of every key whose press
// was dropped, so the game never sees half a press), and the key state queries sacred.exe imports
// (GetAsyncKeyState, GetKeyState, GetKeyboardState) report no key held. Mouse buttons are not touched. Gear's own
// queries (Ui::keyDown, the mods' key_pressed) are not affected: they do not go through the game's imports.

#include <string>

namespace KeyBlock
{
    // Hooks the game's imports (after Automation::install, so the two chain). Safe to call when the game does not
    // import them.
    void install();

    // Blocks or releases the keyboard (any thread).
    void set(bool blocked);
    bool blocked();

    // What the keys the block kept from the game typed since the last call (the characters the player's keyboard layout
    // makes of them, as TranslateMessage would: umlauts, AltGr, Shift; no control characters). For a Gear screen's text
    // field. Empty while nothing is blocked.
    std::wstring takeTyped();

    // Once per tick on the engine thread: puts the block's window procedure on the game window (again, after the game
    // replaced its own).
    void tick();
}
