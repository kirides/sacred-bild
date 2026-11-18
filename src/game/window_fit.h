#pragma once
#include <windows.h>

// A resizable game window (`[Display] Resizable`, with a window frame). The game keeps its resolution and its
// Direct3D 9 back buffer; when the client area has another size, Windows stretches the presented frame to it (DWM
// for the flip model, Direct3D 9's own stretch for the blit model). Resizing the back buffer instead needs every
// default-pool resource released (all of the game's textures), which is what made live resizing fragile. So this
// module only
//   - keeps the window at the frame's aspect ratio (dragging an edge, snapping, "maximize" = the largest such window
//     in the work area), so the stretched frame is not distorted, and
//   - converts the cursor: the game reads and sets it in the screen pixels of a client area that is the size of its
//     frame, so its GetCursorPos / SetCursorPos (screen) and cursor positions (client) go through the scale.
// The window's own coordinates stay the real ones for everything else (clipping the cursor, SacredBild's own
// screens, whose ImGui reads the client rectangle and is stretched with the frame).
namespace WindowFit
{
    // Hooks the game's GetCursorPos (SetCursorPos is Focus's, which calls gameToScreen).
    void install();

    // The main window was created, framed and resizable.
    void windowCreated(HWND window);

    // True while the client area is not the frame's size: positions need converting.
    bool scaled();

    // Cursor positions between the window's client coordinates and the game's (the frame's pixels).
    POINT clientToGame(POINT client);

    // The same in screen coordinates, as the game's GetCursorPos / SetCursorPos use them: unchanged while the
    // window is not scaled.
    POINT screenToGame(POINT screen);
    POINT gameToScreen(POINT game);

    // Window messages of the main window the resizing needs (aspect ratio, minimum size, maximize, background).
    // True if handled; `result` is the window procedure's result. WM_WINDOWPOSCHANGING is adjusted in place and
    // not handled (false): the window procedures after it see the adjusted position.
    bool message(HWND window, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result);
}
