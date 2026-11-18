#include "keyblock.h"
#include "log.h"
#include "patch.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace
{
    std::atomic<bool> g_blocked{false};
    // The block ends a moment after the screen asks: the game's message loop may not have taken a key that the screen
    // already acted on (Esc closes the screen at the tick, the key's message is still in the queue).
    std::atomic<ULONGLONG> g_graceUntil{0};
    constexpr ULONGLONG kGraceMs = 150;

    bool effectivelyBlocked()
    {
        return g_blocked.load(std::memory_order_relaxed) || GetTickCount64() < g_graceUntil.load(std::memory_order_relaxed);
    }
    std::mutex g_lock;
    std::set<WPARAM> g_swallowed;       // keys whose press was dropped: their release is dropped too
    // The character a dropped press makes (TranslateMessage queues it before we see the press) arrives a moment later,
    // maybe after the block ended: it is dropped too.
    std::atomic<ULONGLONG> g_dropCharsUntil{0};
    // What the dropped presses type (a Gear screen's text field), in the keyboard layout of the player (g_lock).
    std::wstring g_typed;
    constexpr size_t kTypedMax = 256;
    // The characters taken from presses lately: a character message that still comes for one of them (made by a
    // TranslateMessage before the press was dropped) is not taken twice (g_lock).
    struct Recent
    {
        wchar_t ch;
        ULONGLONG at;
    };
    std::vector<Recent> g_recent;
    constexpr ULONGLONG kRecentMs = 250;

    void addTyped(wchar_t ch, bool fromPress)
    {
        if (ch < 0x20 || ch == 0x7F)
        {
            return;
        }
        const ULONGLONG now = GetTickCount64();
        std::erase_if(g_recent, [now](const Recent& r) { return now - r.at > kRecentMs; });
        if (!fromPress)
        {
            for (auto it = g_recent.begin(); it != g_recent.end(); ++it)
            {
                if (it->ch == ch)
                {
                    g_recent.erase(it);
                    return;
                }
            }
        }
        else
        {
            g_recent.push_back({ch, now});
        }
        if (g_typed.size() < kTypedMax)
        {
            g_typed.push_back(ch);
        }
    }

    // The character(s) the press of `vk` makes with the keys held now (ToUnicodeEx without touching the dead key state).
    void noteTyped(WPARAM vk, LPARAM lParam)
    {
        BYTE state[256] = {};
        if (!GetKeyboardState(state))
        {
            return;
        }
        if (state[VK_CONTROL] & 0x80 && !(state[VK_MENU] & 0x80))
        {
            return;     // Ctrl+key types nothing (AltGr is Ctrl+Alt and does)
        }
        wchar_t buffer[8] = {};
        const UINT scan = static_cast<UINT>((lParam >> 16) & 0xFF);
        const int n = ToUnicodeEx(static_cast<UINT>(vk), scan, state, buffer, 8, 4, GetKeyboardLayout(0));
        for (int i = 0; i < n; ++i)
        {
            addTyped(buffer[i], true);
        }
    }

    using GetAsyncKeyStateFn = SHORT(WINAPI*)(int);
    using GetKeyStateFn = SHORT(WINAPI*)(int);
    using GetKeyboardStateFn = BOOL(WINAPI*)(PBYTE);
    GetAsyncKeyStateFn g_origAsync = nullptr;
    GetKeyStateFn g_origState = nullptr;
    GetKeyboardStateFn g_origKeyboard = nullptr;

    // Keys held when the block ended stay hidden from the game until they are let go: the game would otherwise see a
    // key it never saw pressed (the Esc that closed the screen is still down for a moment).
    std::atomic<bool> g_hidden[256];

    // Keys 1..7 are the mouse buttons: those stay.
    bool isKey(int vk)
    {
        return vk >= 8 && vk < 256;
    }

    // Whether the game must see `vk` as up. `down`: the real state, to learn when a hidden key is released.
    bool hide(int vk, bool down)
    {
        if (!isKey(vk))
        {
            return false;
        }
        if (effectivelyBlocked())
        {
            return true;
        }
        if (g_hidden[vk].load(std::memory_order_relaxed))
        {
            if (!down)
            {
                g_hidden[vk].store(false);
            }
            return true;
        }
        return false;
    }

    SHORT WINAPI hookAsync(int vk)
    {
        const SHORT real = g_origAsync(vk);
        return hide(vk, (real & 0x8000) != 0) ? SHORT(0) : real;
    }

    SHORT WINAPI hookState(int vk)
    {
        const SHORT real = g_origState(vk);
        return hide(vk, (real & 0x8000) != 0) ? SHORT(0) : real;
    }

    BOOL WINAPI hookKeyboard(PBYTE keys)
    {
        const BOOL ok = g_origKeyboard(keys);
        if (ok && keys)
        {
            for (int i = 8; i < 256; ++i)
            {
                if (hide(i, (keys[i] & 0x80) != 0))
                {
                    keys[i] = 0;
                }
            }
        }
        return ok;
    }

    HWND g_window = nullptr;
    WNDPROC g_next = nullptr;

    // Whether a key or character message must not reach the game. `consume`: the message is being taken off the queue
    // (a PM_NOREMOVE peek decides the same way, but leaves the bookkeeping to the real read).
    bool dropMessage(UINT message, WPARAM wParam, LPARAM lParam, bool consume)
    {
        const bool blocked = effectivelyBlocked();
        switch (message)
        {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (blocked)
            {
                if (consume)
                {
                    std::lock_guard guard(g_lock);
                    g_swallowed.insert(wParam);
                    g_dropCharsUntil.store(GetTickCount64() + 200);
                    if (message == WM_KEYDOWN)
                    {
                        noteTyped(wParam, lParam);
                    }
                }
                return true;
            }
            return false;
        case WM_KEYUP:
        case WM_SYSKEYUP:
        {
            std::lock_guard guard(g_lock);
            const auto it = g_swallowed.find(wParam);
            if (it == g_swallowed.end())
            {
                return false;
            }
            if (consume)
            {
                g_swallowed.erase(it);
            }
            return true;
        }
        case WM_CHAR:
            if (consume && effectivelyBlocked() && g_blocked.load())
            {
                // A character on its own (posted, or made from a press before it was dropped): typed too.
                std::lock_guard guard(g_lock);
                addTyped(static_cast<wchar_t>(wParam), false);
            }
            return blocked || GetTickCount64() < g_dropCharsUntil.load();
        case WM_SYSCHAR:
        case WM_DEADCHAR:
        case WM_SYSDEADCHAR:
        case WM_UNICHAR:
        case WM_IME_CHAR:
            return blocked || GetTickCount64() < g_dropCharsUntil.load();
        default:
            return false;
        }
    }

    LRESULT CALLBACK windowProc(HWND w, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (dropMessage(message, wParam, lParam, true))
        {
            return 0;
        }
        if (message == WM_KILLFOCUS)
        {
            std::lock_guard guard(g_lock);
            g_swallowed.clear();
        }
        return CallWindowProcA(g_next, w, message, wParam, lParam);
    }

    // The game reads its messages itself and may act on keys before (or without) dispatching them: the messages it is
    // handed are filtered too (a dropped one becomes WM_NULL).
    using PeekMessageAFn = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
    using GetMessageAFn = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT);
    PeekMessageAFn g_origPeek = nullptr;
    GetMessageAFn g_origGet = nullptr;

    BOOL WINAPI hookPeek(LPMSG msg, HWND w, UINT min, UINT max, UINT remove)
    {
        const BOOL ok = g_origPeek(msg, w, min, max, remove);
        if (ok && msg && dropMessage(msg->message, msg->wParam, msg->lParam, (remove & PM_REMOVE) != 0))
        {
            msg->message = WM_NULL;
        }
        return ok;
    }

    BOOL WINAPI hookGet(LPMSG msg, HWND w, UINT min, UINT max)
    {
        const BOOL ok = g_origGet(msg, w, min, max);
        if (ok > 0 && msg && dropMessage(msg->message, msg->wParam, msg->lParam, true))
        {
            msg->message = WM_NULL;
        }
        return ok;
    }

    HWND findGameWindow()
    {
        struct Search
        {
            HWND found = nullptr;
        } search;
        EnumWindows(
            [](HWND w, LPARAM p) -> BOOL {
                DWORD pid = 0;
                GetWindowThreadProcessId(w, &pid);
                char name[32] = {};
                if (pid == GetCurrentProcessId() && GetClassNameA(w, name, sizeof name) && std::strcmp(name, "Sacred") == 0)
                {
                    reinterpret_cast<Search*>(p)->found = w;
                    return FALSE;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&search));
        return search.found;
    }

    bool inGameImage(WNDPROC proc)
    {
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        const auto addr = reinterpret_cast<uintptr_t>(proc);
        return addr >= base && addr < base + nt->OptionalHeader.SizeOfImage;
    }
}

