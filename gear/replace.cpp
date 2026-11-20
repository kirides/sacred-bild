#include "replace.h"
#include "game/gear_addr.h"
#include "log.h"
#include "patch.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

// See replace.h. The manager's layout (SacredBild's src/sacred/ui.h, docs/UI_SYSTEM.md): flags at +0x08 (0x01 menus,
// 0x04 in game, 0x08 the in-game windows exist), the in-game windows at +0x80 (23 slots), the menus' screens at
// +0x130 (5). A control's flags at +0x10 (bit 0 visible), its vtable at +0: slot 4 receiveEvent (thiscall (control,
// event) -> bool, ret 4), slot 5 render (thiscall (control, device), ret 4). An event's vtable slot 1 returns its
// kind: 1-4 mouse move / down / up and keys, 5 the named UI events, 0xC IME.

namespace
{
    constexpr uintptr_t kFlags = 0x08, kWindows = 0x80, kMenus = 0x130;
    constexpr uint32_t kMenusMode = 0x01, kWindowsExist = 0x08;
    constexpr int kWindowSlots = 23, kMenuCount = 5;
    constexpr int kSavegameSlot = 11, kLoadgameMenu = 3;
    constexpr ULONGLONG kGraceMs = 1500, kBeatMs = 500, kClaimsMs = 1000;

    const char* const kWindowNames[kWindowSlots] = {
        "UI_WND_TASKBAR", "UI_WND_INVENTORY", "UI_WND_EQUIPMENT", "UI_WND_BLACKSMITH", "UI_WND_MERCHANT",
        "UI_WND_MEGAMAP", "UI_WND_OVERVIEWMAP", "UI_WND_MERC", "UI_WND_STATS_CHAR", "UI_WND_CONSOLE",
        "UI_WND_QUESTBOOK", "UI_WND_SAVEGAME", "UI_WND_ESCMENU", "UI_WND_PURCHASE", "UI_WND_MASTER",
        "UI_WND_OPTIONS", "UI_WND_CHEST", "UI_WND_HORSE", "UI_WND_NETINFO", "UI_WND_NETPORTRAIT", "UI_CHARACTER",
        "UI_WND_CUBE", "UI_WND_TRADE"};
    const char* const kMenuNames[kMenuCount] = {"UI_MENU_MAIN", "UI_MENU_CHARACTER", "UI_MENU_OPTIONS",
                                                "UI_MENU_LOADGAME", "UI_MENU_NETWORK"};

