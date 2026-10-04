#include "game/focus.h"
#include "game/sacred_addr.h"
#include "config.h"
#include "log.h"
#include "patch.h"

#include <atomic>
#include <mutex>

namespace
{
    using GetAsyncKeyStateFn = SHORT(WINAPI*)(int);
    using SetCursorPosFn = BOOL(WINAPI*)(int, int);
    using SetWindowsHookExAFn = HHOOK(WINAPI*)(int, HOOKPROC, HINSTANCE, DWORD);

    GetAsyncKeyStateFn g_origGetAsyncKeyState = nullptr;
    SetCursorPosFn g_origSetCursorPos = nullptr;
    SetWindowsHookExAFn g_origSetWindowsHookExA = nullptr;
    HOOKPROC g_gameKeyboardHook = nullptr;

    HWND g_window = nullptr;
    std::atomic<WNDPROC> g_origWindowProc{nullptr};
    std::mutex g_clipMutex;
    bool g_clipped = false;
    RECT g_target = {};     // the client area clipped to
    RECT g_clip = {};       // what Windows made of it

    // One of the game's windows (the main window, a dialog, a message box) is in the foreground.
    bool active()
    {
        DWORD process = 0;
        HWND foreground = GetForegroundWindow();
        return foreground && GetWindowThreadProcessId(foreground, &process) && process == GetCurrentProcessId();
    }

    SHORT WINAPI hookGetAsyncKeyState(int key)
    {
        const SHORT state = g_origGetAsyncKeyState(key);   // also consumes the "pressed since last call" bit
        return active() ? state : 0;
    }

    BOOL WINAPI hookSetCursorPos(int x, int y)
    {
        return active() ? g_origSetCursorPos(x, y) : TRUE;
    }

    // The game's hook swallows the Windows keys, Ctrl+Esc, Alt+Tab and Alt+Esc. Only in the foreground, and Alt+Tab /
    // Alt+Esc not at all: with the cursor confined to the window they are the way out, and the windowed Direct3D 9
    // device doesn't mind losing the foreground the way the original fullscreen mode did.
    LRESULT CALLBACK keyboardHook(int code, WPARAM wParam, LPARAM lParam)
    {
        if (code == HC_ACTION)
        {
            const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
            const bool altSwitch = (key->vkCode == VK_TAB || key->vkCode == VK_ESCAPE) && (key->flags & LLKHF_ALTDOWN);
            if (altSwitch || !active())
            {
                return CallNextHookEx(nullptr, code, wParam, lParam);
            }
        }
        return g_gameKeyboardHook(code, wParam, lParam);
    }

    HHOOK WINAPI hookSetWindowsHookExA(int id, HOOKPROC proc, HINSTANCE module, DWORD thread)
    {
        if (id == WH_KEYBOARD_LL && proc)
        {
            g_gameKeyboardHook = proc;
            proc = &keyboardHook;
        }
        return g_origSetWindowsHookExA(id, proc, module, thread);
    }

    void releaseLocked()
    {
        if (g_clipped)
        {
            ClipCursor(nullptr);
            g_clipped = false;
        }
    }

    void release()
    {
        std::scoped_lock lock(g_clipMutex);
        releaseLocked();
    }

