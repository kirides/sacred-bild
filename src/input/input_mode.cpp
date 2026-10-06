#include "input/input_mode.h"
#include "log.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace
{
    std::atomic<bool> g_controller{false};
    std::atomic<int64_t> g_anchor{0};   // screen cursor (x << 32 | y) when the controller took over

    constexpr int kMoveThreshold = 10;  // pixels

    int64_t pack(POINT p)
    {
        return (static_cast<int64_t>(p.x) << 32) | static_cast<uint32_t>(p.y);
    }

    // Switches; `why` says what for the log (the first switches only: the player may switch all the time).
    void set(bool controller, const char* why)
    {
        static std::atomic<int> logged{0};
        if (g_controller.exchange(controller) != controller && logged.fetch_add(1) < 50)
        {
            LOG("Controller: input from the {} ({})", controller ? "controller" : "keyboard and mouse", why);
        }
    }
}

bool InputMode::controller()
{
    return g_controller.load(std::memory_order_relaxed);
}

void InputMode::controllerUsed()
{
    if (!g_controller.load(std::memory_order_relaxed))
    {
        POINT p = {};
        GetCursorPos(&p);
        g_anchor = pack(p);
        set(true, "pad");
    }
}

void InputMode::keyboardUsed(unsigned message, uintptr_t wParam)
{
    if (g_controller.load(std::memory_order_relaxed))
    {
        char why[48];
        std::snprintf(why, sizeof(why), "message %04x, %x", message, static_cast<unsigned>(wParam));
        set(false, why);
    }
}

void InputMode::mouseMoved()
{
    if (!g_controller.load(std::memory_order_relaxed))
    {
        return;
    }
    POINT p = {};
    if (!GetCursorPos(&p))
    {
        return;
    }
    const int64_t anchor = g_anchor.load();
    const int ax = static_cast<int>(anchor >> 32), ay = static_cast<int>(static_cast<int32_t>(anchor & 0xFFFFFFFF));
    if (std::abs(p.x - ax) > kMoveThreshold || std::abs(p.y - ay) > kMoveThreshold)
    {
        char why[64];
        std::snprintf(why, sizeof(why), "mouse moved from %d,%d to %ld,%ld", ax, ay, p.x, p.y);
        set(false, why);
    }
}
