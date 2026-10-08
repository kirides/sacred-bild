#pragma once
#include "game/sacred_addr.h"
#include "sacred/vector.h"

#include <cstddef>
#include <cstdint>

struct IDirect3DDevice7;

// The UI: controls and windows (cUI_Control2 / cUI_Window2 and their subclasses, by their RTTI names), the UI manager
// that owns and draws them, events and fonts. Positions are in the 1024x768 layout; a control's x/y are relative to
// its parent.
namespace Sacred
{
    // A rect as the UI returns them: x, y, then width and height (int32 x, y, width | height << 16).
    struct UiRect
    {
        int32_t x, y;
        int16_t width, height;
    };
    static_assert(sizeof(UiRect) == 0x0C);

    struct cEvent
    {
        uintptr_t vtable;
    };

    // A mouse button event (cEventMouseDown / cEventMouseUp).
    struct cEventMouse : cEvent
    {
        uint8_t _04[0x08 - 0x04];
        int32_t x, y;

        // `event` as a mouse button event, nullptr if it is another kind.
        static cEventMouse* of(cEvent* event)
        {
            const uintptr_t v = event ? event->vtable : 0;
            return v && (v == Addr::cEventMouseDown_vtable || v == Addr::cEventMouseUp_vtable)
                ? static_cast<cEventMouse*>(event) : nullptr;
        }
    };
    static_assert(offsetof(cEventMouse, x) == 0x08 && offsetof(cEventMouse, y) == 0x0C);

    struct cUI_Control2
    {
        // The virtual functions SacredBild calls or wraps. They return what they leave in EAX as uint32_t, so
        // wrappers hand it on whatever it is.
        struct Vtable
        {
            void* _00[4];
            uint32_t(__fastcall* receiveEvent)(cUI_Control2* self, void* edx, cEvent* event);       // -> bool
            uint32_t(__fastcall* render)(cUI_Control2* self, void* edx, IDirect3DDevice7* device);
            void* _18;
            uint32_t(__fastcall* isInside)(cUI_Control2* self, void* edx, int x, int y);            // -> bool
        };

        // In flags.
        static constexpr uint32_t visibleFlag = 0x01;
        // Clicks count only while it is set (cUI_Control2 receiveEvent ENG 00731CE0 and the buttons', edit controls',
        // cUI_Listbox's and the menu texts'; not cUI_Listbox2's or cUI_Combobox's); buttons draw their text grey
        // without it (cUI_Button2 render 0072B700).
        static constexpr uint32_t enabledFlag = 0x04;
        static constexpr uint32_t checkedFlag = 0x10;   // a check box's or radio button's checked state

        const Vtable* vtable;
        uint8_t _04[0x10 - 0x04];
        uint32_t flags;
        uint8_t _14[0x24 - 0x14];
        int32_t x, y;
        int16_t width, height;
        uint8_t _30[0x50 - 0x30];
        cUI_Control2* parent;       // x/y are relative to it; nullptr: to the screen
        uint8_t _54[0x78 - 0x54];

        bool visible() const { return flags & visibleFlag; }
        bool enabled() const { return flags & enabledFlag; }
        bool checked() const { return flags & checkedFlag; }
        void setFlags(uint32_t mask) { Addr::cUI_Control2_setFlags(this, mask); }
        void clearFlags(uint32_t mask) { Addr::cUI_Control2_clearFlags(this, mask); }

        // The rect with the parents' positions added, as the game hit-tests it.
        UiRect absoluteRect()
        {
            UiRect r{};
            Addr::cUI_Control2_getAbsoluteRect(this, &r);
            return r;
        }
    };
    static_assert(offsetof(cUI_Control2::Vtable, receiveEvent) == 0x10);
    static_assert(offsetof(cUI_Control2::Vtable, render) == 0x14 && offsetof(cUI_Control2::Vtable, isInside) == 0x1C);
    static_assert(offsetof(cUI_Control2, flags) == 0x10 && offsetof(cUI_Control2, x) == 0x24);
    static_assert(offsetof(cUI_Control2, width) == 0x2C && offsetof(cUI_Control2, parent) == 0x50);
    static_assert(sizeof(cUI_Control2) == 0x78);

