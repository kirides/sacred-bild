#include "automation.h"
#include "game/gear_addr.h"
#include "instance.h"
#include "log.h"
#include "lua_host.h"
#include "patch.h"

#include <ddraw.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace
{
    using namespace Gear;

    // UI manager and control layout (SacredBild's src/sacred/ui.h, which documents them).
    constexpr uintptr_t kUiFlags = 0x08, kUiWindows = 0x80, kUiMenus = 0x130, kUiDialog = 0x144;
    constexpr int kUiWindowCount = 23, kUiMenuCount = 5;
    constexpr uint32_t kUiInMenus = 0x01, kUiInGame = 0x04, kUiCinematic = 0x10, kUiHelp = 0x200;
    constexpr uintptr_t kCtlFlags = 0x10, kCtlX = 0x24, kCtlY = 0x28, kCtlW = 0x2C, kCtlH = 0x2E;
    constexpr uintptr_t kCtlChildren = 0x78;    // cUI_Window2's Vector<cUI_Control2*>
    constexpr uint32_t kCtlVisible = 0x01;
    constexpr uintptr_t kAppWindow = 0xB0, kPlayersLocalHero = 0x14, kMouseX = 0x04, kMouseY = 0x08;
    constexpr UINT kLoadMessage = 0x8002;       // WM_APP + 2: wParam 1 load, lParam the savegame number (007561E0)

    Automation::Options g_options;
    bool g_parsed = false;

    std::mutex g_inputLock;
    // Automation's own cursor (client pixels) and held keys / buttons. Isolated (the default): the game sees only
    // these. Shared (-gear-shared-input): the virtual cursor until the real mouse moves, held keys added to the real
    // ones.
    bool g_virtualCursor = false;
    POINT g_cursor{};
    bool g_cursorSet = false;
    POINT g_realAtSwitch{};
    std::set<int> g_held;

    bool isolated() { return g_options.enabled && !g_options.sharedInput; }

    using GetCursorPosFn = BOOL(WINAPI*)(LPPOINT);
    using SetCursorPosFn = BOOL(WINAPI*)(int, int);
    using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
    using GetAsyncKeyStateFn = SHORT(WINAPI*)(int);
    using GetKeyStateFn = SHORT(WINAPI*)(int);
    using GetKeyboardStateFn = BOOL(WINAPI*)(PBYTE);
    using HwndFn = HWND(WINAPI*)();
    using SetHwndFn = HWND(WINAPI*)(HWND);
    using BoolHwndFn = BOOL(WINAPI*)(HWND);
    using ShowWindowFn = BOOL(WINAPI*)(HWND, int);
    using SetWindowPosFn = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
    using CreateWindowExAFn = HWND(WINAPI*)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
    using SetWindowsHookExAFn = HHOOK(WINAPI*)(int, HOOKPROC, HINSTANCE, DWORD);
    using UnhookWindowsHookExFn = BOOL(WINAPI*)(HHOOK);

    GetCursorPosFn g_origGetCursorPos = nullptr;
    SetCursorPosFn g_origSetCursorPos = nullptr;
    ClipCursorFn g_origClipCursor = nullptr;
    GetAsyncKeyStateFn g_origGetAsyncKeyState = nullptr;
    GetKeyStateFn g_origGetKeyState = nullptr;
    GetKeyboardStateFn g_origGetKeyboardState = nullptr;
    HwndFn g_origGetForegroundWindow = nullptr;
    HwndFn g_origGetFocus = nullptr;
    SetHwndFn g_origSetFocus = nullptr;
    SetHwndFn g_origSetCapture = nullptr;
    BoolHwndFn g_origSetForegroundWindow = nullptr;
    BoolHwndFn g_origBringWindowToTop = nullptr;
    HwndFn g_origGetActiveWindow = nullptr;
    SetHwndFn g_origSetActiveWindow = nullptr;
    ShowWindowFn g_origShowWindow = nullptr;
    SetWindowPosFn g_origSetWindowPos = nullptr;
    CreateWindowExAFn g_origCreateWindowExA = nullptr;
    SetWindowsHookExAFn g_origSetWindowsHookExA = nullptr;
    UnhookWindowsHookExFn g_origUnhookWindowsHookEx = nullptr;

    // A system-wide low-level hook the game asked for while isolated: never installed (the game's own one swallows
    // the Windows keys and Alt+Tab for the whole desktop), answered with this handle.
    const HHOOK kFakeHook = reinterpret_cast<HHOOK>(static_cast<uintptr_t>(0x6EA70001));

    // Input automation posts as a private message that the message hook (on the window's thread) turns into the real
    // one just before the game gets it; real mouse and keyboard messages are dropped there while isolated.
    constexpr UINT kInjected = WM_APP + 0x3F37;
    constexpr LPARAM kInjectedTag = 0x6EA7;
    struct Pending
    {
        UINT message;
        WPARAM wParam;
        LPARAM lParam;
    };
    std::mutex g_pendingLock;
    std::map<WPARAM, Pending> g_pending;
    WPARAM g_nextPending = 1;
    HHOOK g_messageHook = nullptr;
    HMODULE g_module = nullptr;

    bool isInput(UINT m)
    {
        // Not the characters (WM_CHAR..WM_SYSDEADCHAR): TranslateMessage makes them from the key messages, which
        // were injected ones here (real key messages are dropped before it sees them), and the game takes its
        // hotkeys from them.
        if (m >= WM_CHAR && m <= WM_SYSDEADCHAR)
        {
            return false;
        }
        return (m >= WM_KEYFIRST && m <= WM_KEYLAST) || (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) ||
               (m >= WM_NCMOUSEMOVE && m <= WM_NCXBUTTONDBLCLK) || m == WM_INPUT || m == WM_MOUSEHOVER ||
               m == WM_MOUSELEAVE;
    }

    // Screenshot requests from the automation thread (the menus): the message hook runs them on the window's
    // thread, which is the one that renders and flips there.
    constexpr UINT kCapture = WM_APP + 0x3F39;
    struct CaptureRequest
    {
        std::vector<uint8_t> bgr;       // out: 24 bits per pixel, top-down rows of width * 3 bytes
        int width = 0, height = 0;
        const char* error = nullptr;
        bool done = false;
    };
    bool captureBackBuffer(CaptureRequest& r);
    std::mutex g_captureLock;
    std::condition_variable g_captureDone;
    CaptureRequest* g_captureRequest = nullptr;
    DWORD g_driverThread = 0;

    LRESULT CALLBACK messageHook(int code, WPARAM wParam, LPARAM lParam)
    {
        if (code == HC_ACTION && lParam)
        {
            MSG* msg = reinterpret_cast<MSG*>(lParam);
            if (msg->message == kCapture && wParam == PM_REMOVE)
            {
                std::lock_guard guard(g_captureLock);
                if (g_captureRequest && !g_captureRequest->done)
                {
                    captureBackBuffer(*g_captureRequest);
                    g_captureRequest->done = true;
                    g_captureDone.notify_all();
                }
                msg->message = WM_NULL;
            }
            else if (msg->message == kInjected && msg->lParam == kInjectedTag)
            {
                std::lock_guard guard(g_pendingLock);
                const auto it = g_pending.find(msg->wParam);
                if (it == g_pending.end())
                {
                    msg->message = WM_NULL;
                }
                else
                {
                    const Pending p = it->second;
                    if (wParam == PM_REMOVE)
                    {
                        g_pending.erase(it);
                    }
                    msg->message = p.message;
                    msg->wParam = p.wParam;
                    msg->lParam = p.lParam;
                }
            }
            else if (isolated() && isInput(msg->message))
            {
                msg->message = WM_NULL;     // the player's own input, meant for another window
            }
        }
        return CallNextHookEx(g_messageHook, code, wParam, lParam);
    }

    void post(HWND w, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (!w)
        {
            return;
        }
        if (!g_messageHook)
        {
            PostMessageW(w, message, wParam, lParam);
            return;
        }
        WPARAM id = 0;
        {
            std::lock_guard guard(g_pendingLock);
            id = g_nextPending++;
            g_pending[id] = {message, wParam, lParam};
        }
        PostMessageW(w, kInjected, id, kInjectedTag);
    }

    bool safeCopy(void* dst, const void* src, size_t size)
    {
        __try
        {
            std::memcpy(dst, src, size);
            return true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            return false;
        }
    }

    template <class T>
    bool readAt(uintptr_t base, uintptr_t offset, T& out)
    {
        return base && safeCopy(&out, reinterpret_cast<const void*>(base + offset), sizeof(T));
    }

    template <class T>
    uintptr_t global(const Sacred::Global<T*>& g)
    {
        T* p = nullptr;
        return g && safeCopy(&p, g.get(), sizeof(p)) ? reinterpret_cast<uintptr_t>(p) : 0;
    }

    uintptr_t callMouseInstance()
    {
        __try
        {
            return reinterpret_cast<uintptr_t>(Addr::cMouse_instance());
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    POINT screenCursor()
    {
        if (HWND w = Automation::window())
        {
            if (!g_cursorSet)
            {
                RECT r{};
                if (GetClientRect(w, &r))
                {
                    g_cursor = {(r.right - r.left) / 2, (r.bottom - r.top) / 2};   // the middle until automation moves it
                    g_cursorSet = true;
                }
            }
        }
        POINT p = g_cursor;
        if (HWND w = Automation::window())
        {
            ClientToScreen(w, &p);
        }
        return p;
    }

    // GetCursorPos of the game: automation's cursor (isolated always; shared until the real mouse moves).
    BOOL WINAPI hookGetCursorPos(LPPOINT pt)
    {
        if (isolated())
        {
            if (!pt)
            {
                return FALSE;
            }
            std::lock_guard guard(g_inputLock);
            *pt = screenCursor();
            return TRUE;
        }
        const BOOL ok = g_origGetCursorPos(pt);
        std::lock_guard guard(g_inputLock);
        if (!g_virtualCursor || !pt)
        {
            return ok;
        }
        if (ok && (pt->x != g_realAtSwitch.x || pt->y != g_realAtSwitch.y))
        {
            g_virtualCursor = false;    // the player moved the mouse: theirs again
            return ok;
        }
        *pt = screenCursor();
        return TRUE;
    }

    // The game moves the cursor to keep it on its world spot: only automation's cursor moves while isolated.
    BOOL WINAPI hookSetCursorPos(int x, int y)
    {
        if (!isolated())
        {
            return g_origSetCursorPos(x, y);
        }
        POINT p{x, y};
        if (HWND w = Automation::window())
        {
            ScreenToClient(w, &p);
        }
        std::lock_guard guard(g_inputLock);
        g_cursor = p;
        g_cursorSet = true;
        return TRUE;
    }

    BOOL WINAPI hookClipCursor(const RECT* r)
    {
        return isolated() ? TRUE : g_origClipCursor(r);
    }

    SHORT heldState(int vk)
    {
        std::lock_guard guard(g_inputLock);
        return g_held.count(vk) ? static_cast<SHORT>(0x8000) : 0;
    }

    SHORT WINAPI hookGetAsyncKeyState(int vk)
    {
        if (isolated())
        {
            return heldState(vk);
        }
        return static_cast<SHORT>(g_origGetAsyncKeyState(vk) | heldState(vk));
    }

    SHORT WINAPI hookGetKeyState(int vk)
    {
        if (isolated())
        {
            return heldState(vk);
        }
        return static_cast<SHORT>(g_origGetKeyState(vk) | heldState(vk));
    }

    BOOL WINAPI hookGetKeyboardState(PBYTE keys)
    {
        if (!keys)
        {
            return FALSE;
        }
        BOOL ok = TRUE;
        if (isolated())
        {
            std::memset(keys, 0, 256);
        }
        else
        {
            ok = g_origGetKeyboardState(keys);
        }
        std::lock_guard guard(g_inputLock);
        for (int vk : g_held)
        {
            keys[vk & 0xFF] |= 0x80;
        }
        return ok;
    }

    // Focus and activation: the game asks to be in front (00811440 loops on SetForegroundWindow / SetFocus until
    // GetForegroundWindow / GetFocus answer with its window, or times out). Isolated, it never takes the foreground
    // or the focus for real: the window it asked for (else its first top-level window) is the "virtual foreground"
    // and every query answers with it. It cannot rely on g_pApp: the loop runs while the window is being created,
    // before the application object has the handle.
    std::atomic<HWND> g_virtualForeground{nullptr};

    HWND virtualForeground()
    {
        HWND v = g_virtualForeground.load();
        if (v && IsWindow(v))
        {
            return v;
        }
        return Automation::window();
    }

    bool ownWindow(HWND w)
    {
        DWORD process = 0;
        return w && IsWindow(w) && GetWindowThreadProcessId(w, &process) && process == GetCurrentProcessId();
    }

    void setVirtualForeground(HWND w)
    {
        if (ownWindow(w))
        {
            g_virtualForeground = GetAncestor(w, GA_ROOT);
        }
    }

    HWND WINAPI hookGetForegroundWindow()
    {
        HWND w = isolated() ? virtualForeground() : nullptr;
        return w ? w : g_origGetForegroundWindow();
    }

    HWND WINAPI hookGetFocus()
    {
        HWND w = isolated() ? virtualForeground() : nullptr;
        return w ? w : g_origGetFocus();
    }

    HWND WINAPI hookGetActiveWindow()
    {
        HWND w = isolated() ? virtualForeground() : nullptr;
        return w ? w : g_origGetActiveWindow();
    }

    HWND WINAPI hookSetFocus(HWND w)
    {
        if (!isolated())
        {
            return g_origSetFocus(w);
        }
        const HWND previous = virtualForeground();
        setVirtualForeground(w);
        return previous;
    }

    HWND WINAPI hookSetActiveWindow(HWND w)
    {
        if (!isolated())
        {
            return g_origSetActiveWindow(w);
        }
        const HWND previous = virtualForeground();
        setVirtualForeground(w);
        return previous;
    }

    HWND WINAPI hookSetCapture(HWND w)
    {
        return isolated() ? nullptr : g_origSetCapture(w);
    }

    BOOL WINAPI hookSetForegroundWindow(HWND w)
    {
        if (!isolated())
        {
            return g_origSetForegroundWindow(w);
        }
        setVirtualForeground(w);
        return TRUE;
    }

    BOOL WINAPI hookBringWindowToTop(HWND w)
    {
        if (!isolated())
        {
            return g_origBringWindowToTop(w);
        }
        setVirtualForeground(w);
        return TRUE;
    }

    BOOL WINAPI hookShowWindow(HWND w, int cmd)
    {
        if (isolated())
        {
            switch (cmd)
            {
            case SW_SHOWNORMAL: case SW_RESTORE: case SW_SHOWDEFAULT: cmd = SW_SHOWNOACTIVATE; break;
            case SW_SHOW: case SW_SHOWMAXIMIZED: cmd = SW_SHOWNA; break;
            case SW_SHOWMINIMIZED: case SW_MINIMIZE: cmd = SW_SHOWMINNOACTIVE; break;
            default: break;
            }
        }
        return g_origShowWindow(w, cmd);
    }

    BOOL WINAPI hookSetWindowPos(HWND w, HWND after, int x, int y, int cx, int cy, UINT flags)
    {
        if (isolated())
        {
            flags |= SWP_NOACTIVATE;
            if (after == HWND_TOPMOST || after == HWND_TOP)
            {
                flags |= SWP_NOZORDER;      // not over the player's windows
            }
        }
        return g_origSetWindowPos(w, after, x, y, cx, cy, flags);
    }

    HWND WINAPI hookCreateWindowExA(DWORD exStyle, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h,
                                    HWND parent, HMENU menu, HINSTANCE instance, LPVOID param)
    {
        if (isolated() && !(style & WS_CHILD))
        {
            exStyle = (exStyle | WS_EX_NOACTIVATE) & ~static_cast<DWORD>(WS_EX_TOPMOST);
        }
        // The title tells the runs apart in the task bar and the window list.
        std::string titled;
        if (!(style & WS_CHILD) && name && *name && !g_options.instance.empty())
        {
            titled = std::string(name) + " [gear:" + g_options.instance + "]";
            name = titled.c_str();
        }
        const HWND created =
            g_origCreateWindowExA(exStyle, cls, name, style, x, y, w, h, parent, menu, instance, param);
        if (created && isolated() && !(style & WS_CHILD))
        {
            HWND v = g_virtualForeground.load();
            if (!v || !IsWindow(v))
            {
                g_virtualForeground = created;      // the first top-level window is "in front"
            }
        }
        return created;
    }

    HHOOK WINAPI hookSetWindowsHookExA(int id, HOOKPROC proc, HINSTANCE module, DWORD thread)
    {
        if (isolated() && (id == WH_KEYBOARD_LL || id == WH_MOUSE_LL))
        {
            LOG("Automation: the game's system-wide {} hook is not installed (isolated input)",
                id == WH_KEYBOARD_LL ? "keyboard" : "mouse");
            return kFakeHook;
        }
        return g_origSetWindowsHookExA(id, proc, module, thread);
    }

    BOOL WINAPI hookUnhookWindowsHookEx(HHOOK h)
    {
        return h == kFakeHook ? TRUE : g_origUnhookWindowsHookEx(h);
    }

    // Replaces sacred.exe's import `name` with `hook`, keeping the original in `orig` (left null if the game does
    // not import it).
    template <class Fn>
    void hookImport(Fn& orig, const char* name, Fn hook)
    {
        orig = reinterpret_cast<Fn>(Patch::iat("USER32.dll", name, reinterpret_cast<void*>(hook)));
    }

    // While isolated the game window never becomes active; this window procedure makes the game believe it is,
    // so it neither pauses nor mutes when the player works in other windows.
    constexpr UINT kActivate = WM_APP + 0x3F38;
    WNDPROC g_origWndProc = nullptr;

    LRESULT CALLBACK windowProc(HWND w, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (isolated())
        {
            switch (message)
            {
            case kActivate:
                CallWindowProcA(g_origWndProc, w, WM_ACTIVATEAPP, TRUE, 0);
                CallWindowProcA(g_origWndProc, w, WM_ACTIVATE, WA_ACTIVE, 0);
                CallWindowProcA(g_origWndProc, w, WM_SETFOCUS, 0, 0);
                return 0;
            case WM_ACTIVATEAPP:
                wParam = TRUE;
                break;
            case WM_ACTIVATE:
                wParam = MAKEWPARAM(WA_ACTIVE, HIWORD(wParam));
                break;
            case WM_KILLFOCUS:
            case WM_CAPTURECHANGED:
                return 0;
            case WM_MOUSEACTIVATE:
                return MA_NOACTIVATE;
            default:
                break;
            }
        }
        return CallWindowProcA(g_origWndProc, w, message, wParam, lParam);
    }

    // The window exists: the message hook on its thread, and no activation for a window made before Gear loaded.
    void attachWindow(HWND w)
    {
        if (g_messageHook || !w)
        {
            return;
        }
        const DWORD thread = GetWindowThreadProcessId(w, nullptr);
        g_messageHook = SetWindowsHookExW(WH_GETMESSAGE, messageHook, g_module, thread);
        if (!g_messageHook)
        {
            LOG("Automation: cannot hook the window's messages ({}): input goes straight to the window", GetLastError());
            return;
        }
        if (isolated())
        {
            SetWindowLongW(w, GWL_EXSTYLE, (GetWindowLongW(w, GWL_EXSTYLE) | WS_EX_NOACTIVATE) & ~WS_EX_TOPMOST);
            g_origWndProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrA(w, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&windowProc)));
            if (g_origWndProc)
            {
                PostMessageA(w, kActivate, 0, 0);     // runs on the window's thread
            }
        }
        LOG("Automation: attached to the game window{}", isolated() ? " (isolated: the player's input stays theirs)" : "");
    }

    // The game replaces its window procedure when a game starts, which drops ours: it would then see the real
    // (never active) window state and ignore input. Put ours back over the game's new one (only over the game's
    // own: a procedure of another module may already call ours).
    void reattachWindowProc()
    {
        HWND w = Automation::window();
        if (!isolated() || !w || !g_origWndProc)
        {
            return;
        }
        const auto current = reinterpret_cast<WNDPROC>(GetWindowLongPtrA(w, GWLP_WNDPROC));
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        const auto addr = reinterpret_cast<uintptr_t>(current);
        if (current && current != &windowProc && addr >= base && addr < base + nt->OptionalHeader.SizeOfImage)
        {
            g_origWndProc = current;
            SetWindowLongPtrA(w, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&windowProc));
            PostMessageA(w, kActivate, 0, 0);
            LOG("Automation: the game replaced its window procedure; ours is back in front of it");
        }
    }

    LPARAM cursorParam()
    {
        return MAKELPARAM(g_cursor.x, g_cursor.y);
    }

    WPARAM buttonState()
    {
        WPARAM w = 0;
        if (g_held.count(VK_LBUTTON)) w |= MK_LBUTTON;
        if (g_held.count(VK_RBUTTON)) w |= MK_RBUTTON;
        if (g_held.count(VK_MBUTTON)) w |= MK_MBUTTON;
        if (g_held.count(VK_SHIFT)) w |= MK_SHIFT;
        if (g_held.count(VK_CONTROL)) w |= MK_CONTROL;
        return w;
    }

    std::vector<std::string> split(const std::string& s)
    {
        std::vector<std::string> out;
        size_t start = 0;
        while (start <= s.size())
        {
            const size_t end = s.find(',', start);
            const std::string item = s.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!item.empty())
            {
                out.push_back(item);
            }
            if (end == std::string::npos)
            {
                break;
            }
            start = end + 1;
        }
        return out;
    }

    std::string envVar(const char* name)
    {
        char value[512] = {};
        const DWORD n = GetEnvironmentVariableA(name, value, sizeof(value));
        return n && n < sizeof(value) ? std::string(value, n) : std::string();
    }

    void parse()
    {
        g_parsed = true;
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        std::wstring load, mods, timeoutText;
        bool enabled = envVar("SACREDGEAR_AUTO") == "1";
        g_options.sharedInput = envVar("SACREDGEAR_SHARED_INPUT") == "1";
        for (int i = 1; argv && i < argc; ++i)
        {
            const std::wstring a = argv[i];
            if (_wcsicmp(a.c_str(), L"-gear-auto") == 0)
            {
                enabled = true;
            }
            else if (_wcsnicmp(a.c_str(), L"-gear-load=", 11) == 0)
            {
                load = a.substr(11);
            }
            else if (_wcsicmp(a.c_str(), L"-gear-shared-input") == 0)
            {
                g_options.sharedInput = true;
            }
            else if (_wcsnicmp(a.c_str(), L"-gear-timeout=", 14) == 0)
            {
                timeoutText = a.substr(14);
            }
            else if (_wcsnicmp(a.c_str(), L"-gear-mods=", 11) == 0)
            {
                mods = a.substr(11);
            }
        }
        if (argv)
        {
            LocalFree(argv);
        }
        std::string loadText;
        for (const wchar_t c : load)
        {
            loadText.push_back(static_cast<char>(c));   // slot numbers are ASCII
        }
        std::string modsText;   // ids are ASCII
        for (const wchar_t c : mods)
        {
            modsText.push_back(static_cast<char>(c));
        }
        if (loadText.empty())
        {
            loadText = envVar("SACREDGEAR_LOAD");
        }
        if (modsText.empty())
        {
            modsText = envVar("SACREDGEAR_MODS");
        }
        g_options.enabled = enabled;
        g_options.instance = Instance::name();
        {
            std::string t;
            for (const wchar_t c : timeoutText)
            {
                t.push_back(static_cast<char>(c));
            }
            if (t.empty())
            {
                t = envVar("SACREDGEAR_TIMEOUT");
            }
            char* end = nullptr;
            const long v = t.empty() ? -1 : std::strtol(t.c_str(), &end, 10);
            if (!t.empty() && end && *end == '\0' && v >= 0 && v <= 86400)
            {
                g_options.timeoutSeconds = static_cast<int>(v);
            }
        }
        if (enabled && !loadText.empty())
        {
            char* end = nullptr;
            const long slot = std::strtol(loadText.c_str(), &end, 10);
            if (end && *end == '\0' && slot >= 0 && slot < 100)
            {
                g_options.loadSlot = static_cast<int>(slot);
            }
        }
        if (enabled)
        {
            g_options.mods = split(modsText);
        }
    }

    // The main menu is up and quiet: menus on, no game, no message box, no cinematic.
    bool menuReady()
    {
        const Automation::UiState s = Automation::uiState();
        return s.available && s.menus && !s.inGame && !s.dialog && !s.cinematic && Automation::window();
    }

    // Ends a run that took too long, or whose game stopped answering (distinct exit code 3).
    void watchdog(ULONGLONG started, ULONGLONG now, ULONGLONG& stuckSince)
    {
        if (g_options.timeoutSeconds > 0 && now - started > static_cast<ULONGLONG>(g_options.timeoutSeconds) * 1000)
        {
            LOG("Automation: watchdog timeout ({} s)", g_options.timeoutSeconds);
            Automation::quit(3, "watchdog");
        }
        bool stuck = false;
        if (HWND w = Automation::window())
        {
            DWORD_PTR result = 0;
            stuck = !SendMessageTimeoutW(w, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &result);
        }
        if (!stuck && Automation::uiState().inGame && LuaHost::lastFrameTime() && now - LuaHost::lastFrameTime() > 10000)
        {
            stuck = true;       // in a game, but no frame
        }
        if (!stuck)
        {
            stuckSince = 0;
        }
        else if (!stuckSince)
        {
            stuckSince = now;
        }
        else if (now - stuckSince > 60000)
        {
            LOG("Automation: watchdog timeout (the game stopped responding for 60 s)");
            Automation::quit(3, "watchdog");
        }
    }

    void driver()
    {
        g_driverThread = GetCurrentThreadId();
        const ULONGLONG started = GetTickCount64();
        ULONGLONG stuckSince = 0, lastCheck = 0;
        ULONGLONG readySince = 0;
        bool loadDone = g_options.loadSlot < 0;
        for (;;)
        {
            Sleep(50);
            if (!g_messageHook)
            {
                attachWindow(Automation::window());
            }
            else
            {
                reattachWindowProc();
            }
            const ULONGLONG now = GetTickCount64();
            if (now - lastCheck >= 1000)
            {
                lastCheck = now;
                watchdog(started, now, stuckSince);
            }
            if (now - LuaHost::lastFrameTime() > 250)
            {
                LuaHost::idle();
            }
            {
                static bool centered = false;
                if (!centered && isolated() && Automation::uiState().inGame)
                {
                    centered = true;
                    int cw = 0, ch = 0;
                    Automation::clientSize(cw, ch);
                    Automation::mouseMove(cw / 2, ch / 2);
                }
            }
            if (!loadDone)
            {
                if (!menuReady())
                {
                    readySince = 0;
                }
                else if (!readySince)
                {
                    readySince = now;
                }
                else if (now - readySince > 1500)
                {
                    const char* error = nullptr;
                    if (Automation::loadSave(g_options.loadSlot, error))
                    {
                        LOG("Automation: loading savegame {}", g_options.loadSlot);
                    }
                    else
                    {
                        LOG("Automation: cannot load savegame {}: {}", g_options.loadSlot, error);
                    }
                    loadDone = true;
                }
            }
        }
    }
}

