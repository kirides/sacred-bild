#pragma once
#include "sacred/ui.h"

// What the game's UI shows and who gets the input, by the UI manager's own rules (cUI_Manager render ENG 00758ED0
// and receiveEvent 00757840, see docs/UI_SYSTEM.md). Everything SacredBild decides about the UI asks here instead of
// testing windows' visible bits or rects itself:
//
// - A window's visible bit is not "on the screen": in game, an open savegame, options, character or world map
//   window is drawn alone and gets the events alone, and the HUD windows behind it keep their visible bits. shown()
//   answers what the manager actually draws this frame.
// - windows[] holds nothing (or freed windows) outside a game: window() checks cUI_Manager::windowsCreated.
//
// The functions read the manager's live state (cheap reads, any thread).
namespace UiState
{
    using Window = Sacred::cUI_Manager::Window;

    enum class Mode
    {
        None,       // no UI manager yet
        Cinematic,  // letterbox bars; only the escape menu gets events
        Menus,      // the start menu and the screens reached from it
        Game,
    };
    Mode mode();
    Sacred::cUI_Manager* manager();

    // The in-game window, nullptr while the game windows don't exist.
    Sacred::cUI_Window2* window(Window w);
    // In game: the window the manager draws alone, the first visible of savegame, options, character and world map
    // (in that order); nullptr if the HUD is drawn.
    Sacred::cUI_Window2* exclusiveWindow();
    Window exclusive();     // Window::count if none
    // The manager draws the window this frame: in game, visible and not hidden by exclusiveWindow(); the escape
    // menu is drawn over everything, in cinematic mode too. False outside a game.
    bool shown(Window w);
    // A shown window that takes the mouse: none of the HUD's (taskbar, minimap, overview map, chat, portraits).
    bool cursorWindowShown();

    // The menu screen menus[index] if it is visible in the menus.
    Sacred::cUI_Window2* menu(int index);

    // The message box if it is visible: modal, in the menus and in game.
    Sacred::cUI_BusyDlg* messageBox();
    // The escape menu if it is shown (in game).
    Sacred::cUI_EscMenu* escapeMenu();
    // The control with the keyboard focus, nullptr if nobody types.
    Sacred::cUI_Control2* focus();
    // Keys go to text: an edit control has the focus or, in game, the chat line is open.
    bool typing();
    bool helpScreen();
    // The manager swallows mouse and key events (cUI_Manager::inputBlocked).
    bool inputBlocked();
    // The manager's name of a window ("inventory"), for logs.
    const char* name(Window w);
}