    // A control embedded in its window (not a child), of `Size` bytes, whose class SacredBild doesn't need.
    template <size_t Size>
    struct UiEmbedded : cUI_Control2
    {
        uint8_t _78[Size - sizeof(cUI_Control2)];
    };

    // A button; embedded ones lie 0xBC bytes apart.
    struct cUI_Button2 : UiEmbedded<0xBC>
    {
    };
    static_assert(sizeof(cUI_Button2) == 0xBC);

    struct cUI_Slider : cUI_Control2
    {
        uint8_t _78[0x88 - 0x78];
        uint32_t count;             // values 0..count-1

        // fastcall (slider): the low 16 bits are the value.
        uint32_t value() { return Addr::cUI_Slider_getValue(this); }
        void setValue(uint32_t v) { Addr::cUI_Slider_setValue(this, v); }
    };
    static_assert(offsetof(cUI_Slider, count) == 0x88);

    // Text controls: the start menu's entries (cUI_StaticText64FX, render ENG 00754900: always centered, font flags
    // 0x801) and the game menu's (cUI_StaticText64, render 00754140: flags `centered`, 0x40000 / 0x100000 other
    // alignments, none from the left). Fonts are uint16 ids (cFontManager).
    struct cUI_StaticText64FX : cUI_Control2
    {
        uint32_t textId;
        uint16_t font;
    };
    static_assert(offsetof(cUI_StaticText64FX, textId) == 0x78 && offsetof(cUI_StaticText64FX, font) == 0x7C);

    struct cUI_StaticText64 : cUI_Control2
    {
        static constexpr uint32_t centered = 0x80000;   // in flags

        uint8_t _78[0x84 - 0x78];
        uint32_t textId;            // 0: the text at text .. textEnd
        const wchar_t* text;
        const wchar_t* textEnd;
        uint8_t _90[0x94 - 0x90];
        uint16_t font;
    };
    static_assert(offsetof(cUI_StaticText64, textId) == 0x84 && offsetof(cUI_StaticText64, text) == 0x88);
    static_assert(offsetof(cUI_StaticText64, textEnd) == 0x8C && offsetof(cUI_StaticText64, font) == 0x94);

    // A tab control: a vector of pages, the active one at `activePage` (setActivePage ENG 00729260). Its
    // receiveEvent (00728350) hit-tests the tabs 4 pixels below their rects and switches to the one clicked, hidden
    // or not.
    struct cUI_TabControl : cUI_Control2
    {
        struct Page
        {
            uint8_t _00[0x10];
            cUI_Control2* button;   // the tab
            cUI_Control2* window;   // the page
        };

        Vector<Page> pages;
        uint16_t activePage;
    };
    static_assert(sizeof(cUI_TabControl::Page) == 0x18);
    static_assert(offsetof(cUI_TabControl, pages) == 0x78 && offsetof(cUI_TabControl, activePage) == 0x84);

    struct cUI_Window2 : cUI_Control2
    {
        struct Vtable : cUI_Control2::Vtable
        {
            void* _20;
            uint32_t(__fastcall* show)(cUI_Window2* self, void* edx, uint32_t show);    // bool in the low byte
            void* _28[3];
            void* escape;           // Esc
            void* enter;            // Enter
            void* _3c[2];
            // (device, ?, ?): blacksmith, merchant, net portraits and the options window draw here.
            uint32_t(__fastcall* render2)(cUI_Window2* self, void* edx, IDirect3DDevice7* device, uint32_t a,
                uint32_t b);
        };

        Vector<cUI_Control2*> children;     // positioned relative to the window

        const Vtable* vtbl() const { return static_cast<const Vtable*>(vtable); }
        void layoutChildren() { Addr::cUI_Window2_layoutChildren(this); }
    };
    static_assert(offsetof(cUI_Window2::Vtable, show) == 0x24 && offsetof(cUI_Window2::Vtable, escape) == 0x34);
    static_assert(offsetof(cUI_Window2::Vtable, enter) == 0x38 && offsetof(cUI_Window2::Vtable, render2) == 0x44);
    static_assert(offsetof(cUI_Window2, children) == 0x78 && sizeof(cUI_Window2) == 0x84);

