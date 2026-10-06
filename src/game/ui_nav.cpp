#include "game/ui_nav.h"
#include "game/sacred_addr.h"
#include "game/ui_anchor.h"
#include "game/ui_canvas.h"

#include <windows.h>
#include <algorithm>
#include <initializer_list>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace
{
    using namespace Sacred;

    // thiscall (int32 out[3]) on a control, called as fastcall with an unused EDX.
    using RectFn = void(__fastcall*)(void* control, void* edx, int32_t* out);

    enum Kind : uint8_t
    {
        Other,
        Window,     // has children
        Control,    // something to point the cursor at
        Button,     // ... a button
        GameMenu,   // the game menu: its entries are in a vector of their own (Sacred::EscMenu)
        MainMenu,   // the start menu: a vector per screen (Sacred::MainMenu)
    };

    // RTTI class names (MSVC decorated) of the controls D-pad navigation stops at; their subclasses count too.
    constexpr const char* kButtons[] = {".?AVcUI_Button2@@", ".?AVcUI_RadioButton2@@"};
    constexpr const char* kControls[] = {
        ".?AVcUI_StaticText64FX@@", ".?AVcUI_Combobox@@", ".?AVcUI_EditControl@@", ".?AVcUI_DigitEdit@@",
        ".?AVcUI_Listbox@@", ".?AVcUI_Listbox2@@",
    };
    constexpr const char* kWindow = ".?AVcUI_Window2@@";
    constexpr const char* kGameMenu = ".?AVcUI_EscMenu@@";
    constexpr const char* kMainMenu = ".?AVcUI_MainMenu@@";
    constexpr size_t kMaxControls = 512;

    // By vtable; only the presenting thread asks.
    std::unordered_map<const void*, Kind> g_kinds;
    uintptr_t g_imageBegin = 0, g_imageEnd = 0;

    template <class T>
    T& member(void* obj, uintptr_t offset)
    {
        return *reinterpret_cast<T*>(static_cast<uint8_t*>(obj) + offset);
    }

    bool inImage(uintptr_t address)
    {
        if (!g_imageBegin)
        {
            const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
            g_imageBegin = base;
            g_imageEnd = base + nt->OptionalHeader.SizeOfImage;
        }
        return address >= g_imageBegin && address + 16 <= g_imageEnd;
    }

    uintptr_t read(uintptr_t address)
    {
        return *reinterpret_cast<const uintptr_t*>(address);
    }

    // The object's class from its vtable's RTTI: complete object locator at vtable[-1] (+0x10 class hierarchy
    // descriptor: +8 number of base classes, +0xC base class array, each entry's +0 a type descriptor with the
    // decorated name at +8; the class itself comes first).
    Kind kindOf(void* object)
    {
        const auto vtable = read(reinterpret_cast<uintptr_t>(object));
        if (!inImage(vtable))
        {
            return Other;
        }
        const auto cached = g_kinds.find(reinterpret_cast<const void*>(vtable));
        if (cached != g_kinds.end())
        {
            return cached->second;
        }
        Kind kind = Other;
        const uintptr_t locator = read(vtable - 4);
        const uintptr_t hierarchy = inImage(locator) ? read(locator + 0x10) : 0;
        const uintptr_t bases = inImage(hierarchy) ? read(hierarchy + 0x0C) : 0;
        if (inImage(bases))
        {
            const uint32_t count = std::min<uint32_t>(*reinterpret_cast<const uint32_t*>(hierarchy + 8), 64);
            for (uint32_t i = 0; i < count && kind != Button && kind != GameMenu && kind != MainMenu &&
                inImage(bases + i * 4); ++i)
            {
                const uintptr_t base = read(bases + i * 4);
                const uintptr_t type = inImage(base) ? read(base) : 0;
                if (!inImage(type))
                {
                    continue;
                }
                const char* name = reinterpret_cast<const char*>(type + 8);
                if (i == 0 && std::strcmp(name, kGameMenu) == 0)
                {
                    kind = GameMenu;    // the class itself (the first entry)
                }
                if (i == 0 && std::strcmp(name, kMainMenu) == 0)
                {
                    kind = MainMenu;
                }
                for (const char* button : kButtons)
                {
                    if (std::strcmp(name, button) == 0)
                    {
                        kind = Button;
                    }
                }
                for (const char* control : kControls)
                {
                    if (kind == Other && std::strcmp(name, control) == 0)
                    {
                        kind = Control;
                    }
                }
                if (kind == Other && std::strcmp(name, kWindow) == 0)
                {
                    kind = Window;
                }
            }
        }
        g_kinds.emplace(reinterpret_cast<const void*>(vtable), kind);
        return kind;
    }

    bool visible(void* control)
    {
        return control && (member<uint32_t>(control, UiControl::flags) & 1);
    }

    struct Point
    {
        float x, y;
        bool button;
    };

    // A point of the 1024x768 layout of a window shifted by `frame`, in screen pixels.
    Point screen(float x, float y, const UiCanvas::Frame& frame, bool button = false)
    {
        return {static_cast<float>(UiCanvas::toPhysicalX(static_cast<int>(std::lround(x + frame.x)))),
            static_cast<float>(UiCanvas::toPhysicalY(static_cast<int>(std::lround(y + frame.y)))), button};
    }

    // A control's center (its parents' positions added, as the game hit-tests it), false if it has no sensible size.
    bool center(void* control, float& x, float& y)
    {
        int32_t rect[3] = {};
        reinterpret_cast<RectFn>(Addr::cUI_Control2_getAbsoluteRect)(control, nullptr, rect);
        const int w = static_cast<int16_t>(rect[2] & 0xFFFF), h = static_cast<int16_t>(rect[2] >> 16);
        if (w <= 0 || h <= 0 || w > 1024 || h > 768)
        {
            return false;
        }
        x = rect[0] + w * 0.5f;
        y = rect[1] + h * 0.5f;
        return true;
    }

    void addControl(void* control, bool button, const UiCanvas::Frame& frame, std::vector<Point>& out)
    {
        float x, y;
        if (out.size() < kMaxControls && center(control, x, y))
        {
            out.push_back(screen(x, y, frame, button));
        }
    }

    // The visible controls under `control` (in the 1024x768 layout of a window shifted by `frame`), as screen points.
    void collect(void* control, const UiCanvas::Frame& frame, std::vector<Point>& out, int depth)
    {
        if (depth > 8 || out.size() >= kMaxControls || !visible(control))
        {
            return;
        }
        const Kind kind = kindOf(control);
        if (kind == Control || kind == Button)
        {
            addControl(control, kind == Button, frame, out);
            return;
        }
        if (kind == GameMenu || kind == MainMenu)
        {
            uintptr_t list = EscMenu::entriesBegin;
            if (kind == MainMenu)
            {
                const uint32_t screen = member<uint32_t>(control, MainMenu::screen);
                if (screen >= MainMenu::screensWithEntries)
                {
                    return;
                }
                list = MainMenu::entries + screen * MainMenu::entriesStride;
            }
            void** entry = member<void**>(control, list);
            void** end = member<void**>(control, list + 4);
            for (; entry && entry < end && end - entry <= 16; ++entry)
            {
                if (visible(*entry))
                {
                    addControl(*entry, false, frame, out);
                }
            }
            return;
        }
        if (kind == Window)
        {
            void** child = member<void**>(control, UiWindow::childrenBegin);
            void** end = member<void**>(control, UiWindow::childrenEnd);
            if (child && end >= child && static_cast<size_t>(end - child) <= kMaxControls)
            {
                for (; child < end; ++child)
                {
                    collect(*child, frame, out, depth + 1);
                }
            }
        }
    }

    // What the UI manager shows: the menu screens, or in game the open windows (a full-screen one alone, as the
    // game draws it), and the dialog over either.
    void* manager()
    {
        return *reinterpret_cast<void**>(Addr::g_pUiManager);
    }

    UiCanvas::Frame frameOf(void* window)
    {
        UiCanvas::Frame frame;
        if (!UiAnchor::frame(window, frame))
        {
            frame = {};
        }
        return frame;
    }

    void* messageBox()
    {
        void* manager = ::manager();
        void* box = manager ? member<void*>(manager, UiManagerMenus::dialog) : nullptr;
        return visible(box) ? box : nullptr;
    }

    void collectWindow(void* window, std::vector<Point>& out)
    {
        const UiCanvas::Frame frame = frameOf(window);
        collect(window, frame, out, 0);
        if (window == messageBox())
        {
            // Its buttons are members (Sacred::BusyDlg): every visible button object inside it.
            for (uintptr_t offset = 4; offset + 0x30 <= BusyDlg::size; offset += 4)
            {
                void* inside = static_cast<uint8_t*>(window) + offset;
                if (visible(inside) && kindOf(inside) == Button)
                {
                    addControl(inside, true, frame, out);
                }
            }
        }
    }

    std::vector<Point> controls()
    {
        std::vector<Point> out;
        void* manager = ::manager();
        if (!manager)
        {
            return out;
        }
        if (void* window = UiNav::modal())
        {
            collectWindow(window, out);
            return out;
        }
        const auto add = [&](uintptr_t offset) {
            void* window = member<void*>(manager, offset);
            if (visible(window))
            {
                collectWindow(window, out);
            }
        };
        const uint32_t flags = member<uint32_t>(manager, UiManager::flags);
        if (flags & 0x01)
        {
            for (uintptr_t offset = UiManagerMenus::first; offset <= UiManagerMenus::last; offset += 4)
            {
                add(offset);
            }
        }
        else if (flags & UiManager::inGame)
        {
            bool alone = false;
            for (uintptr_t offset : {UiManager::savegame, UiManager::options, UiManager::character, UiManager::megamap})
            {
                if (visible(member<void*>(manager, offset)))
                {
                    add(offset);
                    alone = true;
                }
            }
            if (!alone)
            {
                for (uintptr_t offset = UiManager::firstGameWindow; offset <= UiManager::lastGameWindow; offset += 4)
                {
                    if (offset != UiManager::overviewMap && offset != UiManager::console)
                    {
                        add(offset);
                    }
                }
            }
        }
        return out;
    }
}

