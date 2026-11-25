#include "killstat.h"
#include "log.h"
#include "lua_host.h"
#include "engine/gear_addr.h"
#include "patch.h"

#include <atomic>
#include <cstdint>

namespace
{
    using namespace Gear;

    decltype(Addr::cKillStat_record)::Ptr g_orig = nullptr;
    std::atomic<bool> g_active{false};

    // The engine's function takes 17 stack words after `this`; the first is the record id (the index into the per-id counters when it is
    // below 0x300). The hook passes all of them on untouched.
    void __fastcall hookRecord(void* self, void* edx, uint32_t id, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7, uint32_t a8,
                               uint32_t a9, uint32_t a10, uint32_t a11, uint32_t a12, uint32_t a13, uint32_t a14, uint32_t a15, uint32_t a16,
                               uint32_t a17, uint32_t a18)
    {
        g_orig(self, edx, id, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18);
        if (g_active.load(std::memory_order_relaxed))
        {
            LuaHost::killRecorded(id);
        }
    }
}

bool KillStat::install()
{
    if (!Addr::cKillStat_record)
    {
        return false;
    }
    return Patch::hook(g_orig, Addr::cKillStat_record, hookRecord, "cKillStat_record");
}

void KillStat::setActive(bool active)
{
    g_active.store(active);
}