    // Popup (tooltip) windows, owned by the UI manager (RTTI cUI_Tooltip). Callers set the popup's x/y first, then its
    // text with setText (?, const wstring*, bool) or setTextId (?, text id, bool), which also requests a new layout:
    // the popup's render centers it on x/y and clamps it into 16..1008 x 16..752 before drawing.
    // An NPC dialog is a popup with up to four answers (ENG 006E6D20 sets them, the layout places them; the text
    // setters clear them). Enter picks the first, Esc the second; a click picks the one under the cursor, which the
    // render highlights.
    struct cUI_Popup : cUI_Window2
    {
        // In flags: the layout sizes the popup to its text; unless 2 it centers the popup on x/y; then:
        static constexpr uint32_t clampToScreen = 0x08;     // kept 16 px inside the 1024x768 screen
        static constexpr uint32_t centerOnScreen = 0x20;    // centered on (512, 384)
        static constexpr uint32_t keepChildren = 0x200;     // the children stay where they are
        static constexpr int answerCount = 4;

        struct Answer
        {
            uint32_t text;          // text id, 0 = no answer
            uint8_t _04[4];
            int32_t x, y;           // relative to the popup
            int16_t width, height;
        };

        uint8_t _84[0x154 - 0x84];
        uint32_t popupFlags;
        uint8_t _158[0x217C - 0x158];
        Answer answers[answerCount];

        // Clears 0x40 and, unless keepChildren, lays out the children.
        void layout() { Addr::cUI_Popup_layout(this); }
    };
    static_assert(sizeof(cUI_Popup::Answer) == 0x14 && offsetof(cUI_Popup::Answer, x) == 0x08);
    static_assert(offsetof(cUI_Popup, popupFlags) == 0x154 && offsetof(cUI_Popup, answers) == 0x217C);

    // The taskbar (vtable ENG 00896AC0, setup 006E4DF0). Weapon slots (keys 1-5, "UI_TB_SKILL1".., 64x64) and combat
    // art slots (keys 6-0, "UI_TB_SPELL1".., 64x64): the relayout (006E01F0) hides the slots past the hero's slot count
    // and moves the weapon slots so they end at the center. Potion buttons (Space Q W E R: healing, then types 2-5;
    // 32x32 in an arc above the center ornament): drawn only with the SHOWPOTIONS option; the greyed copies stand for
    // potions the hero has none of.
    struct cUI_Taskbar2 : cUI_Window2
    {
        static constexpr int slotCount = 5;
        // Where the level-up button sits: the absolute position of the stats window's close button, which covers it
        // while that window is open.
        static constexpr int levelUpX = 898;
        static constexpr int levelUpY = 10;

        uint8_t _84[0x164 - 0x84];
        cUI_Control2* weaponSlots[slotCount];   // cUI_Static
        cUI_Control2* artSlots[slotCount];
        // "+", opens the character stats; blinks with flag 0x400000 while points are unspent.
        cUI_Button2 levelUpButton;
        uint8_t _248[0x258 - 0x248];
        cUI_Button2 potionButtons[slotCount];
        cUI_Button2 potionButtonsEmpty[slotCount];
    };
    static_assert(offsetof(cUI_Taskbar2, weaponSlots) == 0x164 && offsetof(cUI_Taskbar2, artSlots) == 0x178);
    static_assert(offsetof(cUI_Taskbar2, levelUpButton) == 0x18C && offsetof(cUI_Taskbar2, potionButtons) == 0x258);
    static_assert(offsetof(cUI_Taskbar2, potionButtonsEmpty) == 0x604);

    // The game menu (Esc in game): its entries (options, save, export, quit, continue) are in a vector of their own,
    // not children; their parent is the menu, and its event and render functions (ENG 006BBA80, 006BB390) hit-test
    // them with the mouse.
    struct cUI_EscMenu : cUI_Window2
    {
        uint8_t _84[0x158 - 0x84];
        Vector<cUI_StaticText64*> entries;
    };
    static_assert(offsetof(cUI_EscMenu, entries) == 0x158);

