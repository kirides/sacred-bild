#pragma once
#include <windows.h>

// Input only while the game has the focus, and the mouse confined to the game window ([Display] ClipCursor).
// The game polls the keyboard and mouse buttons (GetAsyncKeyState) whether or not it is in the foreground, moves
// the cursor (SetCursorPos), and its low-level keyboard hook swallows Windows keys, Alt+Tab
// and Alt+Esc system-wide; all of that now only happens while one of its windows is in the foreground, and Alt+Tab /
// Alt+Esc always work. It keeps reading the cursor position, so its cursor follows the mouse over the window
// in the background too.
namespace Focus
{
    // Patches the game's imports.
    void install();

    // The main game window was created (main thread).
    void windowCreated(HWND window);

    // Once per presented frame: confines the cursor to the window's client area while it is in the foreground.
    void onFrame();
}