    // Releases the cursor right away when the window loses the foreground, is moved, or a menu opens; onFrame
    // confines it again. The close button and the system menu's Close don't quit (too easy to hit by accident);
    // Alt+F4 still does.
    LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if ((message == WM_ACTIVATE && LOWORD(wParam) == WA_INACTIVE) || (message == WM_ACTIVATEAPP && !wParam) ||
            message == WM_ENTERSIZEMOVE || message == WM_ENTERMENULOOP || message == WM_DESTROY)
        {
            release();
        }
        if (message == WM_SYSKEYDOWN && wParam == VK_F4)
        {
            PostMessageA(window, WM_CLOSE, 0, 0);   // what DefWindowProc would do, but Close is disabled
            return 0;
        }
        if (message == WM_SYSCOMMAND && (wParam & 0xFFF0) == SC_CLOSE && HIWORD(lParam) != 0xFFFF)
        {
            return 0;   // by mouse (HIWORD -1 = by accelerator)
        }
        if (message == WM_SYSCOMMAND && (wParam & 0xFFF0) == SC_KEYMENU && lParam == 0)
        {
            return 0;   // Alt alone (the game shows item names with it) doesn't enter the menu; Alt+Space still opens it
        }
        return CallWindowProcA(g_origWindowProc.load(), window, message, wParam, lParam);
    }

    // A window procedure of the game itself (read through GetWindowLongPtrA, which gives the game's ANSI procedures
    // as addresses; one set through SetWindowLongPtrW reads as a handle outside any module).
    bool gameProc(WNDPROC proc)
    {
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        const auto addr = reinterpret_cast<uintptr_t>(proc);
        return addr >= base && addr < base + nt->OptionalHeader.SizeOfImage;
    }

    // Subclasses the main window. `onlyOverGame`: only if the game replaced its window procedure (it does when a game
    // starts), not over a subclass another module put over this one (opengl32's with DXVK under RenderDoc, overlays):
    // that one calls windowProc in turn, and windowProc calling it back recursed until the stack overflowed.
    void subclass(HWND window, bool onlyOverGame)
    {
        const auto current = reinterpret_cast<WNDPROC>(GetWindowLongPtrA(window, GWLP_WNDPROC));
        if (current && current != &windowProc && (!onlyOverGame || gameProc(current)))
        {
            g_origWindowProc = current;
            SetWindowLongPtrA(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&windowProc));
        }
    }

    // The window's client area on the screen, if the cursor should be confined to it now.
    bool clipRect(RECT& rect)
    {
        HWND window = g_window;
        if (!window || GetForegroundWindow() != window || IsIconic(window))
        {
            return false;
        }
        // Holding Alt lets the cursor go (as in many newer games); once released, the cursor is taken again when it
        // is back over the window.
        if (GetAsyncKeyState(VK_MENU) < 0)
        {
            return false;
        }
        // Not while the window is moved or sized, or its system menu is open.
        GUITHREADINFO gui = {};
        gui.cbSize = sizeof(gui);
        if (GetGUIThreadInfo(GetWindowThreadProcessId(window, nullptr), &gui) &&
            (gui.flags & (GUI_INMOVESIZE | GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE)))
        {
            return false;
        }
        POINT origin = {};
        if (!GetClientRect(window, &rect) || !ClientToScreen(window, &origin))
        {
            return false;
        }
        OffsetRect(&rect, origin.x, origin.y);
        return !IsRectEmpty(&rect);
    }
}

void Focus::install()
{
    g_origGetAsyncKeyState = static_cast<GetAsyncKeyStateFn>(
        Patch::iat("USER32.dll", "GetAsyncKeyState", reinterpret_cast<void*>(&hookGetAsyncKeyState)));
    g_origSetCursorPos = static_cast<SetCursorPosFn>(
        Patch::iat("USER32.dll", "SetCursorPos", reinterpret_cast<void*>(&hookSetCursorPos)));
    g_origSetWindowsHookExA = static_cast<SetWindowsHookExAFn>(
        Patch::iat("USER32.dll", "SetWindowsHookExA", reinterpret_cast<void*>(&hookSetWindowsHookExA)));
    LOG("Focus: input only in the foreground, ClipCursor={}", g_config.clipCursor);

    // The window procedure no longer pauses rendering when the game loses the foreground: the menu loop's last
    // frame was often cleared without its UI (a black screen), and switching back during an intro movie resumed the
    // menu under it. [Display] FpsLimitInactive paces the frames in the background.
    // and eax, 0xFFFDFFFF -> and eax, 0xFFFFFFFF; or eax, edi -> mov eax, eax
    constexpr uint8_t kKeep[] = {0x8B, 0xC0};
    if (Patch::verify(Sacred::Addr::activateAppResumeAnd, {0x25, 0xFF, 0xFF, 0xFD, 0xFF}) &&
        Patch::verify(Sacred::Addr::activateAppPauseOr, {0x0B, 0xC7}) &&
        Patch::imm32(Sacred::Addr::activateAppResumeAnd + 1, 0xFFFDFFFF, 0xFFFFFFFF) &&
        Patch::write(Sacred::Addr::activateAppPauseOr, kKeep, sizeof(kKeep)))
    {
        LOG("Focus: rendering continues in the background");
    }
}

void Focus::windowCreated(HWND window)
{
    g_window = window;
    subclass(window, false);
    EnableMenuItem(GetSystemMenu(window, FALSE), SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);   // greys the close button
}

bool Focus::foreground()
{
    return active();
}

void Focus::onFrame()
{
    HWND window = g_window;
    if (!window)
    {
        return;
    }
    subclass(window, true);
    if (!g_config.clipCursor)
    {
        return;
    }
    RECT rect;
    const bool clip = clipRect(rect);
    std::scoped_lock lock(g_clipMutex);
    if (!clip)
    {
        releaseLocked();
        return;
    }
    if (!g_clipped)
    {
        // Only once the cursor is over the client area: a windowed game activated by a click on its title bar can
        // still be dragged.
        POINT cursor;
        if (!GetCursorPos(&cursor) || !PtInRect(&rect, cursor))
        {
            return;
        }
    }
    else
    {
        RECT current;
        if (EqualRect(&rect, &g_target) && GetClipCursor(&current) && EqualRect(&current, &g_clip))
        {
            return;     // still in place
        }
    }
    // New, moved or resized, or reset by Windows (e.g. a UAC prompt).
    if (ClipCursor(&rect) && GetClipCursor(&g_clip))   // Windows clamps it to the screen
    {
        g_target = rect;
        g_clipped = true;
    }
}