    // The start menu (constructor ENG 007119B0, entries built in 00713670): its entries (384,y 256x30, 32 apart) are in
    // one vector per screen, not children; event and render (007125B0, 007128D0) use the vector of the screen shown.
    struct cUI_MainMenu : cUI_Window2
    {
        static constexpr uint32_t screensWithEntries = 3;

        uint8_t _84[0x154 - 0x84];
        uint32_t screen;            // 0 main, 1 multiplayer, 2 extras, 3 credits (no entries)
        Vector<cUI_StaticText64FX*> entries[screensWithEntries];
    };
    static_assert(offsetof(cUI_MainMenu, screen) == 0x154 && offsetof(cUI_MainMenu, entries) == 0x158);

    // The savegame window (vtable ENG 008970E4): in game cUI_Manager::savegame (saving), in the menus menus[3]
    // (loading). Four rows of one hit rect each (receiveEvent 0071D8A0 selects the row clicked through selectRow, a
    // double click or Enter loads / saves it): row 0 the quicksave (GAME00.PAK), rows 1-3 the list's savegames from the
    // first listed one on (the slider's value, at most count - 3 when loading, count - 2 when saving: a row past the
    // end is a new savegame). The list (filled by 0071EAA0 when the window opens) is sorted by file name.
    struct cUI_Savegame : cUI_Window2
    {
        struct Entry
        {
            uint8_t _00[0x04];
            char name[0x100];       // the file in .\SAVE
            uint8_t _104[0x310 - 0x104];
        };

        static constexpr int rowCount = 4;
        static constexpr uint32_t saving = 1;   // mode
        static constexpr uint32_t loading = 2;
        // The savegame file's header (read by ENG 0071E600): when it was saved, local time, int32 each (year, month,
        // day, day of week, hour, minute, second, milliseconds); zero in old ones.
        static constexpr size_t headerSize = 0x100;
        static constexpr size_t headerYear = 0x5C;

        uint8_t _84[0x156 - 0x84];
        uint16_t selectedRow;
        uint8_t _158[0x15C - 0x158];
        Vector<Entry> entries;
        Entry quicksave;            // its name is empty without one
        cUI_Slider slider;          // the first listed savegame
        uint8_t _504[0x72C - 0x478 - sizeof(cUI_Slider)];
        UiRect rows[rowCount];
        uint32_t mode;              // 0 closed, 1 saving, 2 loading, 3-5 asking (a message box is open)
        uint32_t askedMode;         // the mode while asking
        // Not children (the window draws and hit-tests them): load / save, overwrite, delete, back.
        cUI_Button2 buttons[4];

        void selectRow(uint16_t row) { Addr::cUI_Savegame_selectRow(this, row); }
    };
    static_assert(sizeof(cUI_Savegame::Entry) == 0x310 && offsetof(cUI_Savegame::Entry, name) == 0x04);
    static_assert(offsetof(cUI_Savegame, selectedRow) == 0x156 && offsetof(cUI_Savegame, entries) == 0x15C);
    static_assert(offsetof(cUI_Savegame, quicksave) == 0x168 && offsetof(cUI_Savegame, slider) == 0x478);
    static_assert(offsetof(cUI_Savegame, rows) == 0x72C && offsetof(cUI_Savegame, mode) == 0x75C);
    static_assert(offsetof(cUI_Savegame, askedMode) == 0x760 && offsetof(cUI_Savegame, buttons) == 0x764);

    // The message box ("Load savegame?", network waits; ENG 00756A90 allocates it): its buttons are members (OK left
    // of Cancel), laid out per kind of box (00722500). Esc cancels only two kinds, so the controller clicks the
    // buttons.
    // The portal box (modes surfacePortals / underworldPortals) lists the portals below its title, centered in
    // textRect (render 00721B10, receiveEvent 00721380 hit-tests the same rows): with portalFont's line height h
    // (font vtable +0x28 of 'A'), row i spans textRect.x .. +width and textRect.y - 4 + (i + 1) * (h + 4) .. + h.
    // Surface row i is portal i, underworld row i portal portalCount + i; one can be picked if its bit is set in
    // `portals`, else it is drawn dark. The underworld list leaves rows 7..12 empty.
    struct cUI_BusyDlg : cUI_Window2
    {
        static constexpr uint32_t surfacePortals = 5;   // mode
        static constexpr uint32_t underworldPortals = 6;
        static constexpr uint16_t portalFont = 3;
        static constexpr int portalCount = 14;          // 13 without the add-on (g_hasAddon)