const Automation::Options& Automation::options()
{
    if (!g_parsed)
    {
        parse();
    }
    return g_options;
}

namespace
{
    // The game's single-instance check is CreateMutexA("SACRED_INSTANCE") in its start-up (0x817B30; "already
    // running" when the name exists). The name gets this run's suffix, so parallel runs, and a player's own game,
    // never meet. Every named mutex gets it: that is the only one the game makes. The mutexes that the engine
    // creates without a name stay as they are.
    using CreateMutexAFn = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
    using OpenMutexAFn = HANDLE(WINAPI*)(DWORD, BOOL, LPCSTR);
    using ExitProcessFn = void(WINAPI*)(UINT);
    CreateMutexAFn g_origCreateMutexA = nullptr;
    OpenMutexAFn g_origOpenMutexA = nullptr;
    ExitProcessFn g_origExitProcess = nullptr;

    std::string instanceMutexName(LPCSTR name)
    {
        return std::string(name) + "-gear-" + g_options.instance;
    }

    HANDLE WINAPI hookCreateMutexA(LPSECURITY_ATTRIBUTES attributes, BOOL owner, LPCSTR name)
    {
        if (!name)
        {
            return g_origCreateMutexA(attributes, owner, name);
        }
        const std::string renamed = instanceMutexName(name);
        LOG("Automation: mutex {} is {}", name, renamed);
        return g_origCreateMutexA(attributes, owner, renamed.c_str());
    }

