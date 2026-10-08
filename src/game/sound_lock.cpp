#include "game/sound_lock.h"
#include "game/d3d_stats.h"
#include "config/render.h"
#include "log.h"
#include "patch.h"
#include "sacred/sound.h"
#include "spin_lock.h"

#include <windows.h>
#include <atomic>
#include <cstdint>

namespace
{
    using namespace Sacred;

    decltype(Addr::cMutex_lock)::Ptr g_origLock = nullptr;
    decltype(Addr::cMutex_unlock)::Ptr g_origUnlock = nullptr;

    // Recursive, like the mutex it stands in for. An unlock by a thread that doesn't hold it does nothing, as
    // ReleaseMutex fails then.
    struct Lock
    {
        std::atomic<cMutex*> mutex{nullptr};    // the one wrapper it stands in for, claimed by its first use
        SRWLOCK srw = SRWLOCK_INIT;
        std::atomic<DWORD> owner{0};            // thread id; 0 = free
        uint32_t depth = 0;                     // only touched by the owner
    };
    Lock g_lock;
    bool g_active = false;      // both hooks attached: one of them alone would leave the mutex locked for good

    // The game makes one such mutex (the sound system's); another one would keep its kernel mutex.
    bool ours(cMutex* m)
    {
        if (!g_active)
        {
            return false;
        }
        cMutex* current = g_lock.mutex.load(std::memory_order_acquire);
        if (current)
        {
            return current == m;
        }
        return g_lock.mutex.compare_exchange_strong(current, m) || current == m;
    }

    void __fastcall hookLock(cMutex* self, void* edx)
    {
        if (!ours(self))
        {
            g_origLock(self, edx);
            return;
        }
        const DWORD thread = SpinLock::currentThread();
        if (g_lock.owner.load(std::memory_order_relaxed) == thread)
        {
            ++g_lock.depth;
            return;
        }
        if (!TryAcquireSRWLockExclusive(&g_lock.srw))
        {
            const int64_t start = D3DStats::now();
            AcquireSRWLockExclusive(&g_lock.srw);
            if (D3DStats::isRenderThread())
            {
                D3DStats::addTime(D3DStats::TSoundWait, D3DStats::now() - start);
            }
        }
        g_lock.owner.store(thread, std::memory_order_relaxed);
        g_lock.depth = 1;
    }

    void __fastcall hookUnlock(cMutex* self, void* edx)
    {
        if (!ours(self))
        {
            g_origUnlock(self, edx);
            return;
        }
        if (g_lock.owner.load(std::memory_order_relaxed) != SpinLock::currentThread())
        {
            return;
        }
        if (--g_lock.depth == 0)
        {
            g_lock.owner.store(0, std::memory_order_relaxed);
            ReleaseSRWLockExclusive(&g_lock.srw);
        }
    }
}

void SoundLock::install()
{
    if (!Config::render.soundLock || !Addr::cMutex_lock || !Addr::cMutex_unlock)
    {
        return;
    }
    const bool lock = Patch::hook(g_origLock, Addr::cMutex_lock, &hookLock, "cMutex::lock");
    const bool unlock = Patch::hook(g_origUnlock, Addr::cMutex_unlock, &hookUnlock, "cMutex::unlock");
    g_active = lock && unlock;
    if (g_active)
    {
        LOG("Sound lock: the sound system's mutex is a user-mode lock");
    }
}