std::string UiNav::className(void* object)
{
    const uintptr_t vtable = object ? read(reinterpret_cast<uintptr_t>(object)) : 0;
    const uintptr_t locator = inImage(vtable) ? read(vtable - 4) : 0;
    const uintptr_t type = inImage(locator) ? read(locator + 0x0C) : 0;
    if (!inImage(type))
    {
        return "?";
    }
    std::string name(reinterpret_cast<const char*>(type + 8));   // ".?AVname@@"
    if (name.rfind(".?AV", 0) == 0 && name.size() > 6)
    {
        name = name.substr(4, name.size() - 6);
    }
    return name;
}

std::string UiNav::contents(void* window)
{
    std::string out;
    const auto walk = [&](auto&& self, void* control, int depth) -> void {
        if (depth > 3 || out.size() > 300 || kindOf(control) != Window)
        {
            return;
        }
        void** child = member<void**>(control, UiWindow::childrenBegin);
        void** end = member<void**>(control, UiWindow::childrenEnd);
        for (; child && child < end && end - child <= 64; ++child)
        {
            if (visible(*child))
            {
                out += className(*child) + " ";
                self(self, *child, depth + 1);
            }
        }
    };
    walk(walk, window, 0);
    return out;
}

bool UiNav::defaultButton(void* window, float& outX, float& outY)
{
    std::vector<Point> controls;
    collectWindow(window, controls);
    bool found = false;
    for (const Point& p : controls)
    {
        if (p.button && (!found || p.y > outY + 4.0f || (std::fabs(p.y - outY) <= 4.0f && p.x < outX)))
        {
            outX = p.x;
            outY = p.y;
            found = true;
        }
    }
    return found;
}