    HANDLE WINAPI hookOpenMutexA(DWORD access, BOOL inherit, LPCSTR name)
    {
        if (!name)
        {
            return g_origOpenMutexA(access, inherit, name);
        }
        const std::string renamed = instanceMutexName(name);
        return g_origOpenMutexA(access, inherit, renamed.c_str());
    }

    // run.json: written at the start, rewritten when the process ends.
    std::wstring g_runFile;
    std::string g_runStarted, g_runCommandLine;
    std::atomic<bool> g_runEnded{false};

    std::string utf8(const wchar_t* s)
    {
        const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
        std::string out(n > 1 ? n - 1 : 0, '\0');
        if (n > 1)
        {
            WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
        }
        return out;
    }

    std::string jsonString(const std::string& s)
    {
        std::string out = "\"";
        for (const char c : s)
        {
            switch (c)
            {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                }
                else
                {
                    out += c;
                }
            }
        }
        return out + "\"";
    }

    std::string utcNow()
    {
        SYSTEMTIME t;
        GetSystemTime(&t);
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", t.wYear, t.wMonth, t.wDay, t.wHour,
                      t.wMinute, t.wSecond, t.wMilliseconds);
        return buf;
    }

    void writeRun(bool ended, int exitCode, const char* reason)
    {
        if (g_runFile.empty())
        {
            return;
        }
        std::string json = "{\n  \"instance\": " + jsonString(g_options.instance) +
                           ",\n  \"pid\": " + std::to_string(GetCurrentProcessId()) +
                           ",\n  \"command_line\": " + jsonString(g_runCommandLine) +
                           ",\n  \"started\": " + jsonString(g_runStarted);
        if (ended)
        {
            json += ",\n  \"exit_code\": " + std::to_string(exitCode) + ",\n  \"ended\": " + jsonString(utcNow()) +
                    ",\n  \"reason\": " + jsonString(reason);
        }
        json += "\n}\n";
        HANDLE f = CreateFileW(g_runFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteFile(f, json.data(), static_cast<DWORD>(json.size()), &written, nullptr);
            CloseHandle(f);
        }
    }

    // The first end wins (a quit() then ExitProcess is one end, with quit's reason).
    void endRun(int code, const char* reason)
    {
        if (!g_runEnded.exchange(true))
        {
            writeRun(true, code, reason);
        }
    }

    void WINAPI hookExitProcess(UINT code)
    {
        LOG("Automation: the game exits with code {}", code);
        endRun(static_cast<int>(code), "exit");
        g_origExitProcess(code);
    }
}

