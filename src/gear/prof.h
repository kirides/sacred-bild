#pragma once
// Opt-in frame cost counters for Sacred Gear (environment variable SACREDGEAR_PROF set to anything).
// Each slot accumulates the time between Scope's construction and destruction on QueryPerformanceCounter;
// report() writes the totals to the log every few seconds. Off: one load of a bool per scope.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "log.h"

namespace GearProf
{
    enum Slot
    {
        GameUpdate,     // the game's own cEngine_updateZoom (reference)
        Save, ItemTip, Drops, Party, Buffs,
        LuaFrame,       // LuaHost::frame in full (kernel.tick plus the lock)
        DrawHud, DrawWorld, DrawTop,
        DrawCopy,       // the published command list copied for a layer
        DrawCmds,       // commands replayed
        MenuTick,
        SlotCount,
    };

    inline const char* const kNames[SlotCount] = {
        "game_updateZoom", "Save::frame", "ItemTip::frame", "Drops::frame", "Party::frame", "Buffs::frame",
        "LuaHost::frame", "draw.hud", "draw.world", "draw.top", "draw.copy", "draw.commands(count)", "menuTick",
    };

    struct Acc
    {
        double total = 0, max = 0;
        uint64_t n = 0;
    };

    inline bool g_on = false;
    inline Acc g_acc[SlotCount];
    inline double g_freq = 0;
    inline double g_lastReport = 0;

    inline double now()
    {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return static_cast<double>(t.QuadPart) / g_freq;
    }

    inline void init()
    {
        char buf[8];
        if (GetEnvironmentVariableA("SACREDGEAR_PROF", buf, sizeof buf) == 0)
        {
            return;
        }
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_freq = static_cast<double>(f.QuadPart);
        g_on = true;
        g_lastReport = now();
    }

    inline void add(Slot s, double seconds)
    {
        Acc& a = g_acc[s];
        a.total += seconds;
        a.n += 1;
        if (seconds > a.max)
        {
            a.max = seconds;
        }
    }

    inline void count(Slot s, uint64_t n)
    {
        g_acc[s].n += n;
    }

    struct Scope
    {
        Slot slot;
        double t0;
        explicit Scope(Slot s) : slot(s), t0(g_on ? now() : 0) {}
        ~Scope()
        {
            if (g_on)
            {
                add(slot, now() - t0);
            }
        }
    };

    inline void report()
    {
        if (!g_on)
        {
            return;
        }
        const double t = now();
        if (t - g_lastReport < 5.0)
        {
            return;
        }
        g_lastReport = t;
        for (int i = 0; i < SlotCount; ++i)
        {
            const Acc& a = g_acc[i];
            LOG("PROFC|{:.3f}|{}|{}|{:.4f}|{:.4f}", t, kNames[i], a.n, a.total * 1000.0, a.max * 1000.0);
        }
    }
}