bool UiNav::cancelButton(float& outX, float& outY)
{
    void* box = messageBox();
    if (!box)
    {
        return false;
    }
    std::vector<Point> controls;
    collectWindow(box, controls);
    bool found = false;
    for (const Point& p : controls)
    {
        if (p.button && (!found || p.y > outY + 4.0f || (std::fabs(p.y - outY) <= 4.0f && p.x > outX)))
        {
            outX = p.x;
            outY = p.y;
            found = true;
        }
    }
    return found;
}

int UiNav::buttonCount(void* window)
{
    std::vector<Point> controls;
    collectWindow(window, controls);
    return static_cast<int>(std::count_if(controls.begin(), controls.end(), [](const Point& p) { return p.button; }));
}

void* UiNav::modal()
{
    void* manager = ::manager();
    if (!manager)
    {
        return nullptr;
    }
    if (void* box = messageBox())
    {
        return box;
    }
    const uint32_t flags = member<uint32_t>(manager, UiManager::flags);
    if (!(flags & 0x01) && (flags & UiManager::inGame))
    {
        if (void* menu = member<void*>(manager, UiManager::escapeMenu); visible(menu))
        {
            return menu;
        }
    }
    return nullptr;
}

bool UiNav::home(void* window, float& outX, float& outY)
{
    void* manager = ::manager();
    if (manager && window == member<void*>(manager, UiManagerMenus::dialog))
    {
        return defaultButton(window, outX, outY);
    }
    std::vector<Point> controls;
    collectWindow(window, controls);
    bool found = false;
    for (const Point& p : controls)
    {
        if (!found || p.y < outY - 4.0f || (std::fabs(p.y - outY) <= 4.0f && p.x < outX))
        {
            outX = p.x;
            outY = p.y;
            found = true;
        }
    }
    return found;
}