void Automation::installInstance(const std::wstring& gameDir)
{
    if (!options().enabled)
    {
        return;
    }
    g_runFile = Instance::directory(gameDir) + L"\\run.json";
    g_runStarted = utcNow();
    g_runCommandLine = utf8(GetCommandLineW());
    writeRun(false, 0, nullptr);
    g_origCreateMutexA = reinterpret_cast<CreateMutexAFn>(
        Patch::iat("KERNEL32.dll", "CreateMutexA", reinterpret_cast<void*>(&hookCreateMutexA)));
    g_origOpenMutexA = reinterpret_cast<OpenMutexAFn>(
        Patch::iat("KERNEL32.dll", "OpenMutexA", reinterpret_cast<void*>(&hookOpenMutexA)));
    g_origExitProcess = reinterpret_cast<ExitProcessFn>(
        Patch::iat("KERNEL32.dll", "ExitProcess", reinterpret_cast<void*>(&hookExitProcess)));
    LOG("Automation instance {} (pid {}): {}", g_options.instance, GetCurrentProcessId(),
        g_origCreateMutexA ? "the game's mutex is named after it" : "the game does not import CreateMutexA");
}

void Automation::install(const std::wstring&)
{
    if (!options().enabled)
    {
        return;
    }
    LOG("Automation on, instance {} ({} input){}{}", g_options.instance, isolated() ? "isolated" : "shared", g_options.loadSlot >= 0 ? ", loading savegame " + std::to_string(g_options.loadSlot) : "",
        g_options.mods.empty() ? "" : ", only some mods");
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&messageHook), &g_module);
    hookImport(g_origGetCursorPos, "GetCursorPos", &hookGetCursorPos);
    hookImport(g_origGetAsyncKeyState, "GetAsyncKeyState", &hookGetAsyncKeyState);
    if (!g_origGetCursorPos || !g_origGetAsyncKeyState)
    {
        LOG("Automation: the cursor and key imports were not found; simulated input may not reach the game");
    }
    // The rest only where the game imports them.
    hookImport(g_origGetKeyState, "GetKeyState", &hookGetKeyState);
    hookImport(g_origGetKeyboardState, "GetKeyboardState", &hookGetKeyboardState);
    hookImport(g_origCreateWindowExA, "CreateWindowExA", &hookCreateWindowExA);
    if (isolated())
    {
        hookImport(g_origSetCursorPos, "SetCursorPos", &hookSetCursorPos);
        hookImport(g_origClipCursor, "ClipCursor", &hookClipCursor);
        hookImport(g_origGetForegroundWindow, "GetForegroundWindow", &hookGetForegroundWindow);
        hookImport(g_origGetFocus, "GetFocus", &hookGetFocus);
        hookImport(g_origSetFocus, "SetFocus", &hookSetFocus);
        hookImport(g_origSetCapture, "SetCapture", &hookSetCapture);
        hookImport(g_origSetForegroundWindow, "SetForegroundWindow", &hookSetForegroundWindow);
        hookImport(g_origBringWindowToTop, "BringWindowToTop", &hookBringWindowToTop);
        hookImport(g_origGetActiveWindow, "GetActiveWindow", &hookGetActiveWindow);
        hookImport(g_origSetActiveWindow, "SetActiveWindow", &hookSetActiveWindow);
        hookImport(g_origShowWindow, "ShowWindow", &hookShowWindow);
        hookImport(g_origSetWindowPos, "SetWindowPos", &hookSetWindowPos);
        hookImport(g_origSetWindowsHookExA, "SetWindowsHookExA", &hookSetWindowsHookExA);
        hookImport(g_origUnhookWindowsHookEx, "UnhookWindowsHookEx", &hookUnhookWindowsHookEx);
        // A hook the game already installed (Gear injected late): cannot be taken back, only noted.
    }
    std::thread(driver).detach();
}

