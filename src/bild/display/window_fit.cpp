#include "window_fit.h"
#include "resolution.h"
#include "settings/config/display.h"
#include "log.h"
#include "patch.h"

#include <algorithm>
#include <cstdint>

namespace
{
    using GetCursorPosFn = BOOL(WINAPI*)(LPPOINT);

    GetCursorPosFn g_origGetCursorPos = nullptr;
    HWND g_window = nullptr;
    bool g_resizable = false;
    bool g_fixedSize = false;       // fullscreen: the window's size is not the user's to change

    // "Maximized": the largest window of the frame's aspect ratio in the work area, not Windows' own maximized
    // state (which would stretch the frame over the whole work area). The window it was before is kept for restoring.
    bool g_fitMaximized = false;
    RECT g_restoreRect = {};
    // A window maximized by Windows (snapped to the top edge): restored, then fitted. Next to Inject's messages
    // (WM_APP + 0x5B0..0x5B2), which the window procedure unwraps before this one.
    constexpr UINT kFixMaximize = WM_APP + 0x5B3;

    int roundDiv(int64_t value, int64_t divisor)
    {
        return static_cast<int>((value * 2 + divisor) / (divisor * 2));
    }

    // The client area as a fraction of the frame; the cursor over it maps to the frame's pixels.
    struct Fit
    {
        POINT origin;       // client area on the screen
        int clientWidth, clientHeight;
        int gameWidth, gameHeight;
    };

    bool fit(Fit& f)
    {
        HWND window = g_window;
        RECT client = {};
        f.origin = {0, 0};
        if (!g_resizable || !window || IsIconic(window) || !GetClientRect(window, &client) || !ClientToScreen(window, &f.origin))
        {
            return false;
        }
        f.clientWidth = client.right;
        f.clientHeight = client.bottom;
        f.gameWidth = Resolution::width();
        f.gameHeight = Resolution::height();
        return f.clientWidth > 0 && f.clientHeight > 0 && (f.clientWidth != f.gameWidth || f.clientHeight != f.gameHeight);
    }

    // Inside the client area the position stays inside the frame.
    POINT toGame(const Fit& f, POINT client)
    {
        POINT game = {roundDiv(int64_t(client.x) * f.gameWidth, f.clientWidth), roundDiv(int64_t(client.y) * f.gameHeight, f.clientHeight)};
        if (client.x >= 0 && client.y >= 0 && client.x < f.clientWidth && client.y < f.clientHeight)
        {
            game.x = std::clamp<LONG>(game.x, 0, f.gameWidth - 1);
            game.y = std::clamp<LONG>(game.y, 0, f.gameHeight - 1);
        }
        return game;
    }

    POINT toClient(const Fit& f, POINT game)
    {
        return {roundDiv(int64_t(game.x) * f.clientWidth, f.gameWidth), roundDiv(int64_t(game.y) * f.clientHeight, f.gameHeight)};
    }

    BOOL WINAPI hookGetCursorPos(LPPOINT point)
    {
        const BOOL ok = g_origGetCursorPos(point);
        if (ok && point)
        {
            *point = WindowFit::screenToGame(*point);
        }
        return ok;
    }