namespace
{
    // The visible popup with answers (an NPC dialog), nullptr if there is none.
    void* npcDialog()
    {
        void* manager = ::manager();
        if (!manager)
        {
            return nullptr;
        }
        void** popup = member<void**>(manager, UiManager::popupsBegin);
        void** end = member<void**>(manager, UiManager::popupsEnd);
        for (; popup && popup < end && end - popup < 256; ++popup)
        {
            if (!visible(*popup) || UiNav::className(*popup) != "cUI_Tooltip")
            {
                continue;
            }
            for (int i = 0; i < Popup::answerCount; ++i)
            {
                if (member<uint32_t>(*popup, Popup::answers + i * Popup::answerSize + Popup::answerText))
                {
                    return *popup;
                }
            }
        }
        return nullptr;
    }
}

bool UiNav::npcDialogOpen()
{
    return npcDialog() != nullptr;
}

bool UiNav::npcAnswer(int index, float& outX, float& outY)
{
    void* popup = npcDialog();
    if (!popup || index < 0 || index >= Popup::answerCount)
    {
        return false;
    }
    const uintptr_t answer = Popup::answers + index * Popup::answerSize;
    const int16_t w = member<int16_t>(popup, answer + Popup::answerWidth);
    const int16_t h = member<int16_t>(popup, answer + Popup::answerHeight);
    if (!member<uint32_t>(popup, answer + Popup::answerText) || w <= 0 || h <= 0)
    {
        return false;
    }
    // The layout places the answers relative to the popup.
    int32_t rect[3] = {};
    reinterpret_cast<RectFn>(Addr::cUI_Control2_getAbsoluteRect)(popup, nullptr, rect);
    UiCanvas::Frame frame;
    if (!UiAnchor::frame(popup, frame))
    {
        frame = {};
    }
    const float cx = rect[0] + member<int32_t>(popup, answer + Popup::answerX) + w * 0.5f + frame.x;
    const float cy = rect[1] + member<int32_t>(popup, answer + Popup::answerY) + h * 0.5f + frame.y;
    outX = static_cast<float>(UiCanvas::toPhysicalX(static_cast<int>(std::lround(cx))));
    outY = static_cast<float>(UiCanvas::toPhysicalY(static_cast<int>(std::lround(cy))));
    return true;
}