HWND Automation::window()
{
    HWND w = nullptr;
    const uintptr_t app = global(Addr::g_pApp);
    if (!app || !readAt(app, kAppWindow, w) || !w || !IsWindow(w))
    {
        return nullptr;
    }
    return w;
}

void Automation::clientSize(int& width, int& height)
{
    RECT r{};
    HWND w = window();
    width = height = 0;
    if (w && GetClientRect(w, &r))
    {
        width = r.right - r.left;
        height = r.bottom - r.top;
    }
}

Automation::UiState Automation::uiState()
{
    UiState s;
    const uintptr_t ui = global(Addr::g_pUiManager);
    uint32_t flags = 0;
    if (!ui || !readAt(ui, kUiFlags, flags))
    {
        return s;
    }
    s.available = true;
    s.menus = flags & kUiInMenus;
    s.inGame = flags & kUiInGame;
    s.cinematic = flags & kUiCinematic;
    s.help = flags & kUiHelp;
    uintptr_t dialog = 0;
    uint32_t dialogFlags = 0;
    s.dialog = readAt(ui, kUiDialog, dialog) && dialog && readAt(dialog, kCtlFlags, dialogFlags) &&
               (dialogFlags & kCtlVisible);
    if (Addr::cMouse_instance)
    {
        const uintptr_t mouse = callMouseInstance();
        int32_t x = 0, y = 0;
        if (mouse && readAt(mouse, kMouseX, x) && readAt(mouse, kMouseY, y))
        {
            s.mouseX = x;
            s.mouseY = y;
        }
    }
    return s;
}

