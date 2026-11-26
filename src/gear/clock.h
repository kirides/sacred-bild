#pragma once
#include <chrono>

// The time mods and RmlUi see (native.time, RmlUi's GetElapsedTime): a steady clock in seconds. A host other than the game (the UI sandbox,
// docs/Sandbox.md) may give its own clock instead, simulated time for reproducible tests. Budgets and file polling keep the real clock.
namespace GearClock
{
    inline double (*g_override)() = nullptr;

    inline double seconds()
    {
        if (g_override)
        {
            return g_override();
        }
        using Clock = std::chrono::steady_clock;
        static const Clock::time_point start = Clock::now();
        return std::chrono::duration<double>(Clock::now() - start).count();
    }
}