        uint8_t _84[0x154 - 0x84];
        uint32_t mode;
        UiRect textRect;
        uint8_t _164[0x170 - 0x164];
        uint32_t portals;           // bit per portal: it can be picked
        uint8_t _174[0x644 - 0x174];
    };
    static_assert(offsetof(cUI_BusyDlg, mode) == 0x154 && offsetof(cUI_BusyDlg, textRect) == 0x158);
    static_assert(offsetof(cUI_BusyDlg, portals) == 0x170 && sizeof(cUI_BusyDlg) == 0x644);

    // The network screens (made by ENG 0070BF10 for cUI_Network): their controls are members, not children; each
    // screen draws and feeds events to the ones its state shows, and sets the visible and enabled flags of most as
    // that state changes (cUI_NetLan: 006FA490). cUI_CDKey's four fields are part of cUI_NetLogin and cUI_NetAccount.
    struct cUI_NetLogin : cUI_Window2
    {
        uint8_t _84[0x384C - 0x84];
    };
    struct cUI_NetAccount : cUI_Window2
    {
        uint8_t _84[0x40B4 - 0x84];
    };
    struct cUI_NetLobby : cUI_Window2
    {
        uint8_t _84[0x88A4 - 0x84];
    };
    struct cUI_NetPassword : cUI_Window2
    {
        uint8_t _84[0x168C - 0x84];
    };
    struct cUI_NetLan : cUI_Window2
    {
        uint8_t _84[0x51B8 - 0x84];
    };
    struct cUI_NetTest : cUI_Window2
    {
        uint8_t _84[0xCF8 - 0x84];
    };
    // The character choice of a network game, also the character export in game (createGameWindows 00759AF0).
    struct cUI_Character : cUI_Window2
    {
        uint8_t _84[0x460 - 0x84];
    };
    static_assert(sizeof(cUI_NetLogin) == 0x384C && sizeof(cUI_NetAccount) == 0x40B4);
    static_assert(sizeof(cUI_NetLobby) == 0x88A4 && sizeof(cUI_NetPassword) == 0x168C);
    static_assert(sizeof(cUI_NetLan) == 0x51B8 && sizeof(cUI_NetTest) == 0xCF8 && sizeof(cUI_Character) == 0x460);

    // The network menu (a menu screen, ENG 0070B1D0): it shows one of its screens, drawing it (render 0070B500) and
    // handing it the events (receiveEvent 0070B7D0) itself; they are not its children.
    struct cUI_Network : cUI_Window2
    {
        enum Screen : uint32_t
        {
            none, login, account, lobby, password, character, test, lan,
        };

        uint8_t _84[0x158 - 0x84];
        Screen screen;
        cUI_NetLogin* loginScreen;
        cUI_NetAccount* accountScreen;
        cUI_NetLobby* lobbyScreen;
        cUI_NetPassword* passwordScreen;
        cUI_NetLan* lanScreen;
        cUI_Character* characterScreen;
        cUI_NetTest* testScreen;

        // The screen shown, nullptr if none.
        cUI_Window2* current() const
        {
            switch (screen)
            {
            case login: return loginScreen;
            case account: return accountScreen;
            case lobby: return lobbyScreen;
            case password: return passwordScreen;
            case character: return characterScreen;
            case test: return testScreen;
            case lan: return lanScreen;
            default: return nullptr;
            }
        }
    };
    static_assert(offsetof(cUI_Network, screen) == 0x158 && offsetof(cUI_Network, loginScreen) == 0x15C);
    static_assert(offsetof(cUI_Network, lanScreen) == 0x16C && offsetof(cUI_Network, testScreen) == 0x174);

