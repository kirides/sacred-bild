#include "input.h"
#include "ui.h"
#include "game/gear_addr.h"

#include <atomic>
#include <map>
#include <mutex>

namespace
{
    std::mutex g_lock;
    std::vector<Input::Rule> g_rules;
    std::atomic<bool> g_anyPoll{false};
    std::vector<Input::Event> g_events;
    constexpr size_t kEventsMax = 256;

    // Inputs whose press a rule took: their release goes the same way (engine thread only).
    struct Held
    {
        int to;             // 0: swallowed
        std::string id;
    };
    std::map<int, Held> g_held;
    std::atomic<bool> g_hiding{false};
    // The game acts on the characters TranslateMessage makes of the key messages it reads, and TranslateMessage makes them
    // from the keys the player holds: Ctrl+F turned into 6 makes none (Ctrl+digit has no character), Shift+1 turned into
    // 6 makes '&'. So the characters TranslateMessage makes of a rewritten press are dropped and the layer posts the
    // character the game's key makes by itself (marked, so it is passed on as it is).
    int g_dropChars = 0;
    ULONGLONG g_dropUntil = 0;
    constexpr LPARAM kOwnChar = 0x10000000;

    void postChar(HWND window, int vk)
    {
        BYTE state[256] = {};
        wchar_t buffer[8] = {};
        const UINT scan = MapVirtualKeyA(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
        const int n = ToUnicodeEx(static_cast<UINT>(vk), scan, state, buffer, 8, 4, GetKeyboardLayout(0));
        if (n == 1 && window)
        {
            PostMessageW(window, WM_CHAR, buffer[0], kOwnChar | 1 | (static_cast<LPARAM>(scan) << 16));
        }
        g_dropChars = 1;
        g_dropUntil = GetTickCount64() + 100;
    }

    // Offsets in the UI manager (SacredBild's src/sacred/ui.h).
    constexpr uint32_t kFlagsOff = 0x08, kWindowsOff = 0x80, kDialogOff = 0x144;
    constexpr uint32_t kInMenus = 0x01, kInGame = 0x04, kWindowsCreated = 0x08, kCinematic = 0x10, kInputBlocked = 0x80;
    constexpr int kConsole = 9, kSavegame = 11, kEscapeMenu = 12, kOptions = 15, kCharacter = 20;
    constexpr uint32_t kVisible = 0x01, kControlFlagsOff = 0x10;

    const uint8_t* manager()
    {
        return Gear::Addr::g_pUiManager ? *Gear::Addr::g_pUiManager.get() : nullptr;
    }

    uint32_t u32(const uint8_t* p, uint32_t off)
    {
        return *reinterpret_cast<const uint32_t*>(p + off);
    }

    const uint8_t* ptrAt(const uint8_t* p, uint32_t off)
    {
        return reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(u32(p, off)));
    }

    bool visible(const uint8_t* control)
    {
        return control && (u32(control, kControlFlagsOff) & kVisible);
    }

    bool isModifier(int vk)
    {
        return (vk >= VK_SHIFT && vk <= VK_MENU) || (vk >= VK_LSHIFT && vk <= VK_RMENU);
    }

