#pragma once
#include <windows.h>

// Controller play ([Controller] Enabled), in the manner of Diablo 2 Resurrected: the pad drives the game through
// injected keyboard and mouse input (Inject) and a cursor of its own, which the game reads in place of the mouse's.
// In game the left stick walks (clicking ahead of the hero with world picking off, so walking never attacks or
// talks), and the attack and combat art buttons click at the enemy AimAssist picks in the stick's direction; potions,
// weapon slots and windows are buttons (Bindings). In menus and windows the left stick moves the cursor, the D-pad
// jumps between buttons (UiNav), A / X click, B is Esc. Whatever the player used last, controller or keyboard and
// mouse, is in charge (InputMode).
namespace Controller
{
    // Hooks; call inside a Patch transaction.
    void install();

    // Once per presented frame, on the presenting thread.
    void onFrame();

    // The cursor the game reads instead of the mouse's (client pixels); false: the mouse's own.
    bool cursor(POINT& client);
    // The game must not move the system cursor (its view scrolling warps it) while the controller has the cursor.
    bool ownsCursor();
    // The game's cursor is not drawn: the controller walks and fights, or SacredBild's own screen is open.
    bool hideGameCursor();

    // A left click at `client` (client pixels) whatever drives the cursor: for SacredBild's screens to press the
    // game's buttons.
    void clickAt(int x, int y);
}