    // The player list of a network game (in game cUI_Manager::Window::networkInfo, ENG constructor 006F6CC0): opened,
    // it shows four 16x16 icons per player (the cells at cellRect, which the render (006F7730) also gives its cell
    // controls); a click on one (receiveEvent 006F8CA0, for the rows below cNetPlayers::count, while the list is
    // enabled) acts on that player (006F82A0, by column).
    struct cUI_NetworkInfo : cUI_Window2
    {
        static constexpr int columns = 4;

        uint8_t _84[0x259C - 0x84];
        uint8_t opened;             // the list is shown (an event 0x11 sets it)
        uint8_t _259d[0x25A0 - 0x259D];

        // A cell's rect in the 1024x768 layout (absolute, not relative to the window).
        UiRect cellRect(int row, int column)
        {
            UiRect r{};
            Addr::cUI_NetworkInfo_cellRect(this, &r, static_cast<uint32_t>(row), column);
            return r;
        }
    };
    static_assert(offsetof(cUI_NetworkInfo, opened) == 0x259C && sizeof(cUI_NetworkInfo) == 0x25A0);

    // The log book's books (0xA24 bytes): up to seven tabs down its left edge, a list on the left page and the selected
    // entry's text on the right, each page side with its previous / next buttons. Its receiveEvent (ENG 006B3940)
    // hit-tests the members below (whatever their visible bit says; the page functions 006AFFD0 / 006B0120 stop at the
    // ends and hide the buttons there) and the list through lineAt.
    struct cUI_Book : cUI_Window2
    {
        struct Page
        {
            uint8_t _00[0x04];
            int32_t selected;       // the selected entry's index
            uint8_t _08[0x1C - 0x08];
        };

        static constexpr int tabCount = 7;
        // The left page in book coordinates (ENG 006B0240).
        static constexpr int listX = 0x60, listY = 0x20, listWidth = 0x11C, listHeight = 0x1C0;

        uint8_t _84[0x15E - 0x84];
        uint16_t selectedTab;
        uint8_t _160[0x162 - 0x160];
        uint16_t leftPage;          // the list's page
        uint8_t _164[0x168 - 0x164];
        cUI_Button2 tabs[tabCount];
        cUI_Button2 leftPrevious, leftNext, rightPrevious, rightNext;
        Vector<Page> pages[tabCount];   // the list's pages, per tab
        uint8_t _9d0[0xA24 - 0x9D0];

        // Whether (atX, atY) (the 1024x768 layout) lies on an entry of the left page's list, and which.
        bool lineAt(int atX, int atY, uint16_t& index) { return Addr::cUI_Book_lineAt(this, atX, atY, &index); }
    };
    static_assert(sizeof(cUI_Book::Page) == 0x1C);
    static_assert(offsetof(cUI_Book, selectedTab) == 0x15E && offsetof(cUI_Book, leftPage) == 0x162);
    static_assert(offsetof(cUI_Book, tabs) == 0x168 && offsetof(cUI_Book, leftPrevious) == 0x68C);
    static_assert(offsetof(cUI_Book, leftNext) == 0x748 && offsetof(cUI_Book, rightPrevious) == 0x804);
    static_assert(offsetof(cUI_Book, rightNext) == 0x8C0 && offsetof(cUI_Book, pages) == 0x97C);
    static_assert(sizeof(cUI_Book) == 0xA24);

    // The log book (L): four tabs across the top, embedded buttons (parent set) that select a book each (ENG
    // 006AF0E0, from the diary's receiveEvent 006AED40).
    struct cUI_Diary : cUI_Window2
    {
        static constexpr int tabCount = 4;

        uint8_t _84[0x538 - 0x84];
        UiEmbedded<0x7C> tabs[tabCount];
        uint8_t _728[0x7A4 - 0x728];
        uint16_t currentTab;        // tabCount before the first was picked
        uint8_t _7a6[0x7A8 - 0x7A6];
        Vector<cUI_Book*> books;    // one per tab
    };
    static_assert(offsetof(cUI_Diary, tabs) == 0x538 && offsetof(cUI_Diary, currentTab) == 0x7A4);
    static_assert(offsetof(cUI_Diary, books) == 0x7A8);

