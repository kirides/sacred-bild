#include "game/ui_windows.h"
#include "game/sacred_addr.h"
#include "game/ui_nav.h"
#include "log.h"
#include "patch.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <iterator>
#include <mutex>

namespace
{
    using namespace Sacred;
    using UiWindows::Kind;
    using UiWindows::Owner;
    using W = cUI_Manager::Window;

    UiWindows::Handlers g_handlers;
    bool g_installed = false;

    // Wrapped vtables: the game's vtable slots point at the thunks below, which look up the original by the object's
    // vtable. A vtable is shared by all objects of its class (the character screen is a menu and a game window).
    constexpr size_t kSlotCount = sizeof(cUI_Window2::Vtable) / sizeof(void*);
    struct Vtable
    {
        const cUI_Control2::Vtable* table;
        cUI_Window2::Vtable original;   // the slots up to the table's last (the rest zero)
    };
    Vtable g_vtables[64];
    std::atomic<size_t> g_vtableCount{0};
    std::mutex g_patchMutex;
    uintptr_t g_codeBegin = 0, g_codeEnd = 0;
    std::atomic<const cUI_Control2::Vtable*> g_popupVtable{nullptr};

    thread_local Owner t_current;

    // Plain fastcall (manager) in the exe: hooked without EDX.
    using CreateGameWindowsFn = void(__fastcall*)(cUI_Manager* manager);
    CreateGameWindowsFn g_origCreateGameWindows = nullptr;

    const cUI_Window2::Vtable& original(const cUI_Control2* self)
    {
        const size_t n = g_vtableCount.load(std::memory_order_acquire);
        for (size_t i = 0; i < n; ++i)
        {
            if (g_vtables[i].table == self->vtable)
            {
                return g_vtables[i].original;
            }
        }
        static const cUI_Window2::Vtable none{};
        return none;    // unreachable: only patched vtables lead here
    }

    // The calling thread runs `owner`'s code while it lives.
    class OwnerScope
    {
    public:
        explicit OwnerScope(const Owner& owner) : m_previous(t_current) { t_current = owner; }
        ~OwnerScope() { t_current = m_previous; }
        OwnerScope(const OwnerScope&) = delete;
        OwnerScope& operator=(const OwnerScope&) = delete;

    private:
        Owner m_previous;
    };

    bool frameOf(const Owner& owner, UiCanvas::Frame& out)
    {
        return owner.kind != Kind::None && g_handlers.frame && g_handlers.frame(owner, out);
    }

    void traceCall(const Owner& owner, const char* slot)
    {
        if (UiCanvas::tracing())
        {
            UiCanvas::trace(Fmt::format("{} {}", UiWindows::describe(owner), slot));
        }
    }

    uint32_t __fastcall thunkEvent(cUI_Control2* self, void* edx, cEvent* event)
    {
        const auto orig = original(self).receiveEvent;
        const Owner owner = UiWindows::identify(self);
        OwnerScope scope{owner};
        UiCanvas::Frame frame;
        if (!g_handlers.receiveEvent || !frameOf(owner, frame) || frame == UiCanvas::frame())
        {
            return orig(self, edx, event);
        }
        return g_handlers.receiveEvent(owner, frame, event, orig);
    }

    uint32_t __fastcall thunkInside(cUI_Control2* self, void* edx, int x, int y)
    {
        const auto orig = original(self).isInside;
        const Owner owner = UiWindows::identify(self);
        OwnerScope scope{owner};
        UiCanvas::Frame frame;
        if (!g_handlers.isInside || !frameOf(owner, frame) || frame == UiCanvas::frame())
        {
            return orig(self, edx, x, y);
        }
        return g_handlers.isInside(owner, frame, x, y, orig);
    }

    // render, render2 and show run in the object's frame.
    template <typename Self, typename Fn, typename... Args>
    uint32_t inFrame(Self* self, const char* slot, Fn orig, Args... args)
    {
        const Owner owner = UiWindows::identify(self);
        OwnerScope scope{owner};
        UiCanvas::Frame frame;
        if (!frameOf(owner, frame))
        {
            traceCall(owner, slot);
            return orig(self, args...);
        }
        UiCanvas::FrameScope frameScope{frame};
        traceCall(owner, slot);
        return orig(self, args...);
    }

    uint32_t __fastcall thunkRender(cUI_Control2* self, void* edx, IDirect3DDevice7* device)
    {
        return inFrame(self, "render", original(self).render, edx, device);
    }

    uint32_t __fastcall thunkShow(cUI_Window2* self, void* edx, uint32_t show)
    {
        return inFrame(self, show & 0xFF ? "show" : "hide", original(self).show, edx, show);
    }

    uint32_t __fastcall thunkRender2(cUI_Window2* self, void* edx, IDirect3DDevice7* device, uint32_t a, uint32_t b)
    {
        return inFrame(self, "render2", original(self).render2, edx, device, a, b);
    }

