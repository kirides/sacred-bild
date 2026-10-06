#include "input/inject.h"

#include <atomic>
#include <cstdint>
#include <initializer_list>

namespace
{
    constexpr UINT kKeyMessage = WM_APP + 0x5B0;     // wParam: virtual key, lParam: as WM_KEYDOWN / WM_KEYUP's
    constexpr UINT kMouseMessage = WM_APP + 0x5B1;   // wParam: the mouse message, lParam: its wParam
    constexpr UINT kCallMessage = WM_APP + 0x5B2;    // wParam: function, lParam: context

    std::atomic<HWND> g_window{nullptr};
    std::atomic<uint8_t> g_held[256] = {};

    void post(UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (HWND window = g_window.load())
        {
            PostMessageA(window, message, wParam, lParam);
        }
    }

    bool extended(int vk)
    {
        switch (vk)
        {
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_RCONTROL: case VK_RMENU:
            return true;
        default:
            return false;
        }
    }

    // lParam of WM_KEYDOWN / WM_KEYUP: repeat count 1, scan code, extended flag; key up: previous state and transition.
    LPARAM keyParam(int vk, bool down)
    {
        const UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
        uint32_t l = 1u | ((scan & 0xFF) << 16) | (extended(vk) ? 1u << 24 : 0u);
        if (!down)
        {
            l |= (1u << 30) | (1u << 31);
        }
        return static_cast<LPARAM>(l);
    }

    // The character TranslateMessage would send for the key, with the held modifiers.
    bool translated(int vk, LPARAM lParam, char& out)
    {
        BYTE keys[256] = {};
        GetKeyboardState(keys);
        Inject::merge(keys);
        WORD chars = 0;
        const int n = ToAscii(static_cast<UINT>(vk), (static_cast<UINT>(lParam) >> 16) & 0xFF, keys, &chars, 0);
        out = static_cast<char>(chars & 0xFF);
        return n == 1;
    }

    WPARAM mouseKeys()
    {
        WPARAM keys = 0;
        keys |= Inject::held(VK_LBUTTON) ? MK_LBUTTON : 0;
        keys |= Inject::held(VK_RBUTTON) ? MK_RBUTTON : 0;
        keys |= Inject::held(VK_MBUTTON) ? MK_MBUTTON : 0;
        keys |= Inject::held(VK_SHIFT) ? MK_SHIFT : 0;
        keys |= Inject::held(VK_CONTROL) ? MK_CONTROL : 0;
        return keys;
    }
}

void Inject::setWindow(HWND window)
{
    g_window = window;
}

void Inject::key(int vk, bool down)
{
    if (held(vk) == down)
    {
        return;
    }
    g_held[vk & 0xFF] = down;
    post(kKeyMessage, static_cast<WPARAM>(vk), keyParam(vk, down));
}

void Inject::hold(int vk, bool down)
{
    g_held[vk & 0xFF] = down;
}

void Inject::button(int vk, bool down, bool doubleClick)
{
    if (held(vk) == down)
    {
        return;
    }
    g_held[vk & 0xFF] = down;
    UINT message = 0;
    switch (vk)
    {
    case VK_LBUTTON: message = down ? (doubleClick ? WM_LBUTTONDBLCLK : WM_LBUTTONDOWN) : WM_LBUTTONUP; break;
    case VK_RBUTTON: message = down ? (doubleClick ? WM_RBUTTONDBLCLK : WM_RBUTTONDOWN) : WM_RBUTTONUP; break;
    case VK_MBUTTON: message = down ? (doubleClick ? WM_MBUTTONDBLCLK : WM_MBUTTONDOWN) : WM_MBUTTONUP; break;
    default: return;
    }
    post(kMouseMessage, message, static_cast<LPARAM>(mouseKeys()));
}

void Inject::wheel(int delta)
{
    post(kMouseMessage, WM_MOUSEWHEEL, static_cast<LPARAM>(MAKEWPARAM(mouseKeys(), static_cast<WORD>(static_cast<SHORT>(delta)))));
}

void Inject::mouseMove()
{
    post(kMouseMessage, WM_MOUSEMOVE, static_cast<LPARAM>(mouseKeys()));
}

void Inject::call(void (*fn)(void*), void* context)
{
    post(kCallMessage, reinterpret_cast<WPARAM>(fn), reinterpret_cast<LPARAM>(context));
}

void Inject::releaseAll()
{
    for (int vk : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON})
    {
        button(vk, false);
    }
    for (int vk = 0; vk < 256; ++vk)
    {
        if (held(vk))
        {
            if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU)
            {
                hold(vk, false);
            }
            else
            {
                key(vk, false);
            }
        }
    }
}

bool Inject::held(int vk)
{
    return g_held[vk & 0xFF].load(std::memory_order_relaxed) != 0;
}

void Inject::merge(BYTE* keys)
{
    for (int vk = 0; vk < 256; ++vk)
    {
        if (held(vk))
        {
            keys[vk] |= 0x80;
        }
    }
}

bool Inject::unwrap(HWND window, UINT message, WPARAM wParam, LPARAM lParam, WNDPROC game, LRESULT& result)
{
    switch (message)
    {
    case kKeyMessage:
    {
        const bool down = (static_cast<uint32_t>(lParam) & (1u << 31)) == 0;
        result = CallWindowProcA(game, window, down ? WM_KEYDOWN : WM_KEYUP, wParam, lParam);
        char c = 0;
        if (down && translated(static_cast<int>(wParam), lParam, c))
        {
            CallWindowProcA(game, window, WM_CHAR, static_cast<BYTE>(c), lParam);
        }
        return true;
    }
    case kMouseMessage:
    {
        // The position as the message would carry it; the game reads getClientCursorPos instead.
        POINT p = {};
        GetCursorPos(&p);
        ScreenToClient(window, &p);
        LPARAM position = MAKELPARAM(p.x, p.y);
        if (wParam == WM_MOUSEWHEEL)
        {
            ClientToScreen(window, &p);
            position = MAKELPARAM(p.x, p.y);
        }
        result = CallWindowProcA(game, window, static_cast<UINT>(wParam), static_cast<WPARAM>(lParam), position);
        return true;
    }
    case kCallMessage:
        reinterpret_cast<void (*)(void*)>(wParam)(reinterpret_cast<void*>(lParam));
        result = 0;
        return true;
    default:
        return false;
    }
}
