#pragma once
#include <cstdint>

// Which the player used last: the controller, or the keyboard and mouse. While it is the controller, the game's
// cursor follows the controller (Controller) and the options window opens as SacredBild's own (OptionsScreen).
// Injected input (Inject) never counts.
namespace InputMode
{
    bool controller();

    // Input from the pad (the thread presenting frames).
    void controllerUsed();
    // A real key or mouse button (the window's thread): the message and its wParam, for the log.
    void keyboardUsed(unsigned message, uintptr_t wParam);
    // A real WM_MOUSEMOVE (the window's thread): the keyboard and mouse once the cursor is a few pixels away from
    // where it was when the controller took over.
    void mouseMoved();
}
