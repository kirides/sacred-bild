#pragma once
#include <string>

namespace Sacred
{
    struct cUI_Control2;
    struct cUI_Diary;
    struct cUI_Savegame;
    struct cUI_Window2;
}

// D-pad navigation in the game's menus and windows: the controller's cursor jumps to the next button (or menu entry,
// list, check box, edit field) in the pressed direction, so the game shows its own hover effect on it. The controls
// are found in the UI manager's visible windows, their classes told apart through the exe's RTTI.
namespace UiNav
{
    struct Rect
    {
        float left, top, right, bottom;     // screen pixels
    };

    // The center of the nearest control from (x, y) in direction (dx, dy) (screen pixels, y down); false if there is
    // none that way.
    bool next(float x, float y, float dx, float dy, float& outX, float& outY);

    // The class of a game object from the exe's RTTI ("cUI_BusyDlg"), "?" if it has none.
    std::string className(const void* object);
    // The classes of a window's visible children and theirs (diagnostics).
    std::string contents(Sacred::cUI_Window2* window);

    // A dialog's default button: the bottom-most, then left-most button in `window` (OK before Cancel), as a screen
    // point; false if it has no visible button.
    bool defaultButton(Sacred::cUI_Window2* window, float& outX, float& outY);
    // The open message box's Cancel: the bottom-most, then right-most button (its only one if it has one); false if
    // no message box is open.
    bool cancelButton(float& outX, float& outY);
    // The control D-pad navigation would stop at that lies under (x, y) (screen pixels), the smallest if they
    // overlap; false if there is none.
    bool controlAt(float x, float y, Rect& out);
    // A visible control's rect in screen pixels, `window` being the top-level window whose frame it is drawn in;
    // false if it is hidden or has no sensible size.
    bool controlRect(Sacred::cUI_Control2* control, Sacred::cUI_Window2* window, Rect& out);
    // How many buttons D-pad navigation finds in `window` (diagnostics).
    int buttonCount(Sacred::cUI_Window2* window);

    // The window that has to be dealt with first, alone: the message box, else (in game) the game menu; nullptr if
    // neither is open. D-pad navigation stays inside it.
    Sacred::cUI_Window2* modal();
    // Where the cursor starts in a modal window: a message box's OK, the game menu's first entry.
    bool home(Sacred::cUI_Window2* window, float& outX, float& outY);

    // The screen point of the inventory's tab (backpack, combat arts, combos) `step` (-1 / 1) places from the open
    // one; false if the inventory is closed or there is none that way.
    bool inventoryTab(int step, float& outX, float& outY);

    // The log book (L) while it is open in game, else nullptr. The rest give the screen point to click for the
    // tab across the top `step` (-1 / 1) places from the selected one, the tab down the left edge, the left or right
    // page's previous / next page button (false at the first / last page), or the list entry on the left page
    // (its text shows on the right page).
    Sacred::cUI_Diary* logBook();
    bool bookTab(int step, float& outX, float& outY);
    bool bookSection(int step, float& outX, float& outY);
    bool bookPage(bool right, int step, float& outX, float& outY);
    bool bookEntry(int step, float& outX, float& outY);

    // The savegame window while it lists savegames (to load or to save over), else nullptr. Its rows are D-pad stops;
    // the rest give the row (0 the quicksave, 1..3 the list's) under (x, y) that shows a savegame (or the new one
    // when saving), else -1; a row's screen point; and the row D-pad up / down (step -1 / 1) goes to from the row
    // under (x, y): `row` -1 at the end of the list, `scrollTo` the first listed savegame to scroll to before, else
    // -1. savegameStep is false if (x, y) is on no row.
    Sacred::cUI_Savegame* savegames();
    int savegameRow(float x, float y);
    bool savegameRowPoint(int row, float& outX, float& outY);
    bool savegameStep(float x, float y, int step, int& row, int& scrollTo);
    // On the window's thread: scrolls the list (unless `scrollTo` is -1) and selects the row as a click on it does.
    void selectSavegame(int scrollTo, int row);
    // On the window's thread: scrolls the load screen's list so that the newest savegame (by the time saved in its
    // header, else its file's) shows and selects it; its row, -1 if nothing is listed.
    int selectNewestSavegame();

    // An NPC dialog with answers is open (a popup whose answers are set); Enter picks the first.
    bool npcDialogOpen();
    // The screen point of the open NPC dialog's answer `index` (0..3), to click it; false if it has none there.
    bool npcAnswer(int index, float& outX, float& outY);
    bool npcAnswerRect(int index, Rect& out);
}