    // The inventory window: its pages (backpack, combat arts, combos) are a cUI_TabControl, the `tabControl` member of
    // its `tabs` member (its receiveEvent ENG 006C61C0 switches pages through 00729940 for the taskbar's buttons and
    // keys).
    struct cUI_Inventory3 : cUI_Window2
    {
        struct Tabs
        {
            uint8_t _00[0x78];
            cUI_TabControl* tabControl;
        };

        uint8_t _84[0x154 - 0x84];
        Tabs* tabs;
    };
    static_assert(offsetof(cUI_Inventory3::Tabs, tabControl) == 0x78 && offsetof(cUI_Inventory3, tabs) == 0x154);

    // The options window (in game and from the main menu): its controls (the build at ENG 00718CA0), as show
    // (00718660) fills them from the settings and OK (00717C20) stores them. Radio groups list their buttons in the
    // order of the setting's values.
    struct cUI_Options : cUI_Window2
    {
        uint8_t _84[0x158 - 0x84];
        cUI_Control2* cancel;
        cUI_Control2* ok;
        cUI_Control2* detail[3];        // DETAILLEVEL 0, 1, 2
        cUI_Control2* pickupAuto[3];    // PICKUPAUTO 0, 1, 2 (gold, gold and uniques, everything)
        cUI_Control2* pickupAnim;       // PICKUPANIM ("Atmospheric Animation")
        cUI_Control2* netFast;          // NETWORK_SPEEDSETTINGS 2 (DSL / cable / LAN)
        cUI_Control2* netSlow;          // NETWORK_SPEEDSETTINGS 1 (modem / ISDN)
        cUI_Control2* autoTrack;        // AUTOTRACKENEMY
        cUI_Control2* violence;         // VIOLENCE (hidden in builds that don't allow it)
        cUI_Control2* sound;            // SOUND
        cUI_Control2* soundQuality[3];  // SOUNDQUALITY 0, 1, 2
        cUI_Control2* exploreMap;       // EXPLOREMAP
        cUI_Control2* autosave;         // AUTOSAVE
        cUI_Control2* fsaa;             // FSAA_FILTER
        cUI_Slider* sfxVolume;          // SFXVOLUME
        cUI_Slider* voiceVolume;        // VOICEVOLUME
        cUI_Slider* musicVolume;        // MUSICVOLUME
        cUI_Slider* mapAlpha;           // MINIMAP_ALPHA
    };
    static_assert(offsetof(cUI_Options, cancel) == 0x158 && offsetof(cUI_Options, detail) == 0x160);
    static_assert(offsetof(cUI_Options, pickupAuto) == 0x16C && offsetof(cUI_Options, pickupAnim) == 0x178);
    static_assert(offsetof(cUI_Options, violence) == 0x188 && offsetof(cUI_Options, soundQuality) == 0x190);
    static_assert(offsetof(cUI_Options, exploreMap) == 0x19C && offsetof(cUI_Options, fsaa) == 0x1A4);
    static_assert(offsetof(cUI_Options, sfxVolume) == 0x1A8 && offsetof(cUI_Options, mapAlpha) == 0x1B4);

    // The UI manager (one instance): the menu screens, the in-game windows (createGameWindows makes them once per
    // game) and the popups; render draws the letterbox bars and all of them.
    struct cUI_Manager
    {
        // The in-game windows (cUI_Window2 subclasses), with their rects in the 1024x768 layout.
        enum class Window
        {
            taskbar,        // 0,676 1024x92
            inventory,      // 0,388 640x256
            equipment,      // 656,388 256x256
            blacksmith,     // 0,0 640x352
            merchant,       // 0,0 640x336
            megamap,        // world map (M), full screen
            overviewMap,    // the map over the world (Tab)
            minimap,        // UI_WND_MERC: minimap and party portraits, 932,0 92x676
            stats,          // 656,0 256x420
            console,        // chat input, 256,640 512x128
            questbook,      // the log book (cUI_Diary)
            savegame,       // full screen
            escapeMenu,
            _b4,
            master,         // combat art master, 0,0 640x336
            options,        // full screen
            chest,          // 0,0 640x320
            _c4,
            networkInfo,    // the player list of a network game (cUI_NetworkInfo)
            netPortraits,   // 0,0 896x64
            character,      // character export, full screen
            cube,           // 0,0 640x320
            trade,          // 0,0 640x320
            count,
        };

