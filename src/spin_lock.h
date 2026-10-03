#pragma once
#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdint>

// Locks for code the render thread enters tens of thousands of times per frame while other threads only come
// by now and then. Uncontended they cost one atomic operation; a waiting thread spins and yields instead of
// sleeping in the kernel, so the thread releasing the lock never pays for a wake-up system call.
namespace SpinLock
{
    inline DWORD currentThread()
    {
        return __readfsdword(0x24);     // TEB ClientId.UniqueThread (x86), what GetCurrentThreadId returns
    }

    inline void backoff(uint32_t& spin)
    {
        if (++spin < 64)
        {
            _mm_pause();
        }
        else if (spin < 256)
        {
            SwitchToThread();
        }
        else
        {
            Sleep(0);
        }
    }
}

// Recursive exclusive lock (BasicLockable).
class RecursiveSpinLock
{
public:
    void lock()
    {
        const DWORD self = SpinLock::currentThread();
        if (m_owner.load(std::memory_order_relaxed) == self)
        {
            ++m_depth;
            return;
        }
        for (uint32_t spin = 0;; SpinLock::backoff(spin))
        {
            DWORD expected = 0;
            if (m_owner.load(std::memory_order_relaxed) == 0 &&
                m_owner.compare_exchange_weak(expected, self, std::memory_order_acquire, std::memory_order_relaxed))
            {
                break;
            }
        }
        m_depth = 1;
    }

    void unlock()
    {
        if (--m_depth == 0)
        {
            m_owner.store(0, std::memory_order_release);
        }
    }

private:
    std::atomic<DWORD> m_owner{0};  // thread id; 0 = free (no thread has id 0)
    uint32_t m_depth = 0;           // only touched by the owner
};

// Reader/writer lock (SharedLockable), not recursive.
class SharedSpinLock
{
public:
    void lock_shared()
    {
        for (uint32_t spin = 0;; SpinLock::backoff(spin))
        {
            int32_t state = m_state.load(std::memory_order_relaxed);
            if (state >= 0 &&
                m_state.compare_exchange_weak(state, state + 1, std::memory_order_acquire, std::memory_order_relaxed))
            {
                return;
            }
        }
    }

    void unlock_shared()
    {
        m_state.fetch_sub(1, std::memory_order_release);
    }

    void lock()
    {
        for (uint32_t spin = 0;; SpinLock::backoff(spin))
        {
            int32_t expected = 0;
            if (m_state.load(std::memory_order_relaxed) == 0 &&
                m_state.compare_exchange_weak(expected, -1, std::memory_order_acquire, std::memory_order_relaxed))
            {
                return;
            }
        }
    }

    void unlock()
    {
        m_state.store(0, std::memory_order_release);
    }

private:
    std::atomic<int32_t> m_state{0};    // readers, or -1 while a writer holds it
};
