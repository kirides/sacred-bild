#include "game/ui_nav.h"
#include "game/sacred_addr.h"
#include "game/ui_anchor.h"
#include "game/ui_canvas.h"
#include "mem.h"

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

    // thiscall (int32 out[3]) on a control, called as fastcall with an unused EDX.

    enum Kind : uint8_t
    {
        Other,
        Window,     // has children
        Control,    // something to point the cursor at
        Button,     // ... a button
        GameMenu,   // the game menu: its entries are in a vector of their own (Sacred::EscMenu)
        MainMenu,   // the start menu: a vector per screen (Sacred::MainMenu)
        Savegames,  // the savegame window: a window whose list rows are hit rects (Sacred::Savegame)
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

    using Mem::member;

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

    bool visible(void* control)
    {
        return control && (member<uint32_t>(control, UiControl::flags) & 1);
    }

    struct Point
    {
        float x, y;
        bool button;
        UiNav::Rect rect;   // the control's, screen pixels
        void* control = nullptr;
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
    bool layoutRect(void* control, float& left, float& top, float& width, float& height)
    {
        int32_t rect[3] = {};
        Addr::cUI_Control2_getAbsoluteRect(control, rect);
        const int w = static_cast<int16_t>(rect[2] & 0xFFFF), h = static_cast<int16_t>(rect[2] >> 16);
        if (w <= 0 || h <= 0 || w > 1024 || h > 768)
        {
            return false;
        }
        left = static_cast<float>(rect[0]);
        top = static_cast<float>(rect[1]);
        width = static_cast<float>(w);
        height = static_cast<float>(h);
        return true;
    }

    bool center(void* control, float& x, float& y)
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

    void addControl(void* control, bool button, const UiCanvas::Frame& frame, std::vector<Point>& out)
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

    // ---- The savegame window's list (Sacred::Savegame) ----


    // Saving or loading, also while a message box asks about it; 0 if neither.
    uint32_t listMode(void* window)
    {
        uint32_t mode = member<uint32_t>(window, Savegame::mode);
        if (mode > Savegame::loading)
        {
            mode = member<uint32_t>(window, Savegame::askedMode);
        }
        return mode == Savegame::saving || mode == Savegame::loading ? mode : 0;
    }

    uint32_t listed(void* window)
    {
        const auto begin = member<uintptr_t>(window, Savegame::entriesBegin);
        const auto end = member<uintptr_t>(window, Savegame::entriesEnd);
        return begin && end > begin ? static_cast<uint32_t>((end - begin) / Savegame::entrySize) : 0;
    }

    // The first listed savegame, row 1's, as the window works it out (ENG 0071C2B0).
    uint32_t firstListed(void* window)
    {
        void* slider = static_cast<uint8_t*>(window) + Savegame::slider;
        const uint32_t value = Addr::cUI_Slider_getValue(slider) & 0xFFFF;
        const uint32_t last = member<uint32_t>(slider, Slider::count) - (listMode(window) == Savegame::saving ? 2 : 3);
        return last <= value ? last : value;
    }

    // The row shows a savegame, or the new one (the row past the end) when saving.
    bool rowShows(void* window, int row)
    {
        if (row == 0)
        {
            return member<char>(window, Savegame::quicksave + Savegame::entryName) != '\0';
        }
        const uint32_t index = firstListed(window) + row - 1;
        return row > 0 && row < Savegame::rowCount &&
            index < listed(window) + (listMode(window) == Savegame::saving ? 1 : 0);
    }

    // Sets the first listed savegame (the slider; the window renders the rows anew when it changes).
    void scrollList(void* window, uint32_t first)
    {
        Addr::cUI_Slider_setValue(static_cast<uint8_t*>(window) + Savegame::slider, first);
    }

    UiNav::Rect rowRect(void* window, int row, const UiCanvas::Frame& frame)
    {
        const uintptr_t r = Savegame::rows + row * Savegame::rowSize;
        return screenRect(static_cast<float>(member<int32_t>(window, r)), static_cast<float>(member<int32_t>(window, r + 4)),
            member<int16_t>(window, r + 8), member<int16_t>(window, r + 10), frame);
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
        if (kind == Savegames && listMode(control))
        {
            for (int row = 0; row < Savegame::rowCount; ++row)
            {
                if (rowShows(control, row))
                {
                    const UiNav::Rect r = rowRect(control, row, frame);
                    out.push_back({(r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f, false, r});
                }
            }
            for (uintptr_t offset : Savegame::buttons)
            {
                void* button = static_cast<uint8_t*>(control) + offset;
                if (visible(button) && kindOf(button) == Button)
                {
                    addControl(button, true, frame, out);
                }
            }
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
        if (kind == Window || kind == Savegames)
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
        if (depth > 3 || out.size() > 300 || (kindOf(control) != Window && kindOf(control) != Savegames))
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

namespace
{
    using TextWidthFn = uint16_t(__fastcall*)(void* font, void* edx, const wchar_t* text);

    // A menu entry whose text is drawn centered in its rect (Sacred::StaticText): the text's width in the 1024x768
    // layout, as its font measures it; 0 for other controls or a font that does not measure.
    float centeredTextWidth(void* control)
    {
        const std::string name = UiNav::className(control);
        const bool fx = name == "cUI_StaticText64FX";
        if ((!fx && name != "cUI_StaticText64") || !(member<uint32_t>(control, UiControl::flags) & StaticText::centered))
        {
            return 0.0f;
        }
        const wchar_t* begin = nullptr;
        const wchar_t* end = nullptr;
        if (const uint32_t id = member<uint32_t>(control, fx ? StaticText::fxTextId : StaticText::textId))
        {
            void* texts = Addr::textResources_instance();
            const wchar_t* const* text = texts ? Addr::textResources_get(texts, id) : nullptr;
            if (text)
            {
                begin = text[0];
                end = text[1];
            }
        }
        else if (!fx)
        {
            begin = member<const wchar_t*>(control, StaticText::text);
            end = member<const wchar_t*>(control, StaticText::textEnd);
        }
        void* fonts = *reinterpret_cast<void**>(Addr::g_pFontManager);
        if (!begin || end <= begin || end - begin > 256 || !fonts)
        {
            return 0.0f;
        }
        void** first = member<void**>(fonts, 0);
        void** last = member<void**>(fonts, 4);
        const uint16_t index = member<uint16_t>(control, fx ? StaticText::fxFont : StaticText::font);
        void* font = first && last > first && index < last - first ? first[index] : nullptr;
        if (!font)
        {
            return 0.0f;
        }
        const std::wstring text(begin, end);
        const auto measure = reinterpret_cast<TextWidthFn>((*static_cast<uintptr_t**>(font))[Font::textWidthSlot / 4]);
        return measure(font, nullptr, text.c_str());
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

bool UiNav::controlRect(void* control, void* window, Rect& out)
{
    float left, top, w, h;
    if (!visible(control) || !layoutRect(control, left, top, w, h))
    {
        return false;
    }
    out = screenRect(left, top, w, h, frameOf(window));
    return true;
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
    Addr::cUI_Control2_getAbsoluteRect(popup, rect);
    UiCanvas::Frame frame;
    if (!UiAnchor::frame(popup, frame))
    {
        frame = {};
    }
    out = screenRect(static_cast<float>(rect[0] + member<int32_t>(popup, answer + Popup::answerX)),
        static_cast<float>(rect[1] + member<int32_t>(popup, answer + Popup::answerY)), w, h, frame);
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
    Addr::cUI_Control2_getAbsoluteRect(book, rect);
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
            if (!Addr::cUI_Book_lineAt(book, x, y, &index))
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

void* UiNav::savegames()
{
    void* manager = ::manager();
    if (!manager)
    {
        return nullptr;
    }
    const bool menus = member<uint32_t>(manager, UiManager::flags) & 0x01;
    void* window = member<void*>(manager, menus ? Savegame::inMenus : UiManager::savegame);
    return visible(window) && kindOf(window) == Savegames && listMode(window) ? window : nullptr;
}

int UiNav::savegameRow(float x, float y)
{
    void* window = savegames();
    if (!window)
    {
        return -1;
    }
    const UiCanvas::Frame frame = frameOf(window);
    for (int row = 0; row < Savegame::rowCount; ++row)
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
    void* window = savegames();
    if (!window || row < 0 || row >= Savegame::rowCount)
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
    void* window = savegames();
    const int current = savegameRow(x, y);
    if (!window || current < 0)
    {
        return false;
    }
    row = -1;
    scrollTo = -1;
    const int target = current + step;
    const uint32_t first = firstListed(window);
    if (target == Savegame::rowCount && rowShows(window, current))
    {
        // Below the last row: the list scrolls a line if there is more.
        if (first + Savegame::rowCount - 1 < listed(window) + (listMode(window) == Savegame::saving ? 1 : 0))
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
    else if (target >= 0 && target < Savegame::rowCount && rowShows(window, target))
    {
        row = target;
    }
    return true;
}

void UiNav::selectSavegame(int scrollTo, int row)
{
    void* window = savegames();
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
        Addr::cUI_Savegame_selectRow(window, static_cast<uint16_t>(row));
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
        int32_t header[Savegame::headerSize / 4] = {};
        DWORD read = 0;
        const bool complete = ReadFile(file, header, sizeof(header), &read, nullptr) && read == sizeof(header);
        FILETIME written{};
        GetFileTime(file, nullptr, nullptr, &written);
        CloseHandle(file);
        const int32_t* t = header + Savegame::headerYear / 4;   // year, month, day, day of week, hour, ...
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
    void* window = savegames();
    if (!window || listMode(window) != Savegame::loading)
    {
        return -1;
    }
    // -1: the quicksave.
    int64_t newest = -2;
    uint64_t newestTime = 0;
    if (const char* quicksave = &member<char>(window, Savegame::quicksave + Savegame::entryName); *quicksave)
    {
        newest = -1;
        newestTime = savedAt(quicksave);
    }
    const auto* entries = member<const uint8_t*>(window, Savegame::entriesBegin);
    const uint32_t count = listed(window);
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint64_t time = savedAt(reinterpret_cast<const char*>(entries + i * Savegame::entrySize + Savegame::entryName));
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
    if (const uint32_t first = firstListed(window); newest >= 0 && (newest < first || newest >= first + Savegame::rowCount - 1))
    {
        scrollList(window, static_cast<uint32_t>(newest));   // the window keeps the list's end at row 3
    }
    const int row = newest < 0 ? 0 : static_cast<int>(newest - firstListed(window) + 1);
    selectSavegame(-1, row);
    return row;
}