        // In flags.
        static constexpr uint32_t inMenus = 0x01;       // the menu screens are drawn
        static constexpr uint32_t inGame = 0x04;
        static constexpr uint32_t cinematic = 0x10;
        static constexpr uint32_t helpScreen = 0x200;   // the help screen (H key) is shown
        static constexpr int menuCount = 5;
        static constexpr int savegameMenu = 3;          // menus[savegameMenu]: the savegame window (loading)

        uint8_t _00[0x08];
        uint32_t flags;
        uint8_t _0c[0x56 - 0x0C];
        // Indices into `popups` used by the help screen, in the order of its entries.
        uint16_t helpPopups[16];
        uint8_t _76[0x80 - 0x76];
        cUI_Window2* windows[static_cast<int>(Window::count)];
        uint8_t _dc[0x124 - 0xDC];
        Vector<cUI_Popup*> popups;
        // The menu screens (start menu and the screens reached from it), drawn while flags have inMenus.
        cUI_Window2* menus[menuCount];
        cUI_BusyDlg* dialog;        // a message box over them (also in game)
        uint8_t _148[0x14C - 0x148];
        cUI_Window2* other;         // a window of unknown use (the controller's UI trace lists it)

        // nullptr before the game made it.
        static cUI_Manager* instance() { return *Addr::g_pUiManager; }

        cUI_Window2*& window(Window w) { return windows[static_cast<int>(w)]; }
        cUI_Taskbar2* taskbar() { return static_cast<cUI_Taskbar2*>(window(Window::taskbar)); }
        cUI_Inventory3* inventory() { return static_cast<cUI_Inventory3*>(window(Window::inventory)); }
        cUI_Diary* questbook() { return static_cast<cUI_Diary*>(window(Window::questbook)); }
        cUI_Savegame* savegame() { return static_cast<cUI_Savegame*>(window(Window::savegame)); }
        cUI_EscMenu* escapeMenu() { return static_cast<cUI_EscMenu*>(window(Window::escapeMenu)); }
        cUI_NetworkInfo* networkInfo() { return static_cast<cUI_NetworkInfo*>(window(Window::networkInfo)); }

        bool isCursorOverUi(int x, int y) { return Addr::cUI_Manager_isCursorOverUi(this, x, y); }
    };
    static_assert(offsetof(cUI_Manager, flags) == 0x08 && offsetof(cUI_Manager, helpPopups) == 0x56);
    static_assert(offsetof(cUI_Manager, windows) == 0x80 && sizeof(cUI_Manager::windows) == 0xDC - 0x80);
    static_assert(offsetof(cUI_Manager, popups) == 0x124 && offsetof(cUI_Manager, menus) == 0x130);
    static_assert(offsetof(cUI_Manager, dialog) == 0x144 && offsetof(cUI_Manager, other) == 0x14C);

    struct cFont
    {
        struct Vtable
        {
            void* _00[10];
            // The height of a character's line in pixels of the 1024x768 layout (in the low 16 bits).
            uint32_t(__fastcall* lineHeight)(cFont* self, void* edx, uint32_t ch);
            // The text's width in pixels of the 1024x768 layout, the widest line's (cFontTTF2 ENG 00650F70); the other
            // fonts return 0.
            uint16_t(__fastcall* textWidth)(cFont* self, void* edx, const wchar_t* text);
        };

        const Vtable* vtable;

        uint16_t textWidth(const wchar_t* text) { return vtable->textWidth(this, nullptr, text); }
        int16_t lineHeight(wchar_t ch) { return static_cast<int16_t>(vtable->lineHeight(this, nullptr, ch)); }
    };
    static_assert(offsetof(cFont::Vtable, lineHeight) == 0x28 && offsetof(cFont::Vtable, textWidth) == 0x2C);

    // The UI fonts (ENG 00649BE0 makes them).
    struct cFontManager
    {
        Vector<cFont*> fonts;       // by font id

        static cFontManager* instance() { return *Addr::g_pFontManager; }
    };
}
