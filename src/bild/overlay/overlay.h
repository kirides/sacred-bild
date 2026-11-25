#pragma once
#include <windows.h>

// Dear ImGui over the game, with the Direct3D 9 backend only: drawn into the back buffer right before it is
// presented, at the screen's resolution (after the UI canvas scaling). Nothing of it runs while no screen is open.
// A screen draws from the presenting thread; the window's messages reach ImGui through a queue the window's thread
// fills, and the controller through Gamepad. Sized like the menus: screen height / 768.
namespace Overlay
{
    // Hooks the backend's present ([DDraw] Backend=d3d9).
    void install();
    // Frames are presented through it (the Direct3D 9 backend is running).
    bool available();

    // Draws one frame of a screen (between ImGui::NewFrame and Render); false closes it.
    using DrawFn = bool (*)();
    // Shows `draw` from the next presented frame on, until it returns false.
    void open(DrawFn draw);
    bool isOpen();

    // The window's thread: while a screen is open, its keyboard and mouse messages go to ImGui; true if the game
    // must not see the message.
    bool message(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    // Gamepad navigation (off while a screen waits for a button to bind).
    void setGamepadNavigation(bool enabled);

    // Screen height / 768.
    float scale();
}