    bool isCode(const void* p)
    {
        const auto a = reinterpret_cast<uintptr_t>(p);
        return a >= g_codeBegin && a < g_codeEnd;
    }

    // The objects the manager draws through render2 (device, ?, ?), cUI_Manager render ENG 00758ED0; other classes'
    // slot there, if they have one, may take other arguments and is left alone.
    bool drawsWithRender2(const Owner& owner)
    {
        switch (owner.kind)
        {
        case Kind::Window:
            switch (owner.window())
            {
            case W::blacksmith: case W::merchant: case W::questbook: case W::horse: case W::purchase: case W::options:
            case W::character: case W::netPortraits:
                return true;
            default:
                return false;
            }
        case Kind::Menu:
            return owner.index == 1 || owner.index == 2;    // the character screens and the options
        default:
            return false;
        }
    }

    bool wrapped(const cUI_Control2::Vtable* table)
    {
        const size_t n = g_vtableCount.load(std::memory_order_acquire);
        for (size_t i = 0; i < n; ++i)
        {
            if (g_vtables[i].table == table)
            {
                return true;
            }
        }
        return false;
    }

    // Points the object's vtable slots at the thunks (once per vtable). A slot is only touched if it and every slot
    // before it hold code addresses: the slot after a vtable's last one is the next vtable's RTTI pointer (data).
    void wrap(const cUI_Control2* object)
    {
        if (!object || wrapped(object->vtable))
        {
            return;
        }
        const cUI_Control2::Vtable* table = object->vtable;
        std::lock_guard lock(g_patchMutex);
        if (wrapped(table))
        {
            return;
        }
        const size_t n = g_vtableCount.load(std::memory_order_relaxed);
        if (n == std::size(g_vtables))
        {
            LOG("UI windows: no room to wrap vtable {}", static_cast<const void*>(table));
            return;
        }
        Vtable& v = g_vtables[n];
        v.table = table;
        const auto* slots = reinterpret_cast<void* const*>(table);
        size_t count = 0;
        while (count < kSlotCount && isCode(slots[count]))
        {
            ++count;
        }
        std::memcpy(&v.original, table, count * sizeof(void*));
        // Publish the originals before any slot leads to a thunk.
        g_vtableCount.store(n + 1, std::memory_order_release);
        auto* game = const_cast<cUI_Window2::Vtable*>(static_cast<const cUI_Window2::Vtable*>(table));
        int patched = 0;
        const auto patch = [&](auto& slot, decltype(+slot) thunk) {
            if (reinterpret_cast<uintptr_t>(&slot) - reinterpret_cast<uintptr_t>(game) < count * sizeof(void*))
            {
                patched += Patch::value(reinterpret_cast<uintptr_t>(&slot), thunk) ? 1 : 0;
            }
        };
        patch(game->receiveEvent, &thunkEvent);
        patch(game->render, &thunkRender);
        patch(game->isInside, &thunkInside);
        patch(game->show, &thunkShow);
        const Owner owner = UiWindows::identify(object);
        if (drawsWithRender2(owner))
        {
            patch(game->render2, &thunkRender2);
        }
        LOG("UI windows: {} ({}) vtable {} ({} slots) wrapped in {} places", UiWindows::describe(owner),
            UiNav::className(object), static_cast<const void*>(table), count, patched);
    }

    void __fastcall hookCreateGameWindows(cUI_Manager* manager)
    {
        g_origCreateGameWindows(manager);
        UiWindows::sync();
        if (g_handlers.windowsCreated)
        {
            g_handlers.windowsCreated(manager);
        }
    }

    void findCode()
    {
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
        {
            if (section->Characteristics & IMAGE_SCN_MEM_EXECUTE)
            {
                g_codeBegin = base + section->VirtualAddress;
                g_codeEnd = g_codeBegin + section->Misc.VirtualSize;
                return;
            }
        }
    }
}

Owner UiWindows::identify(const cUI_Control2* object)
{
    cUI_Manager* manager = cUI_Manager::instance();
    if (!object || !manager)
    {
        return {};
    }
    auto* self = const_cast<cUI_Control2*>(object);
    if (manager->flags & cUI_Manager::windowsCreated)
    {
        for (int i = 0; i < static_cast<int>(W::count); ++i)
        {
            if (manager->windows[i] == object)
            {
                return {Kind::Window, i, self};
            }
        }
    }
    if (manager->flags & cUI_Manager::menusCreated)
    {
        for (int i = 0; i < cUI_Manager::menuCount; ++i)
        {
            if (manager->menus[i] == object)
            {
                return {Kind::Menu, i, self};
            }
        }
    }
    if (manager->dialog == object)
    {
        return {Kind::MessageBox, -1, self};
    }
    if (manager->textLines() == object)
    {
        return {Kind::TextLines, -1, self};
    }
    if (object->vtable == g_popupVtable.load(std::memory_order_relaxed))
    {
        return {Kind::Popup, -1, self};
    }
    return {};
}

