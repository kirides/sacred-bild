#pragma once
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

// Automation: the game driven without a player, for self tests and scripted runs. Off unless the game starts with
// `-gear-auto` on its command line (or SACREDGEAR_AUTO=1 in its environment); then
//
// - `-gear-load=N` (SACREDGEAR_LOAD=N) loads savegame N (Save\GAMEnn.PAK, 0 the quicksave) as soon as the main menu
//   is up, the way the menu's savegame window does;
// - `-gear-mods=a,b` (SACREDGEAR_MODS) loads only those mods;
// - mods whose mod.lua says `automation = true` get gear.auto: the UI state, simulated mouse and keys, savegame
//   loading, screenshots and quitting (docs/Gear.md);
// - the mods load and run in the menus too (the "idle" tick, every 50 ms while no in-game frame runs), not only
//   from the first in-game frame on.
//
// Input reaches the game as it would from a player: button and key messages posted to its window, the cursor
// through its GetCursorPos (a virtual cursor while automation moved it last), held buttons and keys through its
// GetAsyncKeyState. The real mouse and keyboard keep working.
namespace Automation
{
    struct Options
    {
        bool enabled = false;
        int loadSlot = -1;                  // -1: none
        std::vector<std::string> mods;      // empty: all
    };

    // From the command line and the environment; the same every call.
    const Options& options();

    // After the addresses resolved: input hooks and the driver thread (only if enabled).
    void install(const std::wstring& gameDir);

    // The game window, nullptr before it exists.
    HWND window();

    // The client area's size in pixels (0 x 0 without a window).
    void clientSize(int& width, int& height);

    // The UI manager's state.
    struct UiState
    {
        bool available = false;     // the UI manager exists
        bool menus = false;         // the menu screens are up
        bool inGame = false;
        bool cinematic = false;
        bool help = false;
        bool dialog = false;        // a message box is open
        int mouseX = 0, mouseY = 0; // the game's cursor, UI layout coordinates (cMouse)
    };
    UiState uiState();

    // The visible UI: windows (menus, in-game windows, the message box) and their visible children, absolute rects
    // in UI layout coordinates.
    struct UiNode
    {
        std::string group;          // "menu", "window", "dialog", "child"
        int index = 0;              // the menu / window slot, or the child's index in its parent
        int parent = -1;            // index into the list, -1 for top level
        uint32_t vtable = 0;        // tells the classes apart (see SacredBild's src/sacred/ui.h)
        uint32_t flags = 0;
        int x = 0, y = 0, width = 0, height = 0;
    };
    std::vector<UiNode> uiTree();

    // Input. Positions in client pixels.
    void mouseMove(int x, int y);
    void button(int vk, bool down);         // VK_LBUTTON, VK_RBUTTON, VK_MBUTTON at the current position
    void key(int vk, bool down);
    void character(wchar_t ch);             // WM_CHAR (typing into edit fields)
    void releaseAll();

    // Loads savegame `slot` (0 the quicksave); false if the game is not ready for it.
    bool loadSave(int slot, const char*& error);

    // Saves the game window's client area as a BMP; false on failure.
    bool screenshot(const std::wstring& path, const char*& error);

    // Ends the process with `code` (the log is flushed).
    [[noreturn]] void quit(int code);
}