namespace
{
    // The open log book's book (the selected top tab's), nullptr if the book is closed.
    void* currentBook(void*& diary)
    {
        diary = UiNav::logBook();
        if (!diary)
        {
            return nullptr;
        }
        const uint16_t tab = member<uint16_t>(diary, Diary::currentTab);
        void** begin = member<void**>(diary, Diary::booksBegin);
        void** end = member<void**>(diary, Diary::booksEnd);
        return begin && tab < end - begin && end - begin <= Diary::tabCount ? begin[tab] : nullptr;
    }

    // The center of the visible control in `controls` (count of them, stride bytes apart) `step` places from
    // `current` (skipping hidden ones), as a screen point in `window`'s frame.
    bool stepTo(void* window, uint8_t* controls, uintptr_t stride, int count, int current, int step, float& outX,
        float& outY)
    {
        for (int i = current + step; i >= 0 && i < count; i += step)
        {
            void* control = controls + i * stride;
            float x, y;
            if (visible(control) && center(control, x, y))
            {
                const Point p = screen(x, y, frameOf(window));
                outX = p.x;
                outY = p.y;
                return true;
            }
        }
        return false;
    }
}

void* UiNav::logBook()
{
    void* manager = ::manager();
    if (!manager)
    {
        return nullptr;
    }
    const uint32_t flags = member<uint32_t>(manager, UiManager::flags);
    void* diary = member<void*>(manager, UiManager::questbook);
    return !(flags & 0x01) && (flags & UiManager::inGame) && visible(diary) ? diary : nullptr;
}

bool UiNav::bookTab(int step, float& outX, float& outY)
{
    void* diary = logBook();
    if (!diary)
    {
        return false;
    }
    int current = member<uint16_t>(diary, Diary::currentTab);
    if (current >= Diary::tabCount)
    {
        current = step > 0 ? -1 : Diary::tabCount;
    }
    return stepTo(diary, static_cast<uint8_t*>(diary) + Diary::tabs, Diary::tabSize, Diary::tabCount, current, step,
        outX, outY);
}

bool UiNav::bookSection(int step, float& outX, float& outY)
{
    void* diary;
    void* book = currentBook(diary);
    if (!book)
    {
        return false;
    }
    return stepTo(diary, static_cast<uint8_t*>(book) + Book::verticalTabs, Book::verticalTabSize,
        Book::verticalTabCount, member<uint16_t>(book, Book::selectedTab), step, outX, outY);
}

bool UiNav::bookPage(bool right, int step, float& outX, float& outY)
{
    void* diary;
    void* book = currentBook(diary);
    if (!book)
    {
        return false;
    }
    const uintptr_t offset = right ? (step > 0 ? Book::rightNext : Book::rightPrevious)
                                   : (step > 0 ? Book::leftNext : Book::leftPrevious);
    void* button = static_cast<uint8_t*>(book) + offset;
    float x, y;
    if (!visible(button) || !center(button, x, y))
    {
        return false;   // the page buttons hide at the first and last page
    }
    const Point p = screen(x, y, frameOf(diary));
    outX = p.x;
    outY = p.y;
    return true;
}

