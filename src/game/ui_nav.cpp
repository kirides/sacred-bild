#include "game/ui_nav.h"
#include "game/ui_anchor.h"
#include "game/ui_canvas.h"
#include "sacred/text.h"
#include "sacred/ui.h"

#include <windows.h>
#include <algorithm>
#include <initializer_list>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    using namespace Sacred;

    enum Kind : uint8_t
    {
        Other,
        Window,    // has children
        Control,    // something to point the cursor at
        Button,     // ... a button
        GameMenu,   // the game menu: its entries are in a vector of their own (cUI_EscMenu)
        MainMenu,   // the start menu: a vector per screen (cUI_MainMenu)
        Savegames,  // the savegame window: a window whose list rows are hit rects (cUI_Savegame)
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
    constexpr const char* kSavegames = ".?AVcUI_Savegame@@";
    constexpr size_t kMaxControls = 512;

    // By vtable; only the presenting thread asks.
    std::unordered_map<const void*, Kind> g_kinds;
    uintptr_t g_imageBegin = 0, g_imageEnd = 0;

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
    Kind kindOf(const void* object)
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
                kind != Savegames && inImage(bases + i * 4); ++i)
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
                if (i == 0 && std::strcmp(name, kSavegames) == 0)
                {
                    kind = Savegames;
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

    bool visible(const cUI_Control2* control)
    {
        return control && control->visible();
    }

    struct Point
    {
        float x, y;
        bool button;
        UiNav::Rect rect;   // the control's, screen pixels
        cUI_Control2* control = nullptr;
        UiCanvas::Frame frame{};
    };

    float physicalX(float x, const UiCanvas::Frame& frame)
    {
        return static_cast<float>(UiCanvas::toPhysicalX(static_cast<int>(std::lround(x + frame.x))));
    }

    float physicalY(float y, const UiCanvas::Frame& frame)
    {
        return static_cast<float>(UiCanvas::toPhysicalY(static_cast<int>(std::lround(y + frame.y))));
    }

    // A point of the 1024x768 layout of a window shifted by `frame`, in screen pixels.
    Point screen(float x, float y, const UiCanvas::Frame& frame, bool button = false)
    {
        const float px = physicalX(x, frame), py = physicalY(y, frame);
        return {px, py, button, {px, py, px, py}};
    }

    // A control's rect in the 1024x768 layout (its parents' positions added, as the game hit-tests it), false if it
    // has no sensible size.
    bool layoutRect(cUI_Control2* control, float& left, float& top, float& width, float& height)
    {
        const UiRect r = control->absoluteRect();
        if (r.width <= 0 || r.height <= 0 || r.width > 1024 || r.height > 768)
        {
            return false;
        }
        left = static_cast<float>(r.x);
        top = static_cast<float>(r.y);
        width = static_cast<float>(r.width);
        height = static_cast<float>(r.height);
        return true;
    }

    bool center(cUI_Control2* control, float& x, float& y)
    {
        float left, top, w, h;
        if (!layoutRect(control, left, top, w, h))
        {
            return false;
        }
        x = left + w * 0.5f;
        y = top + h * 0.5f;
        return true;
    }

    // A layout rect in screen pixels.
    UiNav::Rect screenRect(float left, float top, float w, float h, const UiCanvas::Frame& frame)
    {
        return {physicalX(left, frame), physicalY(top, frame), physicalX(left + w, frame), physicalY(top + h, frame)};
    }

    void addControl(cUI_Control2* control, bool button, const UiCanvas::Frame& frame, std::vector<Point>& out)
    {
        float left, top, w, h;
        if (out.size() < kMaxControls && layoutRect(control, left, top, w, h))
        {
            Point p = screen(left + w * 0.5f, top + h * 0.5f, frame, button);
            p.rect = screenRect(left, top, w, h, frame);
            p.control = control;
            p.frame = frame;
            out.push_back(p);
        }
    }

    // A menu's entries (not its children), at most 16.
    template <class T>
    void addEntries(const Vector<T*>& entries, const UiCanvas::Frame& frame, std::vector<Point>& out)
    {
        if (entries.size() > 16)
        {
            return;
        }
        for (T* entry : entries)
        {
            if (visible(entry))
            {
                addControl(entry, false, frame, out);
            }
        }
    }

    // ---- The savegame window's list (cUI_Savegame) ----

    // Saving or loading, also while a message box asks about it; 0 if neither.
    uint32_t listMode(const cUI_Savegame* window)
    {
        uint32_t mode = window->mode;
        if (mode > cUI_Savegame::loading)
        {
            mode = window->askedMode;
        }
        return mode == cUI_Savegame::saving || mode == cUI_Savegame::loading ? mode : 0;
    }

    uint32_t listed(const cUI_Savegame* window)
    {
        return static_cast<uint32_t>(window->entries.size());
    }

    // The first listed savegame, row 1's, as the window works it out (ENG 0071C2B0).
    uint32_t firstListed(cUI_Savegame* window)
    {
        const uint32_t value = window->slider.value() & 0xFFFF;
        const uint32_t last = window->slider.count - (listMode(window) == cUI_Savegame::saving ? 2 : 3);
        return last <= value ? last : value;
    }

    // The row shows a savegame, or the new one (the row past the end) when saving.
    bool rowShows(cUI_Savegame* window, int row)
    {
        if (row == 0)
        {
            return window->quicksave.name[0] != '\0';
        }
        const uint32_t index = firstListed(window) + row - 1;
        return row > 0 && row < cUI_Savegame::rowCount &&
            index < listed(window) + (listMode(window) == cUI_Savegame::saving ? 1 : 0);
    }

    // Sets the first listed savegame (the slider; the window renders the rows anew when it changes).
    void scrollList(cUI_Savegame* window, uint32_t first)
    {
        window->slider.setValue(first);
    }

    UiNav::Rect rowRect(const cUI_Savegame* window, int row, const UiCanvas::Frame& frame)
    {
        const UiRect& r = window->rows[row];
        return screenRect(static_cast<float>(r.x), static_cast<float>(r.y), r.width, r.height, frame);
    }

    // The visible controls under `control` (in the 1024x768 layout of a window shifted by `frame`), as screen points.
    void collect(cUI_Control2* control, const UiCanvas::Frame& frame, std::vector<Point>& out, int depth)
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
        if (kind == Savegames && listMode(static_cast<cUI_Savegame*>(control)))
        {
            auto* window = static_cast<cUI_Savegame*>(control);
            for (int row = 0; row < cUI_Savegame::rowCount; ++row)
            {
                if (rowShows(window, row))
                {
                    const UiNav::Rect r = rowRect(window, row, frame);
                    out.push_back({(r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f, false, r});
                }
            }
            for (cUI_Button2& button : window->buttons)
            {
                if (visible(&button) && kindOf(&button) == Button)
                {
                    addControl(&button, true, frame, out);
                }
            }
        }
        if (kind == GameMenu)
        {
            addEntries(static_cast<cUI_EscMenu*>(control)->entries, frame, out);
            return;
        }
        if (kind == MainMenu)
        {
            auto* menu = static_cast<cUI_MainMenu*>(control);
            if (menu->screen < cUI_MainMenu::screensWithEntries)
            {
                addEntries(menu->entries[menu->screen], frame, out);
            }
            return;
        }
        if (kind == Window || kind == Savegames)
        {
            const Vector<cUI_Control2*>& children = static_cast<cUI_Window2*>(control)->children;
            if (children.size() <= kMaxControls)
            {
                for (cUI_Control2* child : children)
                {
                    collect(child, frame, out, depth + 1);
                }
            }
        }
    }

    // What the UI manager shows: the menu screens, or in game the open windows (a full-screen one alone, as the
    // game draws it), and the dialog over either.
    cUI_Manager* manager()
    {
        return cUI_Manager::instance();
    }

    UiCanvas::Frame frameOf(cUI_Window2* window)
    {
        UiCanvas::Frame frame;
        if (!UiAnchor::frame(window, frame))
        {
            frame = {};
        }
        return frame;
    }

    cUI_Window2* messageBox()
    {
        cUI_Manager* manager = ::manager();
        cUI_Window2* box = manager ? manager->dialog : nullptr;
        return visible(box) ? box : nullptr;
    }

    void collectWindow(cUI_Window2* window, std::vector<Point>& out)
    {
        const UiCanvas::Frame frame = frameOf(window);
        collect(window, frame, out, 0);
        if (window == messageBox())
        {
            // Its buttons are members (cUI_BusyDlg): every visible button object inside it.
            for (uintptr_t offset = 4; offset + 0x30 <= sizeof(cUI_BusyDlg); offset += 4)
            {
                auto* inside = reinterpret_cast<cUI_Control2*>(reinterpret_cast<uint8_t*>(window) + offset);
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
        cUI_Manager* manager = ::manager();
        if (!manager)
        {
            return out;
        }
        if (cUI_Window2* window = UiNav::modal())
        {
            collectWindow(window, out);
            return out;
        }
        const auto add = [&](cUI_Window2* window) {
            if (visible(window))
            {
                collectWindow(window, out);
            }
        };
        const uint32_t flags = manager->flags;
        if (flags & cUI_Manager::inMenus)
        {
            for (cUI_Window2* menu : manager->menus)
            {
                add(menu);
            }
        }
        else if (flags & cUI_Manager::inGame)
        {
            using W = cUI_Manager::Window;
            bool alone = false;
            for (W w : {W::savegame, W::options, W::character, W::megamap})
            {
                if (visible(manager->window(w)))
                {
                    add(manager->window(w));
                    alone = true;
                }
            }
            if (!alone)
            {
                for (int i = 0; i < static_cast<int>(W::count); ++i)
                {
                    if (i != static_cast<int>(W::overviewMap) && i != static_cast<int>(W::console))
                    {
                        add(manager->windows[i]);
                    }
                }
            }
        }
        return out;
    }
}

std::string UiNav::className(const void* object)
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

std::string UiNav::contents(cUI_Window2* window)
{
    std::string out;
    const auto walk = [&](auto&& self, cUI_Control2* control, int depth) -> void {
        if (depth > 3 || out.size() > 300 || (kindOf(control) != Window && kindOf(control) != Savegames))
        {
            return;
        }
        const Vector<cUI_Control2*>& children = static_cast<cUI_Window2*>(control)->children;
        if (children.size() > 64)
        {
            return;
        }
        for (cUI_Control2* child : children)
        {
            if (visible(child))
            {
                out += className(child) + " ";
                self(self, child, depth + 1);
            }
        }
    };
    walk(walk, window, 0);
    return out;
}

bool UiNav::defaultButton(cUI_Window2* window, float& outX, float& outY)
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
    cUI_Window2* box = messageBox();
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

namespace
{
    // A menu entry whose text is drawn centered in its rect (cUI_StaticText64, cUI_StaticText64FX): the text's width
    // in the 1024x768 layout, as its font measures it; 0 for other controls or a font that does not measure.
    float centeredTextWidth(cUI_Control2* control)
    {
        const std::string name = UiNav::className(control);
        const bool fx = name == "cUI_StaticText64FX";
        if ((!fx && name != "cUI_StaticText64") || !(control->flags & cUI_StaticText64::centered))
        {
            return 0.0f;
        }
        const wchar_t* begin = nullptr;
        const wchar_t* end = nullptr;
        uint32_t id;
        uint16_t fontId;
        if (fx)
        {
            const auto* text = static_cast<const cUI_StaticText64FX*>(control);
            id = text->textId;
            fontId = text->font;
        }
        else
        {
            const auto* text = static_cast<const cUI_StaticText64*>(control);
            id = text->textId;
            fontId = text->font;
            begin = text->text;
            end = text->textEnd;
        }
        if (id)
        {
            begin = end = nullptr;
            cTextResources* texts = cTextResources::instance();
            if (const wchar_t* const* text = texts ? texts->text(id) : nullptr)
            {
                begin = text[0];
                end = text[1];
            }
        }
        cFontManager* fonts = cFontManager::instance();
        if (!begin || end <= begin || end - begin > 256 || !fonts)
        {
            return 0.0f;
        }
        cFont* font = fontId < fonts->fonts.size() ? fonts->fonts[fontId] : nullptr;
        if (!font)
        {
            return 0.0f;
        }
        const std::wstring text(begin, end);
        return font->textWidth(text.c_str());
    }
}

bool UiNav::controlAt(float x, float y, Rect& out)
{
    const Point* best = nullptr;
    float area = 0.0f;
    const std::vector<Point>& points = controls();
    for (const Point& p : points)
    {
        const Rect& r = p.rect;
        const float a = (r.right - r.left) * (r.bottom - r.top);
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom && (!best || a < area))
        {
            best = &p;
            area = a;
        }
    }
    if (!best)
    {
        return false;
    }
    out = best->rect;
    // A centered menu text: its own extent, which may be much narrower than its rect (or wider).
    float left, top, w, h;
    if (const float width = best->control ? centeredTextWidth(best->control) : 0.0f;
        width > 0.0f && layoutRect(best->control, left, top, w, h))
    {
        const float center = left + w * 0.5f;
        out.left = physicalX(center - width * 0.5f, best->frame);
        out.right = physicalX(center + width * 0.5f, best->frame);
    }
    return true;
}

bool UiNav::controlRect(cUI_Control2* control, cUI_Window2* window, Rect& out)
{
    float left, top, w, h;
    if (!visible(control) || !layoutRect(control, left, top, w, h))
    {
        return false;
    }
    out = screenRect(left, top, w, h, frameOf(window));
    return true;
}

int UiNav::buttonCount(cUI_Window2* window)
{
    std::vector<Point> controls;
    collectWindow(window, controls);
    return static_cast<int>(std::count_if(controls.begin(), controls.end(), [](const Point& p) { return p.button; }));
}

cUI_Window2* UiNav::modal()
{
    cUI_Manager* manager = ::manager();
    if (!manager)
    {
        return nullptr;
    }
    if (cUI_Window2* box = messageBox())
    {
        return box;
    }
    const uint32_t flags = manager->flags;
    if (!(flags & cUI_Manager::inMenus) && (flags & cUI_Manager::inGame))
    {
        if (cUI_Window2* menu = manager->escapeMenu(); visible(menu))
        {
            return menu;
        }
    }
    return nullptr;
}

bool UiNav::home(cUI_Window2* window, float& outX, float& outY)
{
    cUI_Manager* manager = ::manager();
    if (manager && window == manager->dialog)
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
    cUI_Popup* npcDialog()
    {
        cUI_Manager* manager = ::manager();
        if (!manager || manager->popups.size() >= 256)
        {
            return nullptr;
        }
        for (cUI_Popup* popup : manager->popups)
        {
            if (!visible(popup) || UiNav::className(popup) != "cUI_Tooltip")
            {
                continue;
            }
            for (const cUI_Popup::Answer& answer : popup->answers)
            {
                if (answer.text)
                {
                    return popup;
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
    Rect r;
    if (!npcAnswerRect(index, r))
    {
        return false;
    }
    outX = std::round((r.left + r.right) * 0.5f);
    outY = std::round((r.top + r.bottom) * 0.5f);
    return true;
}

bool UiNav::npcAnswerRect(int index, Rect& out)
{
    cUI_Popup* popup = npcDialog();
    if (!popup || index < 0 || index >= cUI_Popup::answerCount)
    {
        return false;
    }
    const cUI_Popup::Answer& answer = popup->answers[index];
    if (!answer.text || answer.width <= 0 || answer.height <= 0)
    {
        return false;
    }
    // The layout places the answers relative to the popup.
    const UiRect rect = popup->absoluteRect();
    UiCanvas::Frame frame;
    if (!UiAnchor::frame(popup, frame))
    {
        frame = {};
    }
    out = screenRect(static_cast<float>(rect.x + answer.x), static_cast<float>(rect.y + answer.y), answer.width,
        answer.height, frame);
    return true;
}

namespace
{
    // The open log book's book (the selected top tab's), nullptr if the book is closed.
    cUI_Book* currentBook(cUI_Diary*& diary)
    {
        diary = UiNav::logBook();
        if (!diary)
        {
            return nullptr;
        }
        const uint16_t tab = diary->currentTab;
        const Vector<cUI_Book*>& books = diary->books;
        return tab < books.size() && books.size() <= cUI_Diary::tabCount ? books[tab] : nullptr;
    }

    // The center of the visible control in `controls` `step` places from `current` (skipping hidden ones), as a
    // screen point in `window`'s frame.
    template <class T>
    bool stepTo(cUI_Window2* window, T* controls, int count, int current, int step, float& outX, float& outY)
    {
        for (int i = current + step; i >= 0 && i < count; i += step)
        {
            T* control = &controls[i];
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

cUI_Diary* UiNav::logBook()
{
    cUI_Manager* manager = ::manager();
    if (!manager)
    {
        return nullptr;
    }
    const uint32_t flags = manager->flags;
    cUI_Diary* diary = manager->questbook();
    return !(flags & cUI_Manager::inMenus) && (flags & cUI_Manager::inGame) && visible(diary) ? diary : nullptr;
}

bool UiNav::bookTab(int step, float& outX, float& outY)
{
    cUI_Diary* diary = logBook();
    if (!diary)
    {
        return false;
    }
    int current = diary->currentTab;
    if (current >= cUI_Diary::tabCount)
    {
        current = step > 0 ? -1 : cUI_Diary::tabCount;
    }
    return stepTo(diary, diary->tabs, cUI_Diary::tabCount, current, step, outX, outY);
}

bool UiNav::bookSection(int step, float& outX, float& outY)
{
    cUI_Diary* diary;
    cUI_Book* book = currentBook(diary);
    if (!book)
    {
        return false;
    }
    return stepTo(diary, book->tabs, cUI_Book::tabCount, book->selectedTab, step, outX, outY);
}

bool UiNav::bookPage(bool right, int step, float& outX, float& outY)
{
    cUI_Diary* diary;
    cUI_Book* book = currentBook(diary);
    if (!book)
    {
        return false;
    }
    cUI_Button2& button = right ? (step > 0 ? book->rightNext : book->rightPrevious)
                                : (step > 0 ? book->leftNext : book->leftPrevious);
    float x, y;
    if (!visible(&button) || !center(&button, x, y))
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
    cUI_Diary* diary;
    cUI_Book* book = currentBook(diary);
    if (!book)
    {
        return false;
    }
    // The selected entry of the list's page.
    const uint16_t tab = book->selectedTab;
    if (tab >= cUI_Book::tabCount)
    {
        return false;
    }
    const Vector<cUI_Book::Page>& pages = book->pages[tab];
    const uint16_t page = book->leftPage;
    if (page >= pages.size())
    {
        return false;
    }
    const int selected = pages[page].selected;
    // Where the entries are: the game's own hit test down the left page, at a few points across it.
    const UiRect rect = book->absoluteRect();
    const int left = rect.x + cUI_Book::listX, top = rect.y + cUI_Book::listY;
    struct Entry
    {
        int index, top, bottom;
    };
    std::vector<Entry> entries;
    int column = 0;
    for (int x : {left + 0x20, left + cUI_Book::listWidth / 2, left + cUI_Book::listWidth - 0x20})
    {
        for (int y = top; y < top + cUI_Book::listHeight; y += 3)
        {
            uint16_t index = 0;
            if (!book->lineAt(x, y, index))
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
    cUI_Manager* manager = ::manager();
    cUI_Inventory3* inventory = manager ? manager->inventory() : nullptr;
    cUI_Inventory3::Tabs* tabs = visible(inventory) ? inventory->tabs : nullptr;
    cUI_TabControl* control = tabs ? tabs->tabControl : nullptr;
    if (!control || className(control) != "cUI_TabControl")
    {
        return false;
    }
    const Vector<cUI_TabControl::Page>& pages = control->pages;
    if (pages.size() > 16)
    {
        return false;
    }
    const int count = static_cast<int>(pages.size());
    for (int i = control->activePage + step; i >= 0 && i < count; i += step)
    {
        cUI_Control2* button = pages[i].button;
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

cUI_Savegame* UiNav::savegames()
{
    cUI_Manager* manager = ::manager();
    if (!manager)
    {
        return nullptr;
    }
    const bool menus = manager->flags & cUI_Manager::inMenus;
    cUI_Window2* window = menus ? manager->menus[cUI_Manager::savegameMenu] : manager->savegame();
    auto* savegame = static_cast<cUI_Savegame*>(window);
    return visible(window) && kindOf(window) == Savegames && listMode(savegame) ? savegame : nullptr;
}

int UiNav::savegameRow(float x, float y)
{
    cUI_Savegame* window = savegames();
    if (!window)
    {
        return -1;
    }
    const UiCanvas::Frame frame = frameOf(window);
    for (int row = 0; row < cUI_Savegame::rowCount; ++row)
    {
        const Rect r = rowRect(window, row, frame);
        if (rowShows(window, row) && x >= r.left && x < r.right && y >= r.top && y < r.bottom)
        {
            return row;
        }
    }
    return -1;
}

bool UiNav::savegameRowPoint(int row, float& outX, float& outY)
{
    cUI_Savegame* window = savegames();
    if (!window || row < 0 || row >= cUI_Savegame::rowCount)
    {
        return false;
    }
    const Rect r = rowRect(window, row, frameOf(window));
    outX = std::round((r.left + r.right) * 0.5f);
    outY = std::round((r.top + r.bottom) * 0.5f);
    return true;
}

bool UiNav::savegameStep(float x, float y, int step, int& row, int& scrollTo)
{
    cUI_Savegame* window = savegames();
    const int current = savegameRow(x, y);
    if (!window || current < 0)
    {
        return false;
    }
    row = -1;
    scrollTo = -1;
    const int target = current + step;
    const uint32_t first = firstListed(window);
    if (target == cUI_Savegame::rowCount && rowShows(window, current))
    {
        // Below the last row: the list scrolls a line if there is more.
        if (first + cUI_Savegame::rowCount - 1 < listed(window) + (listMode(window) == cUI_Savegame::saving ? 1 : 0))
        {
            row = current;
            scrollTo = static_cast<int>(first + 1);
        }
    }
    else if (target == 0 && first > 0)
    {
        row = current;  // above row 1: the list scrolls back before the quicksave row
        scrollTo = static_cast<int>(first - 1);
    }
    else if (target >= 0 && target < cUI_Savegame::rowCount && rowShows(window, target))
    {
        row = target;
    }
    return true;
}

void UiNav::selectSavegame(int scrollTo, int row)
{
    cUI_Savegame* window = savegames();
    if (!window)
    {
        return;
    }
    if (scrollTo >= 0)
    {
        scrollList(window, static_cast<uint32_t>(scrollTo));
    }
    if (rowShows(window, row))
    {
        window->selectRow(static_cast<uint16_t>(row));
    }
}

namespace
{
    // When a savegame was saved, as FILETIME ticks in local time: its header's time, else its file's; 0 if the file
    // can't be opened.
    uint64_t savedAt(const char* name)
    {
        const std::string path = ".\\SAVE\\" + std::string(name, strnlen(name, 0x100));
        HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, 0, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return 0;
        }
        int32_t header[cUI_Savegame::headerSize / 4] = {};
        DWORD read = 0;
        const bool complete = ReadFile(file, header, sizeof(header), &read, nullptr) && read == sizeof(header);
        FILETIME written{};
        GetFileTime(file, nullptr, nullptr, &written);
        CloseHandle(file);
        const int32_t* t = header + cUI_Savegame::headerYear / 4;   // year, month, day, day of week, hour, ...
        const SYSTEMTIME saved = {static_cast<WORD>(t[0]), static_cast<WORD>(t[1]), static_cast<WORD>(t[3]),
            static_cast<WORD>(t[2]), static_cast<WORD>(t[4]), static_cast<WORD>(t[5]), static_cast<WORD>(t[6]),
            static_cast<WORD>(t[7])};
        FILETIME time{};
        if (!complete || t[0] <= 0 || !SystemTimeToFileTime(&saved, &time))
        {
            FileTimeToLocalFileTime(&written, &time);
        }
        return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    }
}

int UiNav::selectNewestSavegame()
{
    cUI_Savegame* window = savegames();
    if (!window || listMode(window) != cUI_Savegame::loading)
    {
        return -1;
    }
    // -1: the quicksave.
    int64_t newest = -2;
    uint64_t newestTime = 0;
    if (const char* quicksave = window->quicksave.name; *quicksave)
    {
        newest = -1;
        newestTime = savedAt(quicksave);
    }
    const uint32_t count = listed(window);
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint64_t time = savedAt(window->entries[i].name);
        if (newest == -2 || time > newestTime)
        {
            newest = i;
            newestTime = time;
        }
    }
    if (newest == -2)
    {
        return -1;
    }
    if (const uint32_t first = firstListed(window); newest >= 0 && (newest < first || newest >= first + cUI_Savegame::rowCount - 1))
    {
        scrollList(window, static_cast<uint32_t>(newest));   // the window keeps the list's end at row 3
    }
    const int row = newest < 0 ? 0 : static_cast<int>(newest - firstListed(window) + 1);
    selectSavegame(-1, row);
    return row;
}