namespace
{
    // Adds `control` (if visible) and, for a window, its visible children (depth levels below it).
    void addNode(std::vector<Automation::UiNode>& out, uintptr_t control, const char* group, int index, int parent,
                 int baseX, int baseY, int depth)
    {
        uintptr_t vtable = 0;
        uint32_t flags = 0;
        int32_t x = 0, y = 0;
        int16_t w = 0, h = 0;
        if (!control || out.size() >= 2048 || !readAt(control, 0, vtable) || !readAt(control, kCtlFlags, flags) ||
            !(flags & kCtlVisible) || !readAt(control, kCtlX, x) || !readAt(control, kCtlY, y) ||
            !readAt(control, kCtlW, w) || !readAt(control, kCtlH, h))
        {
            return;
        }
        Automation::UiNode n;
        n.group = group;
        n.index = index;
        n.parent = parent;
        n.vtable = static_cast<uint32_t>(vtable);
        n.flags = flags;
        n.x = baseX + x;
        n.y = baseY + y;
        n.width = w;
        n.height = h;
        const int self = static_cast<int>(out.size());
        out.push_back(n);
        if (depth <= 0)
        {
            return;
        }
        // Children: only of windows. A control's vector slot holds garbage for plain controls, so a child list is
        // taken only if it looks like a vector (ordered, bounded) of pointers.
        uintptr_t range[3] = {};
        if (!safeCopy(range, reinterpret_cast<const void*>(control + kCtlChildren), sizeof(range)) || !range[0] ||
            range[1] < range[0] || range[2] < range[1] || (range[1] - range[0]) % 4 || (range[1] - range[0]) / 4 > 256)
        {
            return;
        }
        const int count = static_cast<int>((range[1] - range[0]) / 4);
        for (int i = 0; i < count; ++i)
        {
            uintptr_t child = 0;
            if (readAt(range[0], static_cast<uintptr_t>(i) * 4, child) && child != control)
            {
                addNode(out, child, "child", i, self, n.x, n.y, depth - 1);
            }
        }
    }
}

