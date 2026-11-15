#include "automation.h"
#include "game/gear_addr.h"
#include "log.h"
#include "lua_host.h"
#include "patch.h"

#include <shellapi.h>

#include <atomic>
#include <cstring>
#include <cwchar>
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
    bool g_virtualCursor = false;   // GetCursorPos answers with g_cursor (client pixels) until the real mouse moves
    POINT g_cursor{};
    POINT g_realAtSwitch{};         // the real cursor when automation took over, to notice the player moving it
    std::set<int> g_held;

    using GetCursorPosFn = BOOL(WINAPI*)(LPPOINT);
    using GetAsyncKeyStateFn = SHORT(WINAPI*)(int);
    GetCursorPosFn g_origGetCursorPos = nullptr;
    GetAsyncKeyStateFn g_origGetAsyncKeyState = nullptr;

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

    // GetCursorPos of the game: the virtual cursor (in screen coordinates) while automation owns it.
    BOOL WINAPI hookGetCursorPos(LPPOINT pt)
    {
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
        POINT p = g_cursor;
        if (HWND w = Automation::window())
        {
            ClientToScreen(w, &p);
        }
        *pt = p;
        return TRUE;
    }

    SHORT WINAPI hookGetAsyncKeyState(int vk)
    {
        const SHORT state = g_origGetAsyncKeyState(vk);
        std::lock_guard guard(g_inputLock);
        return g_held.count(vk) ? static_cast<SHORT>(state | 0x8000) : state;
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
        std::wstring load, mods;
        bool enabled = envVar("SACREDGEAR_AUTO") == "1";
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
            else if (_wcsnicmp(a.c_str(), L"-gear-mods=", 11) == 0)
            {
                mods = a.substr(11);
            }
        }
        if (argv)
        {
            LocalFree(argv);
        }
        std::string loadText(load.begin(), load.end());
        std::string modsText(mods.begin(), mods.end());   // ids are ASCII
        if (loadText.empty())
        {
            loadText = envVar("SACREDGEAR_LOAD");
        }
        if (modsText.empty())
        {
            modsText = envVar("SACREDGEAR_MODS");
        }
        g_options.enabled = enabled;
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

    void driver()
    {
        ULONGLONG readySince = 0;
        bool loadDone = g_options.loadSlot < 0;
        for (;;)
        {
            Sleep(50);
            const ULONGLONG now = GetTickCount64();
            if (now - LuaHost::lastFrameTime() > 250)
            {
                LuaHost::idle();
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

void Automation::install(const std::wstring&)
{
    if (!options().enabled)
    {
        return;
    }
    LOG("Automation on{}{}", g_options.loadSlot >= 0 ? ", loading savegame " + std::to_string(g_options.loadSlot) : "",
        g_options.mods.empty() ? "" : ", only some mods");
    g_origGetCursorPos = reinterpret_cast<GetCursorPosFn>(
        Patch::iat("USER32.dll", "GetCursorPos", reinterpret_cast<void*>(&hookGetCursorPos)));
    g_origGetAsyncKeyState = reinterpret_cast<GetAsyncKeyStateFn>(
        Patch::iat("USER32.dll", "GetAsyncKeyState", reinterpret_cast<void*>(&hookGetAsyncKeyState)));
    if (!g_origGetCursorPos || !g_origGetAsyncKeyState)
    {
        LOG("Automation: input hooks not installed; simulated input may not reach the game");
        // Restore what did install, so the game calls the real functions.
        if (g_origGetCursorPos)
        {
            Patch::iat("USER32.dll", "GetCursorPos", reinterpret_cast<void*>(g_origGetCursorPos));
        }
        if (g_origGetAsyncKeyState)
        {
            Patch::iat("USER32.dll", "GetAsyncKeyState", reinterpret_cast<void*>(g_origGetAsyncKeyState));
        }
        g_origGetCursorPos = nullptr;
        g_origGetAsyncKeyState = nullptr;
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
        g_virtualCursor = true;
        if (g_origGetCursorPos)
        {
            g_origGetCursorPos(&g_realAtSwitch);
        }
    }
    if (w)
    {
        std::lock_guard guard(g_inputLock);
        PostMessageW(w, WM_MOUSEMOVE, buttonState(), cursorParam());
    }
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
    std::lock_guard guard(g_inputLock);
    if (down)
    {
        g_held.insert(vk);
    }
    else
    {
        g_held.erase(vk);
    }
    if (w)
    {
        PostMessageW(w, message, buttonState(), cursorParam());
    }
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
        PostMessageW(w, down ? WM_KEYDOWN : WM_KEYUP, static_cast<WPARAM>(vk), lp);
    }
}

void Automation::character(wchar_t ch)
{
    if (HWND w = window())
    {
        PostMessageW(w, WM_CHAR, ch, 1);
    }
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

bool Automation::screenshot(const std::wstring& path, const char*& error)
{
    HWND w = window();
    int width = 0, height = 0;
    clientSize(width, height);
    if (!w || width <= 0 || height <= 0)
    {
        error = "no game window";
        return false;
    }
    HDC screen = GetDC(w);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = height;     // bottom-up, as BMP files store it
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (mem && bmp)
    {
        HGDIOBJ old = SelectObject(mem, bmp);
        // PrintWindow with PW_RENDERFULLCONTENT (2) gets Direct3D content of a window in the background too;
        // BitBlt from the window's DC where it fails (older Windows).
        ok = PrintWindow(w, mem, PW_CLIENTONLY | 2) || BitBlt(mem, 0, 0, width, height, screen, 0, 0, SRCCOPY);
        GdiFlush();
        SelectObject(mem, old);
    }
    if (ok)
    {
        const DWORD stride = (static_cast<DWORD>(width) * 3 + 3) & ~3u;
        const DWORD size = stride * static_cast<DWORD>(height);
        BITMAPFILEHEADER fh{};
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
        fh.bfSize = fh.bfOffBits + size;
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written = 0;
        ok = f != INVALID_HANDLE_VALUE && WriteFile(f, &fh, sizeof(fh), &written, nullptr) &&
             WriteFile(f, &bi.bmiHeader, sizeof(bi.bmiHeader), &written, nullptr) &&
             WriteFile(f, bits, size, &written, nullptr);
        if (f != INVALID_HANDLE_VALUE)
        {
            CloseHandle(f);
        }
        if (!ok)
        {
            error = "cannot write the file";
        }
    }
    else
    {
        error = "cannot capture the window";
    }
    if (bmp)
    {
        DeleteObject(bmp);
    }
    if (mem)
    {
        DeleteDC(mem);
    }
    ReleaseDC(w, screen);
    return ok;
}

void Automation::quit(int code)
{
    LOG("Automation: quitting with exit code {}", code);
    // TerminateProcess: the game's own shutdown waits for its threads and windows, which a test run does not need.
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
    for (;;)
    {
        Sleep(INFINITE);
    }
}