Owner UiWindows::current()
{
    return t_current;
}

std::string UiWindows::describe(const Owner& owner)
{
    switch (owner.kind)
    {
    case Kind::Window: return std::string("window ") + UiState::name(owner.window());
    case Kind::Menu: return Fmt::format("menu {}", owner.index);
    case Kind::MessageBox: return "message box";
    case Kind::Popup: return Fmt::format("popup {}", static_cast<const void*>(owner.object));
    case Kind::TextLines: return "text lines";
    default: return "manager";
    }
}

void UiWindows::setHandlers(const Handlers& handlers)
{
    g_handlers = handlers;
}

void UiWindows::sync()
{
    cUI_Manager* manager = cUI_Manager::instance();
    if (!g_installed || !manager)
    {
        return;
    }
    if (manager->flags & cUI_Manager::windowsCreated)
    {
        for (cUI_Window2* window : manager->windows)
        {
            wrap(window);
        }
    }
    if (manager->flags & cUI_Manager::menusCreated)
    {
        for (cUI_Window2* menu : manager->menus)
        {
            wrap(menu);
        }
    }
    wrap(manager->dialog);
    wrap(manager->textLines());
    // The popups exist since the manager's constructor and share one class (cUI_Tooltip).
    if (!g_popupVtable.load(std::memory_order_relaxed) && manager->popups.size() < 256)
    {
        for (cUI_Popup* popup : manager->popups)
        {
            if (popup)
            {
                g_popupVtable = popup->vtable;
                wrap(popup);
                break;
            }
        }
    }
}

namespace
{
    // The windows' drawn rects (canvas coordinates): being drawn (render thread) and of the last frame (g_drawnLock).
    struct Drawn
    {
        float left = 0, top = 0, right = 0, bottom = 0;
        bool any = false;
    };
    constexpr size_t kWindows = static_cast<size_t>(W::count);
    Drawn g_drawing[kWindows];
    Drawn g_drawn[kWindows];
    ULONGLONG g_drawnAt[kWindows]{};
    SRWLOCK g_drawnLock = SRWLOCK_INIT;
}

void UiWindows::noteDraw(float minX, float minY, float maxX, float maxY)
{
    const Owner& owner = t_current;
    const UiCanvas::Frame frame = UiCanvas::frame();
    if (owner.kind != Kind::Window || owner.index < 0 || owner.index >= static_cast<int>(kWindows) || !frame.confine)
    {
        return;
    }
    minX = std::max(minX, 0.0f);
    minY = std::max(minY, 0.0f);
    maxX = std::min(maxX, 1024.0f);
    maxY = std::min(maxY, 768.0f);
    if (maxX <= minX || maxY <= minY)
    {
        return;
    }
    Drawn& d = g_drawing[owner.index];
    if (!d.any)
    {
        d = {minX, minY, maxX, maxY, true};
        return;
    }
    d.left = std::min(d.left, minX);
    d.top = std::min(d.top, minY);
    d.right = std::max(d.right, maxX);
    d.bottom = std::max(d.bottom, maxY);
}

void UiWindows::publishDrawn()
{
    // A window that did not draw in this pass keeps its last rect for a moment: the manager skips frames while the
    // engine is paused, and Sacred Gear lists only the windows the manager draws.
    const ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_drawnLock);
    for (size_t i = 0; i < kWindows; ++i)
    {
        if (g_drawing[i].any)
        {
            g_drawn[i] = g_drawing[i];
            g_drawnAt[i] = now;
        }
        else if (now - g_drawnAt[i] > 250)
        {
            g_drawn[i] = {};
        }
    }
    ReleaseSRWLockExclusive(&g_drawnLock);
    std::fill(std::begin(g_drawing), std::end(g_drawing), Drawn{});
}

// Sacred Gear: where window `slot` (cUI_Manager::Window) drew in the last UI frame, as x, y, width, height in the
// window's own layout (add its frame for the canvas): its visual bounds. 0 if it drew nothing (closed, hidden, the
// canvas off).
extern "C" int __cdecl SacredBild_UiGetWindowBounds(int slot, float* rect)
{
    if (slot < 0 || slot >= static_cast<int>(kWindows) || !rect)
    {
        return 0;
    }
    AcquireSRWLockShared(&g_drawnLock);
    const Drawn d = g_drawn[slot];
    ReleaseSRWLockShared(&g_drawnLock);
    if (!d.any)
    {
        return 0;
    }
    rect[0] = d.left;
    rect[1] = d.top;
    rect[2] = d.right - d.left;
    rect[3] = d.bottom - d.top;
    return 1;
}

void UiWindows::install()
{
    if (!UiCanvas::enabled())
    {
        return;
    }
    findCode();
    g_installed = true;
    Patch::hook(g_origCreateGameWindows, Addr::cUI_Manager_createGameWindows, &hookCreateGameWindows,
        "cUI_Manager::createGameWindows");
}