bool UiNav::bookEntry(int step, float& outX, float& outY)
{
    using LineAtFn = bool(__fastcall*)(void* book, void* edx, int x, int y, uint16_t* index);
    void* diary;
    void* book = currentBook(diary);
    if (!book)
    {
        return false;
    }
    // The selected entry of the list's page.
    const uint16_t tab = member<uint16_t>(book, Book::selectedTab);
    uint8_t* pages = member<uint8_t*>(book, Book::pages + tab * Book::pagesStride);
    uint8_t* pagesEnd = member<uint8_t*>(book, Book::pages + tab * Book::pagesStride + 4);
    const uint16_t page = member<uint16_t>(book, Book::leftPage);
    if (tab >= Book::verticalTabCount || !pages || pagesEnd < pages ||
        page >= static_cast<size_t>(pagesEnd - pages) / Book::pageSize)
    {
        return false;
    }
    const int selected = member<int32_t>(pages + page * Book::pageSize, Book::pageSelected);
    // Where the entries are: the game's own hit test down the left page, at a few points across it.
    int32_t rect[3] = {};
    reinterpret_cast<RectFn>(Addr::cUI_Control2_getAbsoluteRect)(book, nullptr, rect);
    const int left = rect[0] + Book::listX, top = rect[1] + Book::listY;
    struct Entry
    {
        int index, top, bottom;
    };
    std::vector<Entry> entries;
    int column = 0;
    for (int x : {left + 0x20, left + Book::listWidth / 2, left + Book::listWidth - 0x20})
    {
        for (int y = top; y < top + Book::listHeight; y += 3)
        {
            uint16_t index = 0;
            if (!reinterpret_cast<LineAtFn>(Addr::cUI_Book_lineAt)(book, nullptr, x, y, &index))
            {
                continue;
            }
            if (!entries.empty() && entries.back().index == index)
            {
                entries.back().bottom = y;
            }
            else
            {
                entries.push_back({index, y, y});
            }
        }
        if (!entries.empty())
        {
            column = x;
            break;
        }
    }
    // The next one that way.
    const Entry* best = nullptr;
    for (const Entry& e : entries)
    {
        if (step > 0 ? e.index > selected && (!best || e.index < best->index)
                     : e.index < selected && (!best || e.index > best->index))
        {
            best = &e;
        }
    }
    if (!best)
    {
        return false;
    }
    const Point p = screen(static_cast<float>(column), (best->top + best->bottom) * 0.5f, frameOf(diary));
    outX = p.x;
    outY = p.y;
    return true;
}

bool UiNav::inventoryTab(int step, float& outX, float& outY)
{
    void* manager = ::manager();
    void* inventory = manager ? member<void*>(manager, UiManager::inventory) : nullptr;
    void* tabs = visible(inventory) ? member<void*>(inventory, Inventory::tabs) : nullptr;
    void* control = tabs ? member<void*>(tabs, Inventory::tabControl) : nullptr;
    if (!control || className(control) != "cUI_TabControl")
    {
        return false;
    }
    uint8_t* pages = member<uint8_t*>(control, TabControl::pagesBegin);
    uint8_t* end = member<uint8_t*>(control, TabControl::pagesEnd);
    if (!pages || end < pages || static_cast<size_t>(end - pages) > 16 * TabControl::pageSize)
    {
        return false;
    }
    const int count = static_cast<int>((end - pages) / TabControl::pageSize);
    for (int i = member<uint16_t>(control, TabControl::activePage) + step; i >= 0 && i < count; i += step)
    {
        void* button = member<void*>(pages + i * TabControl::pageSize, TabControl::pageButton);
        float x, y;
        if (button && visible(button) && center(button, x, y))
        {
            const Point p = screen(x, y + 2.0f, frameOf(inventory));   // the hit test starts 4 pixels lower
            outX = p.x;
            outY = p.y;
            return true;
        }
    }
    return false;
}

bool UiNav::next(float x, float y, float dx, float dy, float& outX, float& outY)
{
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length < 1e-3f)
    {
        return false;
    }
    dx /= length;
    dy /= length;
    bool found = false;
    float best = 0.0f;
    for (const Point& p : controls())
    {
        const float vx = p.x - x, vy = p.y - y;
        const float along = vx * dx + vy * dy;
        if (along < 8.0f)
        {
            continue;
        }
        // Straight ahead first: sideways distance counts double.
        const float score = along + 2.0f * std::fabs(vx * dy - vy * dx);
        if (!found || score < best)
        {
            best = score;
            outX = p.x;
            outY = p.y;
            found = true;
        }
    }
    return found;
}