void KeyBlock::install()
{
    g_origAsync = reinterpret_cast<GetAsyncKeyStateFn>(Patch::iat("USER32.dll", "GetAsyncKeyState", reinterpret_cast<void*>(&hookAsync)));
    g_origState = reinterpret_cast<GetKeyStateFn>(Patch::iat("USER32.dll", "GetKeyState", reinterpret_cast<void*>(&hookState)));
    g_origKeyboard = reinterpret_cast<GetKeyboardStateFn>(Patch::iat("USER32.dll", "GetKeyboardState", reinterpret_cast<void*>(&hookKeyboard)));
    g_origPeek = reinterpret_cast<PeekMessageAFn>(Patch::iat("USER32.dll", "PeekMessageA", reinterpret_cast<void*>(&hookPeek)));
    g_origGet = reinterpret_cast<GetMessageAFn>(Patch::iat("USER32.dll", "GetMessageA", reinterpret_cast<void*>(&hookGet)));
    LOG("KeyBlock: GetAsyncKeyState {}, GetKeyState {}, GetKeyboardState {}", g_origAsync ? "hooked" : "not imported",
        g_origState ? "hooked" : "not imported", g_origKeyboard ? "hooked" : "not imported");
}

void KeyBlock::set(bool blocked)
{
    if (g_blocked.exchange(blocked) != blocked)
    {
        LOG("KeyBlock: keyboard {} the game", blocked ? "kept from" : "given back to");
        if (!blocked)
        {
            g_graceUntil.store(GetTickCount64() + kGraceMs);
        }
        if (!blocked && g_origAsync)
        {
            for (int vk = 8; vk < 256; ++vk)
            {
                if (g_origAsync(vk) & 0x8000)
                {
                    g_hidden[vk].store(true);
                }
            }
        }
    }
}

std::wstring KeyBlock::takeTyped()
{
    std::lock_guard guard(g_lock);
    std::wstring out;
    out.swap(g_typed);
    return out;
}

bool KeyBlock::blocked()
{
    return g_blocked.load();
}

void KeyBlock::tick()
{
    // Only once something was blocked: the window procedure stays then.
    if (!g_blocked.load() && !g_next)
    {
        return;
    }
    if (!g_window || !IsWindow(g_window))
    {
        g_window = findGameWindow();
        g_next = nullptr;
    }
    if (!g_window)
    {
        return;
    }
    const auto current = reinterpret_cast<WNDPROC>(GetWindowLongPtrA(g_window, GWLP_WNDPROC));
    if (current == &windowProc)
    {
        return;
    }
    // Ours is not in front: take the front when the game put its own procedure there (a new game replaces it), or when
    // there is none of ours yet (someone else's may already call ours, which is why a foreign procedure is left).
    if (!g_next || inGameImage(current))
    {
        g_next = current;
        SetWindowLongPtrA(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&windowProc));
        LOG("KeyBlock: window procedure in place");
    }
}