std::vector<Automation::UiNode> Automation::uiTree()
{
    std::vector<UiNode> out;
    const uintptr_t ui = global(Addr::g_pUiManager);
    uint32_t flags = 0;
    if (!ui || !readAt(ui, kUiFlags, flags))
    {
        return out;
    }
    if (flags & kUiInMenus)
    {
        for (int i = 0; i < kUiMenuCount; ++i)
        {
            uintptr_t w = 0;
            if (readAt(ui, kUiMenus + i * 4, w))
            {
                addNode(out, w, "menu", i, -1, 0, 0, 2);
            }
        }
    }
    for (int i = 0; i < kUiWindowCount; ++i)
    {
        uintptr_t w = 0;
        if (readAt(ui, kUiWindows + i * 4, w))
        {
            addNode(out, w, "window", i, -1, 0, 0, 2);
        }
    }
    uintptr_t dialog = 0;
    if (readAt(ui, kUiDialog, dialog))
    {
        addNode(out, dialog, "dialog", 0, -1, 0, 0, 2);
    }
    return out;
}

void Automation::mouseMove(int x, int y)
{
    HWND w = window();
    {
        std::lock_guard guard(g_inputLock);
        g_cursor = {x, y};
        g_cursorSet = true;
        g_virtualCursor = true;
        if (g_origGetCursorPos && !isolated())
        {
            g_origGetCursorPos(&g_realAtSwitch);
        }
    }
    WPARAM state;
    LPARAM pos;
    {
        std::lock_guard guard(g_inputLock);
        state = buttonState();
        pos = cursorParam();
    }
    post(w, WM_MOUSEMOVE, state, pos);
}

void Automation::button(int vk, bool down)
{
    UINT message = 0;
    switch (vk)
    {
    case VK_LBUTTON: message = down ? WM_LBUTTONDOWN : WM_LBUTTONUP; break;
    case VK_RBUTTON: message = down ? WM_RBUTTONDOWN : WM_RBUTTONUP; break;
    case VK_MBUTTON: message = down ? WM_MBUTTONDOWN : WM_MBUTTONUP; break;
    default: return;
    }
    HWND w = window();
    WPARAM state;
    LPARAM pos;
    {
        std::lock_guard guard(g_inputLock);
        if (down)
        {
            g_held.insert(vk);
        }
        else
        {
            g_held.erase(vk);
        }
        state = buttonState();
        pos = cursorParam();
    }
    post(w, message, state, pos);
}

void Automation::key(int vk, bool down)
{
    if (vk <= 0 || vk > 0xFE)
    {
        return;
    }
    HWND w = window();
    {
        std::lock_guard guard(g_inputLock);
        if (down)
        {
            g_held.insert(vk);
        }
        else
        {
            g_held.erase(vk);
        }
    }
    if (w)
    {
        const UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
        const LPARAM lp = 1 | (static_cast<LPARAM>(scan) << 16) | (down ? 0 : (3 << 30));
        post(w, down ? WM_KEYDOWN : WM_KEYUP, static_cast<WPARAM>(vk), lp);
    }
}

bool Automation::keyHeld(int vk)
{
    return heldState(vk) != 0;
}

bool Automation::isolatedInput()
{
    return isolated();
}

void Automation::character(wchar_t ch)
{
    post(window(), WM_CHAR, ch, 1);
}

void Automation::releaseAll()
{
    std::vector<int> held;
    {
        std::lock_guard guard(g_inputLock);
        held.assign(g_held.begin(), g_held.end());
    }
    for (int vk : held)
    {
        if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON)
        {
            button(vk, false);
        }
        else
        {
            key(vk, false);
        }
    }
}

bool Automation::loadSave(int slot, const char*& error)
{
    HWND w = window();
    const uintptr_t players = global(Addr::g_pNetPlayers);
    if (!w || !players)
    {
        error = "the game is not ready (no window or players object yet)";
        return false;
    }
    if (slot < 0 || slot > 99)
    {
        error = "the savegame number is 0..99";
        return false;
    }
    const UiState s = uiState();
    if (!s.menus || s.inGame)
    {
        error = "only from the menus";
        return false;
    }
    // As the savegame window does in single player (0071CC90 action 6): the local hero is id 1, then the message.
    const uint32_t one = 1;
    if (!safeCopy(reinterpret_cast<void*>(players + kPlayersLocalHero), &one, sizeof(one)))
    {
        error = "cannot write the players object";
        return false;
    }
    if (!PostMessageW(w, kLoadMessage, 1, slot))
    {
        error = "cannot post to the game window";
        return false;
    }
    return true;
}

namespace
{
    // Shift and width of the lowest set run of a colour mask.
    void maskShift(uint32_t mask, int& shift, int& bits)
    {
        shift = bits = 0;
        if (!mask)
        {
            return;
        }
        while (!((mask >> shift) & 1))
        {
            ++shift;
        }
        while (shift + bits < 32 && ((mask >> (shift + bits)) & 1))
        {
            ++bits;
        }
    }

    uint8_t scaleChannel(uint32_t v, int bits)
    {
        if (bits == 0)
        {
            return 0;
        }
        return bits >= 8 ? static_cast<uint8_t>(v >> (bits - 8)) : static_cast<uint8_t>((v * 255) / ((1u << bits) - 1));
    }