    template <class T>
    bool read(uintptr_t address, T& out)
    {
        __try
        {
            std::memcpy(&out, reinterpret_cast<const void*>(address), sizeof(T));
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    template <class T>
    T readOr(uintptr_t address, T fallback)
    {
        T v{};
        return read(address, v) ? v : fallback;
    }

    uintptr_t manager()
    {
        return Gear::Addr::g_pUiManager ? reinterpret_cast<uintptr_t>(*Gear::Addr::g_pUiManager) : 0;
    }

    // A replaceable window: an in-game slot (menu = false) or a menu screen.
    struct Target
    {
        bool menu = false;
        int index = -1;
    };

    bool target(const std::string& name, Target& out)
    {
        for (int i = 0; i < kWindowSlots; ++i)
        {
            if (name == kWindowNames[i])
            {
                out = {false, i};
                return true;
            }
        }
        for (int i = 0; i < kMenuCount; ++i)
        {
            if (name == kMenuNames[i])
            {
                out = {true, i};
                return true;
            }
        }
        return false;
    }

    uintptr_t objectOf(const Target& t)
    {
        const uintptr_t m = manager();
        if (!m || t.index < 0)
        {
            return 0;
        }
        const uint32_t flags = readOr<uint32_t>(m + kFlags, 0);
        if (t.menu)
        {
            return flags & kMenusMode ? readOr<uintptr_t>(m + kMenus + t.index * 4, 0) : 0;
        }
        return flags & kWindowsExist ? readOr<uintptr_t>(m + kWindows + t.index * 4, 0) : 0;
    }

    bool visible(uintptr_t control) { return control && (readOr<uint32_t>(control + 0x10, 0) & 1); }

    // ---- The vtable thunks ----

    using ReceiveFn = uint32_t(__fastcall*)(void* self, void* edx, void* event);
    using RenderFn = uint32_t(__fastcall*)(void* self, void* edx, void* device);

    struct Patched
    {
        uintptr_t vtable = 0;
        ReceiveFn receive = nullptr;
        RenderFn render = nullptr;
    };
    Patched g_patched[8];
    std::atomic<size_t> g_patchedCount{0};     // grows only; an entry is written before the count (under g_lock)

    // One claimed window (g_lock).
    struct Claim
    {
        std::string name;
        Target target;
        uintptr_t object = 0;       // as of the last tick
        bool open = false;
        ULONGLONG openedAt = 0;     // when the tick saw it open first
        ULONGLONG beatAt = 0;       // the mod's last beat while open
        bool lapsed = false;        // no beat: the game's window shows until it closes
    };
    std::mutex g_lock;
    std::vector<Claim> g_claims;
    ULONGLONG g_claimsAt = 0;

    const Patched* patchedFor(uintptr_t vtable)
    {
        const size_t n = g_patchedCount.load(std::memory_order_acquire);
        for (size_t i = 0; i < n; ++i)
        {
            if (g_patched[i].vtable == vtable)
            {
                return &g_patched[i];
            }
        }
        return nullptr;
    }

    // Whether `self` is hidden now: claimed (claims fresh), open, and either just opened or beaten recently.
    bool hidden(uintptr_t self)
    {
        const ULONGLONG now = GetTickCount64();
        std::scoped_lock lock(g_lock);
        if (now - g_claimsAt > kClaimsMs)
        {
            return false;
        }
        for (const Claim& c : g_claims)
        {
            if (c.object != self || c.lapsed)
            {
                continue;
            }
            // Opened since the last tick (not seen open yet), within the grace, or beaten lately.
            if (!c.open || now - c.openedAt < kGraceMs || now - c.beatAt < kBeatMs)
            {
                return visible(self);
            }
        }
        return false;
    }

    int eventKind(void* event)
    {
        __try
        {
            using KindFn = int(__thiscall*)(void*);
            return (*reinterpret_cast<KindFn* const*>(event))[1](event);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    uint32_t __fastcall thunkReceive(void* self, void* edx, void* event)
    {
        const uintptr_t vt = readOr<uintptr_t>(reinterpret_cast<uintptr_t>(self), 0);
        const Patched* p = patchedFor(vt);
        if (hidden(reinterpret_cast<uintptr_t>(self)))
        {
            const int kind = event ? eventKind(event) : -1;
            if (kind >= 1 && kind <= 4)
            {
                return 1;   // the mouse and the keys are the replacement's; handled, so nothing else gets them
            }
        }
        return p && p->receive ? p->receive(self, edx, event) : 0;
    }

    uint32_t __fastcall thunkRender(void* self, void* edx, void* device)
    {
        const uintptr_t vt = readOr<uintptr_t>(reinterpret_cast<uintptr_t>(self), 0);
        const Patched* p = patchedFor(vt);
        if (hidden(reinterpret_cast<uintptr_t>(self)))
        {
            return 0;
        }
        return p && p->render ? p->render(self, edx, device) : 0;
    }

    // SacredBild wraps the windows' vtables itself (src/game/ui_windows.cpp) and only takes slots that point into the
    // exe; so with SacredBild loaded, Gear patches a vtable only after SacredBild did (its slots then point into
    // ddraw.dll) and chains to SacredBild's thunks. Without it, the slots are the exe's.
    bool exeCode(uintptr_t p)
    {
        static const auto range = [] {
            auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
            auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            return std::pair<uintptr_t, uintptr_t>(reinterpret_cast<uintptr_t>(base),
                                                   reinterpret_cast<uintptr_t>(base) + nt->OptionalHeader.SizeOfImage);
        }();
        return p >= range.first && p < range.second;
    }

    bool bildWrapsWindows()
    {
        static const bool bild = [] {
            HMODULE m = GetModuleHandleW(L"ddraw.dll");
            return m && GetProcAddress(m, "SacredBild_UiPushFrame") != nullptr;
        }();
        return bild;
    }

    // Patches the object's class (g_lock held). False if it must wait (SacredBild has not wrapped it yet).
    bool ensurePatched(uintptr_t object)
    {
        const uintptr_t vt = readOr<uintptr_t>(object, 0);
        if (!vt)
        {
            return false;
        }
        if (patchedFor(vt))
        {
            return true;
        }
        const size_t n = g_patchedCount.load(std::memory_order_relaxed);
        if (n == std::size(g_patched))
        {
            return false;
        }
        const uintptr_t receive = readOr<uintptr_t>(vt + 4 * 4, 0), render = readOr<uintptr_t>(vt + 5 * 4, 0);
        if (!receive || !render)
        {
            return false;
        }
        if (bildWrapsWindows() && (exeCode(receive) || exeCode(render)))
        {
            return false;   // SacredBild wraps it on its next UI frame
        }
        Patched& p = g_patched[n];
        p.vtable = vt;
        p.receive = reinterpret_cast<ReceiveFn>(receive);
        p.render = reinterpret_cast<RenderFn>(render);
        g_patchedCount.store(n + 1, std::memory_order_release);     // the originals are known before a slot leads to a thunk
        const bool ok = Patch::value(vt + 4 * 4, &thunkReceive) && Patch::value(vt + 5 * 4, &thunkRender);
        LOG("Replace: window class (vtable {:08X}) {}", vt, ok ? "can be replaced" : "could not be patched");
        return ok;
    }

    Claim* find(const std::string& name)
    {
        for (Claim& c : g_claims)
        {
            if (c.name == name)
            {
                return &c;
            }
        }
        return nullptr;
    }
}

bool Replace::known(const std::string& name)
{
    Target t;
    return target(name, t);
}

uintptr_t Replace::object(const std::string& name)
{
    Target t;
    return target(name, t) ? objectOf(t) : 0;
}

bool Replace::open(const std::string& name)
{
    return visible(object(name));
}

void Replace::setClaims(const std::vector<std::string>& names)
{
    const ULONGLONG now = GetTickCount64();
    std::scoped_lock lock(g_lock);
    std::vector<Claim> next;
    for (const std::string& name : names)
    {
        Target t;
        if (!target(name, t))
        {
            continue;
        }
        Claim c;
        if (const Claim* old = find(name))
        {
            c = *old;
        }
        c.name = name;
        c.target = t;
        c.object = objectOf(t);
        const bool isOpen = c.object && visible(c.object) && ensurePatched(c.object);
        if (isOpen && !c.open)
        {
            c.openedAt = now;
            c.beatAt = 0;
            c.lapsed = false;
        }
        else if (isOpen && !c.lapsed && now - c.openedAt >= kGraceMs && now - c.beatAt >= kBeatMs)
        {
            c.lapsed = true;
            LOG("Replace: {} is open but its replacement was not drawn: the game's window shows", name);
        }
        c.open = isOpen;
        next.push_back(std::move(c));
    }
    g_claims = std::move(next);
    g_claimsAt = now;
}

bool Replace::beat(const std::string& name)
{
    std::scoped_lock lock(g_lock);
    Claim* c = find(name);
    if (!c)
    {
        return false;
    }
    // The tick's claims ran before the mods: c->open is this tick's.
    if (!c->open || c->lapsed)
    {
        return false;
    }
    c->beatAt = GetTickCount64();
    return true;
}

bool Replace::close(const std::string& name)
{
    Target t;
    if (!target(name, t))
    {
        return false;
    }
    const uintptr_t w = objectOf(t);
    if (!visible(w))
    {
        return false;
    }
    const bool savegame = (t.menu && t.index == kLoadgameMenu) || (!t.menu && t.index == kSavegameSlot);
    if (savegame && Gear::Addr::cUI_Savegame_command)
    {
        // Its Back button: in the menus it shows the start menu again.
        Gear::Addr::cUI_Savegame_command(reinterpret_cast<void*>(w), 0xC);
        return true;
    }
    if (!t.menu && Gear::Addr::cUI_Control_clearFlags)
    {
        Gear::Addr::cUI_Control_clearFlags(reinterpret_cast<void*>(w), 5);     // as the Esc ladder closes windows
        return true;
    }
    return false;
}

namespace
{
    // The entries of the game menu (ENG 006BBA80): a vector of cUI_StaticText64 at +0x158 / +0x15C, their text at +0x88 /
    // +0x8C (wide chars), each a control (x, y at +0x24, +0x28 relative to the parent at +0x50; width, height int16 at
    // +0x2C, +0x2E).
    constexpr uintptr_t kEntriesBegin = 0x158, kEntriesEnd = 0x15C;
    constexpr size_t kMaxEntries = 16;

    bool entryRect(uintptr_t control, float& x, float& y, float& w, float& h)
    {
        float ax = 0, ay = 0;
        uintptr_t c = control;
        for (int depth = 0; c && depth < 4; ++depth)
        {
            ax += static_cast<float>(readOr<int32_t>(c + 0x24, 0));
            ay += static_cast<float>(readOr<int32_t>(c + 0x28, 0));
            c = readOr<uintptr_t>(c + 0x50, 0);
        }
        x = ax;
        y = ay;
        w = static_cast<float>(readOr<int16_t>(control + 0x2C, 0));
        h = static_cast<float>(readOr<int16_t>(control + 0x2E, 0));
        return w > 0 && h > 0;
    }

    // The window's own receiveEvent (the one before Gear's thunk, if it is patched), so a press reaches it replaced or not.
    ReceiveFn receiveOf(uintptr_t object)
    {
        const uintptr_t vt = readOr<uintptr_t>(object, 0);
        if (!vt)
        {
            return nullptr;
        }
        if (const Patched* p = patchedFor(vt))
        {
            return p->receive;
        }
        return reinterpret_cast<ReceiveFn>(readOr<uintptr_t>(vt + 4 * 4, 0));
    }

    // Copies game memory (no objects in this function: __try needs none to unwind).
    bool copyBytes(void* destination, uintptr_t source, size_t bytes)
    {
        __try
        {
            std::memcpy(destination, reinterpret_cast<const void*>(source), bytes);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // A mouse event as the game builds them (vtable, button, x, y; the rest zero): what ui.cpp reads in its hook.
    struct MouseEvent
    {
        uintptr_t vtable;
        int32_t button;
        int32_t x, y;
        uint8_t rest[0x80];
    };

    bool sendMouse(uintptr_t object, ReceiveFn receive, uintptr_t vtable, int x, int y)
    {
        MouseEvent e{};
        e.vtable = vtable;
        e.button = 0;
        e.x = x;
        e.y = y;
        __try
        {
            receive(reinterpret_cast<void*>(object), nullptr, &e);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}

std::vector<Replace::Entry> Replace::entries(const std::string& name)
{
    std::vector<Entry> out;
    Target t;
    if (name != "UI_WND_ESCMENU" || !target(name, t))
    {
        return out;
    }
    const uintptr_t window = objectOf(t);
    if (!window || !visible(window))
    {
        return out;
    }
    const uintptr_t begin = readOr<uintptr_t>(window + kEntriesBegin, 0), end = readOr<uintptr_t>(window + kEntriesEnd, 0);
    if (!begin || end <= begin || (end - begin) / 4 > kMaxEntries)
    {
        return out;
    }
    for (uintptr_t at = begin; at < end; at += 4)
    {
        const uintptr_t control = readOr<uintptr_t>(at, 0);
        Entry e;
        if (!control || !entryRect(control, e.x, e.y, e.w, e.h))
        {
            continue;
        }
        e.textId = readOr<int32_t>(control + 0x84, 0);
        const uintptr_t text = readOr<uintptr_t>(control + 0x88, 0), textEnd = readOr<uintptr_t>(control + 0x8C, 0);
        if (text && textEnd > text && (textEnd - text) / 2 <= 128)
        {
            e.text.resize((textEnd - text) / 2);
            if (!copyBytes(e.text.data(), text, e.text.size() * 2))
            {
                e.text.clear();
            }
        }
        out.push_back(std::move(e));
    }
    return out;
}

bool Replace::clickEntry(const std::string& name, size_t index)
{
    const std::vector<Entry> list = entries(name);
    Target t;
    if (index >= list.size() || !target(name, t))
    {
        return false;
    }
    const uintptr_t window = objectOf(t);
    const ReceiveFn receive = window ? receiveOf(window) : nullptr;
    const uintptr_t down = Gear::Addr::cEventMouseDown_vtable, up = Gear::Addr::cEventMouseUp_vtable;
    if (!receive || !down || !up)
    {
        return false;
    }
    const Entry& e = list[index];
    const int x = static_cast<int>(e.x + e.w * 0.5f), y = static_cast<int>(e.y + e.h * 0.5f);
    return sendMouse(window, receive, down, x, y) && sendMouse(window, receive, up, x, y);
}