    // What the window's frame and caption add to the client area, from its style (its rectangles are the minimized
    // window's while it is restored from there).
    bool overhead(HWND window, int& width, int& height)
    {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        using AdjustForDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
        static const HMODULE user32 = GetModuleHandleW(L"user32.dll");
        static const auto getDpi = reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(user32, "GetDpiForWindow"));
        static const auto adjustForDpi = reinterpret_cast<AdjustForDpiFn>(GetProcAddress(user32, "AdjustWindowRectExForDpi"));
        RECT rect = {};
        const DWORD style = static_cast<DWORD>(GetWindowLongA(window, GWL_STYLE));
        const DWORD exStyle = static_cast<DWORD>(GetWindowLongA(window, GWL_EXSTYLE));
        if (getDpi && adjustForDpi)
        {
            adjustForDpi(&rect, style, FALSE, exStyle, getDpi(window));
        }
        else
        {
            AdjustWindowRectEx(&rect, style, FALSE, exStyle);
        }
        width = rect.right - rect.left;
        height = rect.bottom - rect.top;
        return true;
    }

    // Client width for a client height of the frame's aspect ratio, and back.
    int widthFor(int height) { return roundDiv(int64_t(height) * Resolution::width(), Resolution::height()); }
    int heightFor(int width) { return roundDiv(int64_t(width) * Resolution::height(), Resolution::width()); }

    // Within a pixel of the frame's aspect ratio.
    bool aspectOk(int clientWidth, int clientHeight)
    {
        return std::abs(heightFor(clientWidth) - clientHeight) <= 1 || std::abs(widthFor(clientHeight) - clientWidth) <= 1;
    }

    void sizing(HWND window, WPARAM edge, RECT& rect)
    {
        int ow = 0, oh = 0;
        if (!overhead(window, ow, oh))
        {
            return;
        }
        const int width = (rect.right - rect.left) - ow;
        const int height = (rect.bottom - rect.top) - oh;
        if (width <= 0 || height <= 0 || aspectOk(width, height))
        {
            return;
        }
        switch (edge)
        {
        case WMSZ_LEFT:
        case WMSZ_RIGHT:
            rect.bottom = rect.top + heightFor(width) + oh;
            break;
        case WMSZ_TOP:
        case WMSZ_BOTTOM:
            rect.right = rect.left + widthFor(height) + ow;
            break;
        case WMSZ_TOPLEFT:
        case WMSZ_TOPRIGHT:
            rect.top = rect.bottom - (heightFor(width) + oh);
            break;
        default:    // the bottom corners
            rect.bottom = rect.top + heightFor(width) + oh;
            break;
        }
    }

    // Any other change of the window's size (snapping, a program's SetWindowPos): the larger dimension gives way.
    void positionChanging(HWND window, WINDOWPOS& pos)
    {
        int ow = 0, oh = 0;
        if ((pos.flags & SWP_NOSIZE) || IsZoomed(window) || IsIconic(window) || pos.x <= -30000 || !overhead(window, ow, oh))
        {
            return;     // (a minimized window is parked at -32000)
        }
        const int width = pos.cx - ow;
        const int height = pos.cy - oh;
        if (width <= 0 || height <= 0 || aspectOk(width, height))
        {
            return;
        }
        if (heightFor(width) <= height)
        {
            pos.cy = heightFor(width) + oh;
        }
        else
        {
            pos.cx = widthFor(height) + ow;
        }
    }

    void fitMaximize(HWND window)
    {
        int ow = 0, oh = 0;
        MONITORINFO monitor = {};
        monitor.cbSize = sizeof(monitor);
        if (!overhead(window, ow, oh) || !GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor))
        {
            return;
        }
        const RECT work = monitor.rcWork;
        int width = (work.right - work.left) - ow;
        int height = (work.bottom - work.top) - oh;
        if (heightFor(width) <= height)
        {
            height = heightFor(width);
        }
        else
        {
            width = widthFor(height);
        }
        if (!g_fitMaximized)
        {
            GetWindowRect(window, &g_restoreRect);
        }
        const int outerWidth = width + ow, outerHeight = height + oh;
        const int x = work.left + ((work.right - work.left) - outerWidth) / 2;
        const int y = work.top + ((work.bottom - work.top) - outerHeight) / 2;
        g_fitMaximized = true;
        SetWindowPos(window, nullptr, x, y, outerWidth, outerHeight, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void fitRestore(HWND window)
    {
        g_fitMaximized = false;
        SetWindowPos(window, nullptr, g_restoreRect.left, g_restoreRect.top, g_restoreRect.right - g_restoreRect.left,
            g_restoreRect.bottom - g_restoreRect.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void WindowFit::install()
{
    if (!Config::display.resizable && Config::display.fullscreen == Config::Display::Fullscreen::Off)
    {
        return;
    }
    g_origGetCursorPos = static_cast<GetCursorPosFn>(
        Patch::iat("USER32.dll", "GetCursorPos", reinterpret_cast<void*>(&hookGetCursorPos)));
    if (!g_origGetCursorPos)
    {
        g_origGetCursorPos = &GetCursorPos;
    }
}

void WindowFit::windowCreated(HWND window, bool fixedSize)
{
    g_window = window;
    g_resizable = true;
    g_fixedSize = fixedSize;
    LOG("Window: {}, the {}x{} frame is stretched to the client area", fixedSize ? "fullscreen" : "resizable",
        Resolution::width(), Resolution::height());
}

bool WindowFit::scaled()
{
    Fit f;
    return fit(f);
}

POINT WindowFit::clientToGame(POINT client)
{
    Fit f;
    return fit(f) ? toGame(f, client) : client;
}

POINT WindowFit::screenToGame(POINT screen)
{
    Fit f;
    if (!fit(f))
    {
        return screen;
    }
    const POINT game = toGame(f, {screen.x - f.origin.x, screen.y - f.origin.y});
    return {f.origin.x + game.x, f.origin.y + game.y};
}

POINT WindowFit::gameToScreen(POINT game)
{
    Fit f;
    if (!fit(f))
    {
        return game;
    }
    const POINT client = toClient(f, {game.x - f.origin.x, game.y - f.origin.y});
    return {f.origin.x + client.x, f.origin.y + client.y};
}

bool WindowFit::message(HWND window, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result)
{
    if (!g_resizable || window != g_window)
    {
        return false;
    }
    if (g_fixedSize)
    {
        if (message != WM_ERASEBKGND)
        {
            return false;
        }
        result = 1;
        return true;
    }
    switch (message)
    {
    case WM_GETMINMAXINFO:
    {
        // Not smaller than a quarter of the frame (plus the window's frame and caption on top of the client area).
        int ow = 0, oh = 0;
        if (!overhead(window, ow, oh))
        {
            return false;
        }
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = std::max(320, Resolution::width() / 4) + ow;
        info->ptMinTrackSize.y = std::max(240, Resolution::height() / 4) + oh;
        result = 0;
        return true;
    }
    case WM_SIZING:
        g_fitMaximized = false;     // sized by hand: maximizing again maximizes
        sizing(window, wParam, *reinterpret_cast<RECT*>(lParam));
        result = TRUE;
        return true;
    case WM_WINDOWPOSCHANGING:
        positionChanging(window, *reinterpret_cast<WINDOWPOS*>(lParam));
        return false;
    case WM_SYSCOMMAND:
        switch (wParam & 0xFFF0)
        {
        case SC_MAXIMIZE:
            if (g_fitMaximized)
            {
                fitRestore(window);
            }
            else
            {
                fitMaximize(window);
            }
            result = 0;
            return true;
        case SC_RESTORE:
            if (g_fitMaximized && !IsIconic(window))
            {
                fitRestore(window);
                result = 0;
                return true;
            }
            return false;
        default:
            return false;
        }
    case WM_SIZE:
        // Windows maximized the window itself (snapped to the top edge): back to normal, then the fitted size.
        if (wParam == SIZE_MAXIMIZED)
        {
            LOG("Window: maximized by Windows, fitting it");
            PostMessageW(window, kFixMaximize, 0, 0);
        }
        return false;
    case kFixMaximize:
        if (IsZoomed(window))
        {
            ShowWindow(window, SW_RESTORE);
            fitMaximize(window);
        }
        result = 0;
        return true;
    case WM_ERASEBKGND:
        result = 1;     // the stretched frame covers the whole client area
        return true;
    default:
        return false;
    }
}