    // The input a message stands for: the key, or a mouse button; 0 if none.
    int inputOf(const MSG& m, bool& down, bool& repeat, bool& key)
    {
        repeat = false;
        key = false;
        switch (m.message)
        {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            down = true;
            key = true;
            repeat = (m.lParam & (1 << 30)) != 0;
            return static_cast<int>(m.wParam);
        case WM_KEYUP:
        case WM_SYSKEYUP:
            down = false;
            key = true;
            return static_cast<int>(m.wParam);
        case WM_MBUTTONDOWN:
            down = true;
            return VK_MBUTTON;
        case WM_MBUTTONUP:
            down = false;
            return VK_MBUTTON;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
            down = m.message == WM_XBUTTONDOWN;
            return HIWORD(m.wParam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
        default:
            return 0;
        }
    }

    const Input::Rule* find(const std::vector<Input::Rule>& rules, int vk)
    {
        const bool ctrl = Ui::keyDown(VK_CONTROL), shift = Ui::keyDown(VK_SHIFT), alt = Ui::keyDown(VK_MENU);
        const Input::Rule* any = nullptr;
        for (const Input::Rule& r : rules)
        {
            if (r.from != vk)
            {
                continue;
            }
            if (r.anyModifiers)
            {
                if (!any)
                {
                    any = &r;
                }
            }
            else if (r.ctrl == ctrl && r.shift == shift && r.alt == alt)
            {
                return &r;
            }
        }
        return any;
    }

    void note(const std::string& id, bool down, bool repeat)
    {
        if (id.empty())
        {
            return;
        }
        std::lock_guard guard(g_lock);
        if (g_events.size() < kEventsMax)
        {
            g_events.push_back({id, down, repeat});
        }
    }

    void asKey(MSG& m, int vk, bool down, bool repeat)
    {
        const UINT scan = MapVirtualKeyA(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
        m.message = down ? WM_KEYDOWN : WM_KEYUP;
        m.wParam = static_cast<WPARAM>(vk);
        m.lParam = 1 | (static_cast<LPARAM>(scan) << 16) | (repeat ? (1 << 30) : 0) |
                   (down ? 0 : static_cast<LPARAM>(0xC0000000));
    }
}

void Input::setRules(std::vector<Rule> rules)
{
    std::lock_guard guard(g_lock);
    g_rules = std::move(rules);
    bool any = false;
    for (const Rule& r : g_rules)
    {
        any = any || r.poll;
    }
    g_anyPoll.store(any);
}

std::vector<Input::Event> Input::takeEvents()
{
    std::lock_guard guard(g_lock);
    std::vector<Event> out;
    out.swap(g_events);
    return out;
}

bool Input::typing()
{
    // The game routes what is typed into the chat line by whether the console window shows; its edit control keeps the
    // keyboard focus flag after the line closed (and takes it in single player where nobody can chat), so the focus
    // says nothing.
    const uint8_t* ui = manager();
    if (!ui || !(u32(ui, kFlagsOff) & kWindowsCreated))
    {
        return false;
    }
    return visible(ptrAt(ui, kWindowsOff + 4 * kConsole));
}

bool Input::context()
{
    const uint8_t* ui = manager();
    if (!ui)
    {
        return false;
    }
    const uint32_t flags = u32(ui, kFlagsOff);
    if (!(flags & kInGame) || (flags & (kInMenus | kCinematic | kInputBlocked)) || !(flags & kWindowsCreated))
    {
        return false;
    }
    if (visible(ptrAt(ui, kDialogOff)))
    {
        return false;
    }
    // The escape menu, and the screens with text fields (savegame names, options, the character export).
    for (const int window : {kEscapeMenu, kSavegame, kOptions, kCharacter})
    {
        if (visible(ptrAt(ui, kWindowsOff + 4 * window)))
        {
            return false;
        }
    }
    return !typing();
}

bool Input::hidingModifiers()
{
    return g_hiding.load(std::memory_order_relaxed);
}

void Input::filter(MSG& m, bool consume)
{
    g_hiding.store(false, std::memory_order_relaxed);
    if (m.message == WM_CHAR || m.message == WM_SYSCHAR || m.message == WM_DEADCHAR || m.message == WM_SYSDEADCHAR)
    {
        if (m.lParam & kOwnChar)
        {
            m.lParam &= ~kOwnChar;
        }
        else if (g_dropChars > 0 && GetTickCount64() < g_dropUntil)
        {
            if (consume)
            {
                --g_dropChars;
            }
            m.message = WM_NULL;
        }
        return;
    }
    bool down = false, repeat = false, key = false;
    const int vk = inputOf(m, down, repeat, key);
    if (!vk)
    {
        return;
    }
    if (!down)
    {
        // The release of an input a rule took.
        const auto it = g_held.find(vk);
        if (it == g_held.end())
        {
            return;
        }
        const Held held = it->second;
        if (consume)
        {
            g_held.erase(it);
            note(held.id, false, false);
        }
        if (held.to)
        {
            asKey(m, held.to, false, false);
            g_hiding.store(true, std::memory_order_relaxed);
        }
        else
        {
            m.message = WM_NULL;
        }
        return;
    }
    if (isModifier(vk) || vk == VK_ESCAPE)
    {
        return;
    }
    if (const auto held = g_held.find(vk); held != g_held.end())
    {
        // Auto repeat of an input a rule took: the same way, whatever is held now.
        if (consume)
        {
            note(held->second.id, true, true);
        }
        if (held->second.to)
        {
            if (consume)
            {
                postChar(m.hwnd, held->second.to);
            }
            asKey(m, held->second.to, true, true);
            g_hiding.store(true, std::memory_order_relaxed);
        }
        else
        {
            m.message = WM_NULL;
        }
        return;
    }
    std::vector<Rule> rules;
    {
        std::lock_guard guard(g_lock);
        if (g_rules.empty())
        {
            return;
        }
        rules = g_rules;
    }
    if (!context())
    {
        return;
    }
    const Rule* rule = find(rules, vk);
    if (!rule)
    {
        return;
    }
    const int to = rule->poll ? 0 : rule->to;
    if (consume)
    {
        g_held[vk] = {to, rule->id};
        note(rule->id, true, repeat);
        if (to)
        {
            postChar(m.hwnd, to);
        }
    }
    if (to)
    {
        asKey(m, to, true, repeat);
        g_hiding.store(true, std::memory_order_relaxed);
    }
    else
    {
        m.message = WM_NULL;
    }
}

bool Input::polled(int vk, bool real)
{
    if (!g_anyPoll.load(std::memory_order_relaxed))
    {
        return real;
    }
    std::vector<Rule> rules;
    {
        std::lock_guard guard(g_lock);
        rules = g_rules;
    }
    bool mapped = false, released = false;
    for (const Rule& r : rules)
    {
        if (!r.poll)
        {
            continue;
        }
        if (r.to == vk)
        {
            mapped = true;
        }
        else if (!r.to && r.from == vk)
        {
            released = true;
        }
    }
    if (!mapped && !released)
    {
        return real;
    }
    if (!context())
    {
        return real;
    }
    if (mapped)
    {
        const bool ctrl = Ui::keyDown(VK_CONTROL), shift = Ui::keyDown(VK_SHIFT), alt = Ui::keyDown(VK_MENU);
        for (const Rule& r : rules)
        {
            if (!r.poll || r.to != vk || !Ui::keyDown(r.from))
            {
                continue;
            }
            // The modifiers named must be the ones held, not counting the key itself when it is a modifier.
            const bool c = r.from == VK_CONTROL ? r.ctrl : ctrl, s = r.from == VK_SHIFT ? r.shift : shift,
                       a = r.from == VK_MENU ? r.alt : alt;
            if (r.anyModifiers || (r.ctrl == c && r.shift == s && r.alt == a))
            {
                return true;
            }
        }
    }
    return released ? false : real && !mapped;
}
