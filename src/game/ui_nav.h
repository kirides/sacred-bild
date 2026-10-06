#pragma once
#include <string>

// D-pad navigation in the game's menus and windows: the controller's cursor jumps to the next button (or menu entry,
// list, check box, edit field) in the pressed direction, so the game shows its own hover effect on it. The controls
// are found in the UI manager's visible windows, their classes told apart through the exe's RTTI.
namespace UiNav
{
    // The center of the nearest control from (x, y) in direction (dx, dy) (screen pixels, y down); false if there is
    // none that way.
    bool next(float x, float y, float dx, float dy, float& outX, float& outY);

    // The class of a game object from the exe's RTTI ("cUI_BusyDlg"), "?" if it has none.
    std::string className(void* object);
    // The classes of a window's visible children and theirs (diagnostics).
    std::string contents(void* window);

    // A dialog's default button: the bottom-most, then left-most button in `window` (OK before Cancel), as a screen
    // point; false if it has no visible button.
    bool defaultButton(void* window, float& outX, float& outY);
    // The open message box's Cancel: the bottom-most, then right-most button (its only one if it has one); false if
    // no message box is open.
    bool cancelButton(float& outX, float& outY);
    // How many buttons D-pad navigation finds in `window` (diagnostics).
    int buttonCount(void* window);

    // The window that has to be dealt with first, alone: the message box, else (in game) the game menu; nullptr if
    // neither is open. D-pad navigation stays inside it.
    void* modal();
    // Where the cursor starts in a modal window: a message box's OK, the game menu's first entry.
    bool home(void* window, float& outX, float& outY);

    // The screen point of the inventory's tab (backpack, combat arts, combos) `step` (-1 / 1) places from the open
    // one; false if the inventory is closed or there is none that way.
    bool inventoryTab(int step, float& outX, float& outY);

    // The log book (L) while it is open in game, else nullptr. The rest give the screen point to click for the
    // tab across the top `step` (-1 / 1) places from the selected one, the tab down the left edge, the left or right
    // page's previous / next page button (false at the first / last page), or the list entry on the left page
    // (its text shows on the right page).
    void* logBook();
    bool bookTab(int step, float& outX, float& outY);
    bool bookSection(int step, float& outX, float& outY);
    bool bookPage(bool right, int step, float& outX, float& outY);
    bool bookEntry(int step, float& outX, float& outY);

    // An NPC dialog with answers is open (a popup whose answers are set); Enter picks the first.
    bool npcDialogOpen();
    // The screen point of the open NPC dialog's answer `index` (0..3), to click it; false if it has none there.
    bool npcAnswer(int index, float& outX, float& outY);
}