    // Locks `surface` for reading and converts it to 24-bit BGR rows (top-down) in `out` (width x height). 16 and
    // 32 bits per pixel, whatever the channel masks. Raw pointers only: the exception handler cannot sit in a
    // function with objects to unwind.
    const char* readSurface(IDirectDrawSurface7* surface, uint8_t* out, int width, int height)
    {
        __try
        {
            DDSURFACEDESC2 desc{};
            desc.dwSize = sizeof(desc);
            const HRESULT hr = surface->Lock(nullptr, &desc, DDLOCK_READONLY | DDLOCK_WAIT, nullptr);
            if (FAILED(hr) || !desc.lpSurface)
            {
                return "cannot lock the back buffer";
            }
            const char* error = nullptr;
            const int bpp = static_cast<int>(desc.ddpfPixelFormat.dwRGBBitCount);
            const int w = std::min<int>(width, static_cast<int>(desc.dwWidth));
            const int h = std::min<int>(height, static_cast<int>(desc.dwHeight));
            if ((bpp != 16 && bpp != 32) || w <= 0 || h <= 0)
            {
                error = "unsupported back buffer format";
            }
            else
            {
                int rs, rb, gs, gb, bs, bb;
                maskShift(desc.ddpfPixelFormat.dwRBitMask, rs, rb);
                maskShift(desc.ddpfPixelFormat.dwGBitMask, gs, gb);
                maskShift(desc.ddpfPixelFormat.dwBBitMask, bs, bb);
                const uint8_t* base = static_cast<const uint8_t*>(desc.lpSurface);
                for (int y = 0; y < h; ++y)
                {
                    const uint8_t* row = base + static_cast<size_t>(y) * desc.lPitch;
                    uint8_t* dst = out + static_cast<size_t>(y) * width * 3;
                    for (int x = 0; x < w; ++x)
                    {
                        const uint32_t px = bpp == 32 ? reinterpret_cast<const uint32_t*>(row)[x]
                                                      : reinterpret_cast<const uint16_t*>(row)[x];
                        dst[x * 3 + 0] = scaleChannel((px >> bs) & ((1u << bb) - 1), bb);
                        dst[x * 3 + 1] = scaleChannel((px >> gs) & ((1u << gb) - 1), gb);
                        dst[x * 3 + 2] = scaleChannel((px >> rs) & ((1u << rb) - 1), rb);
                    }
                }
            }
            surface->Unlock(nullptr);
            return error;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return "exception while reading the back buffer";
        }
    }

    bool isBlank(const CaptureRequest& r)
    {
        for (size_t i = 0; i < r.bgr.size(); i += 61)
        {
            if (r.bgr[i])
            {
                return false;
            }
        }
        return true;
    }

    // The game's own back buffer (dxDriver7, the object g_pApp points to: +0x1C height, +0x20 width, +0xBC the
    // IDirectDrawSurface7; SacredBild's ddraw9 reads render targets back in its Lock). On the thread that renders.
    bool captureBackBuffer(CaptureRequest& r)
    {
        const uintptr_t driver = global(Addr::g_pApp);
        uint16_t height = 0, width = 0;
        uintptr_t back = 0;
        if (!driver || !readAt(driver, 0x1C, height) || !readAt(driver, 0x20, width) || !readAt(driver, 0xBC, back) ||
            !back || !width || !height)
        {
            r.error = "the game has no back buffer yet";
            return false;
        }
        r.width = width;
        r.height = height;
        r.bgr.assign(static_cast<size_t>(width) * height * 3, 0);
        r.error = readSurface(reinterpret_cast<IDirectDrawSurface7*>(back), r.bgr.data(), width, height);
        const auto blank = [&r] { return isBlank(r); };
        // The menus draw straight to the primary surface (+0xB8), not the back buffer.
        uintptr_t primary = 0;
        if (!r.error && blank() && readAt(driver, 0xB8, primary) && primary)
        {
            r.error = readSurface(reinterpret_cast<IDirectDrawSurface7*>(primary), r.bgr.data(), width, height);
        }
        return r.error == nullptr;
    }
}

bool Automation::screenshot(const std::wstring& path, const char*& error)
{
    CaptureRequest request;
    if (g_driverThread && GetCurrentThreadId() == g_driverThread)
    {
        // The menus: the window's thread renders; ask it between two frames.
        HWND w = window();
        if (!w)
        {
            error = "no game window";
            return false;
        }
        // A menu frame clears the back buffer before it draws: between two frames it may be empty (the Direct3D 9
        // backend does not keep it), so ask again a few times until something is in it.
        for (int attempt = 0; attempt < 40; ++attempt)
        {
            request = CaptureRequest();
            std::unique_lock lock(g_captureLock);
            g_captureRequest = &request;
            PostMessageW(w, kCapture, 0, 0);
            const bool answered = g_captureDone.wait_for(lock, std::chrono::seconds(10), [&] { return request.done; });
            g_captureRequest = nullptr;
            if (!answered)
            {
                error = "the game did not render in time";
                return false;
            }
            if (request.error || !isBlank(request))
            {
                break;
            }
            lock.unlock();
            Sleep(25);
        }
    }
    else
    {
        captureBackBuffer(request);     // in game: the engine's thread, which is the one that flips
    }
    if (request.error)
    {
        error = request.error;
        return false;
    }
    const int width = request.width, height = request.height;
    BITMAPINFOHEADER bi{};
    bi.biSize = sizeof(bi);
    bi.biWidth = width;
    bi.biHeight = height;       // bottom-up, as BMP files store it
    bi.biPlanes = 1;
    bi.biBitCount = 24;
    bi.biCompression = BI_RGB;
    const DWORD stride = (static_cast<DWORD>(width) * 3 + 3) & ~3u;
    std::vector<uint8_t> file(stride * static_cast<size_t>(height), 0);
    for (int y = 0; y < height; ++y)
    {
        std::memcpy(file.data() + static_cast<size_t>(height - 1 - y) * stride,
                    request.bgr.data() + static_cast<size_t>(y) * width * 3, static_cast<size_t>(width) * 3);
    }
    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(bi);
    fh.bfSize = fh.bfOffBits + static_cast<DWORD>(file.size());
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    const bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, &fh, sizeof(fh), &written, nullptr) &&
                    WriteFile(f, &bi, sizeof(bi), &written, nullptr) &&
                    WriteFile(f, file.data(), static_cast<DWORD>(file.size()), &written, nullptr);
    if (f != INVALID_HANDLE_VALUE)
    {
        CloseHandle(f);
    }
    if (!ok)
    {
        error = "cannot write the file";
    }
    return ok;
}

// For SacredBild: the window automation shows as the foreground one (isolated input), nullptr if the real
// foreground counts. Optional: neither DLL needs the other.
extern "C" __declspec(dllexport) HWND __cdecl SacredGear_IsolatedForeground()
{
    return isolated() ? virtualForeground() : nullptr;
}

void Automation::quit(int code, const char* reason)
{
    LOG("Automation: quitting with exit code {} ({})", code, reason);
    endRun(code, reason);
    // TerminateProcess: the game's own shutdown waits for its threads and windows, which a test run does not need.
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
    for (;;)
    {
        Sleep(INFINITE);
    }
}
