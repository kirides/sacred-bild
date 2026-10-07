#include "game/granny_async.h"
#include "config.h"
#include "fmt.h"
#include "log.h"
#include "patch.h"
#include "profiler.h"

#include <windows.h>
#include <float.h>
#include <intrin.h>
#include <xmmintrin.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
    using AdvanceTimeFn = int(__stdcall*)(void* handle, double seconds);

    AdvanceTimeFn g_origAdvanceTime = nullptr;
    void** g_originals = nullptr;           // original import targets, jumped to by the stubs

    HANDLE g_jobEvent = nullptr;            // auto reset: a job is ready
    HANDLE g_idleEvent = nullptr;           // manual reset: set while no job runs
    std::atomic<bool> g_busy{false};
    DWORD g_workerId = 0;
    void* g_jobHandle = nullptr;
    double g_jobSeconds = 0.0;
    unsigned g_jobFpu = 0;                  // the caller's x87 control word and MXCSR, for identical results
    unsigned g_jobMxcsr = 0;

    // Diagnostics, read once every few seconds.
    std::atomic<int64_t> g_advanceTicks{0}, g_waitTicks{0};
    std::atomic<uint32_t> g_jobs{0}, g_waits{0};
    std::atomic<bool> g_firstWaitPending{false};
    std::atomic<uint32_t> g_earlyJobs{0}, g_lateJobs{0};

    // EarlyAnimation (render thread only): the advance the game asked for, not started yet.
    bool g_early = false;
    bool g_pending = false;
    void* g_pendingHandle = nullptr;
    double g_pendingSeconds = 0.0;
    std::mutex g_siteMutex;
    std::unordered_map<uintptr_t, uint32_t> g_firstWaitSites;   // where the game first waited after a job started

    int64_t qpc()
    {
        LARGE_INTEGER v;
        QueryPerformanceCounter(&v);
        return v.QuadPart;
    }

    void waitIdle(uintptr_t caller)
    {
        if (!g_busy.load(std::memory_order_acquire) || GetCurrentThreadId() == g_workerId)
        {
            return;
        }
        const int64_t start = qpc();
        WaitForSingleObject(g_idleEvent, INFINITE);
        g_waitTicks.fetch_add(qpc() - start, std::memory_order_relaxed);
        g_waits.fetch_add(1, std::memory_order_relaxed);
        if (g_firstWaitPending.exchange(false))
        {
            std::scoped_lock lock(g_siteMutex);
            ++g_firstWaitSites[caller];
        }
    }

    // Entered from the generated stubs, first thing in the Granny call: [esp] returns into the stub,
    // [esp + 4] is the game's return address. Clobbers only what a stdcall callee may clobber.
    void __cdecl waitFromStub()
    {
        waitIdle(reinterpret_cast<uintptr_t>(*(static_cast<void**>(_AddressOfReturnAddress()) + 1)));
    }

    // On the worker; the caller made sure it is idle.
    void startJob(void* handle, double seconds)
    {
        g_jobHandle = handle;
        g_jobSeconds = seconds;
        g_jobFpu = _control87(0, 0);
        g_jobMxcsr = _mm_getcsr();
        ResetEvent(g_idleEvent);
        g_busy.store(true, std::memory_order_release);
        g_firstWaitPending.store(true);
        SetEvent(g_jobEvent);
    }

    int __stdcall asyncAdvanceTime(void* handle, double seconds)
    {
        const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
        if (GetCurrentThreadId() == g_workerId)
        {
            return g_origAdvanceTime(handle, seconds);
        }
        if (g_early)
        {
            // Recorded for afterWorld. One still waiting (no world view since) runs now, as it would have without
            // EarlyAnimation: the time adds up and the lag stays one frame. Never for another animation context.
            if (g_pending && g_pendingHandle == handle)
            {
                waitIdle(caller);
                startJob(g_pendingHandle, g_pendingSeconds);
                g_lateJobs.fetch_add(1, std::memory_order_relaxed);
            }
            g_pending = true;
            g_pendingHandle = handle;
            g_pendingSeconds = seconds;
            return 0;
        }
        waitIdle(caller);
        startJob(handle, seconds);
        return 0;   // the only caller (0x401953) ignores the result
    }

    DWORD WINAPI worker(LPVOID)
    {
        for (;;)
        {
            WaitForSingleObject(g_jobEvent, INFINITE);
            _control87(g_jobFpu, _MCW_PC | _MCW_RC | _MCW_EM);
            _mm_setcsr(g_jobMxcsr);
            const int64_t start = qpc();
            g_origAdvanceTime(g_jobHandle, g_jobSeconds);
            g_advanceTicks.fetch_add(qpc() - start, std::memory_order_relaxed);
            g_jobs.fetch_add(1, std::memory_order_relaxed);
            g_busy.store(false, std::memory_order_release);
            SetEvent(g_idleEvent);
        }
    }

    const IMAGE_IMPORT_DESCRIPTOR* grannyImports(uint8_t* base)
    {
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
        const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        for (auto* imp = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp)
        {
            if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), "granny.dll") == 0)
            {
                return imp;
            }
        }
        return nullptr;
    }
}

void GrannyAsync::install()
{
    if (!g_config.asyncAnimation)
    {
        return;
    }
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    const IMAGE_IMPORT_DESCRIPTOR* imp = grannyImports(base);
    if (!imp)
    {
        LOG("Animation: granny.dll imports not found, staying synchronous");
        return;
    }
    auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
    auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
    size_t count = 0;
    while (names[count].u1.AddressOfData)
    {
        ++count;
    }

    // Per import: call waitFromStub; jmp [original].
    constexpr size_t kStubSize = 11;
    auto* stubs = static_cast<uint8_t*>(VirtualAlloc(nullptr, count * kStubSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g_originals = new void*[count];
    if (!stubs)
    {
        LOG("Animation: no memory for stubs, staying synchronous");
        return;
    }
    std::vector<std::pair<uintptr_t, void*>> patches;
    for (size_t i = 0; i < count; ++i)
    {
        void* original = reinterpret_cast<void*>(slots[i].u1.Function);
        const char* name = IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal)
            ? ""
            : reinterpret_cast<const char*>(reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData)->Name);
        if (std::strcmp(name, "_GrannyAdvanceTime@12") == 0)
        {
            g_origAdvanceTime = reinterpret_cast<AdvanceTimeFn>(original);
            patches.emplace_back(reinterpret_cast<uintptr_t>(&slots[i].u1.Function), reinterpret_cast<void*>(&asyncAdvanceTime));
            continue;
        }
        g_originals[i] = original;
        uint8_t* stub = stubs + i * kStubSize;
        stub[0] = 0xE8;
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&waitFromStub) - reinterpret_cast<uintptr_t>(stub + 5));
        std::memcpy(stub + 1, &rel, 4);
        stub[5] = 0xFF;
        stub[6] = 0x25;
        void** target = &g_originals[i];
        std::memcpy(stub + 7, &target, 4);
        patches.emplace_back(reinterpret_cast<uintptr_t>(&slots[i].u1.Function), stub);
    }
    if (!g_origAdvanceTime)
    {
        LOG("Animation: GrannyAdvanceTime not imported, staying synchronous");
        VirtualFree(stubs, 0, MEM_RELEASE);
        return;
    }
    DWORD old = 0;
    VirtualProtect(stubs, count * kStubSize, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), stubs, count * kStubSize);

    g_jobEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_idleEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    HANDLE thread = CreateThread(nullptr, 0, &worker, nullptr, 0, &g_workerId);
    if (!g_jobEvent || !g_idleEvent || !thread)
    {
        LOG("Animation: worker thread could not start, staying synchronous");
        return;
    }
    // Animation finishing late holds up the frame: keep up with the render thread.
    SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL);
    CloseHandle(thread);
    if (g_config.profiler)
    {
        Profiler::addThread(g_workerId);
    }

    for (const auto& [slot, target] : patches)
    {
        Patch::write(slot, &target, sizeof(target));
    }
    g_early = g_config.earlyAnimation;
    LOG("Animation: GrannyAdvanceTime runs on worker thread {}{}, {} other Granny imports wait for it", g_workerId,
        g_early ? ", started after the world view (EarlyAnimation)" : "", count - 1);
}

void GrannyAsync::afterWorld()
{
    // A job still running (no character in this frame waited for it) keeps the recorded one for the next frame's call.
    if (!g_early || !g_pending || g_busy.load(std::memory_order_acquire))
    {
        return;
    }
    g_pending = false;
    startJob(g_pendingHandle, g_pendingSeconds);
    g_earlyJobs.fetch_add(1, std::memory_order_relaxed);
}

void GrannyAsync::onFrame()
{
    static uint32_t frames = 0;
    static int64_t lastReport = 0;
    if (!g_origAdvanceTime)
    {
        return;
    }
    ++frames;
    const int64_t now = qpc();
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    if (!lastReport)
    {
        lastReport = now;
        return;
    }
    if (now - lastReport < 5 * freq.QuadPart)
    {
        return;
    }
    lastReport = now;
    // DDrawCompat limits the process to one CPU by default (CpuAffinity = 1); then nothing runs in parallel.
    DWORD_PTR process = 0, system = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &process, &system);
    static DWORD_PTR loggedMask = 0;
    if (process != loggedMask)
    {
        loggedMask = process;
        LOG("Animation: process may use {} of {} CPUs (mask {:x}){}", __popcnt(static_cast<unsigned>(process)),
            __popcnt(static_cast<unsigned>(system)), process,
            __popcnt(static_cast<unsigned>(process)) < 2 ? ": the worker can't run in parallel (DDrawCompat CpuAffinity)" : "");
    }
    const double ms = 1000.0 / static_cast<double>(freq.QuadPart) / std::max<uint32_t>(frames, 1);
    std::vector<std::pair<uintptr_t, uint32_t>> sites;
    {
        std::scoped_lock lock(g_siteMutex);
        sites.assign(g_firstWaitSites.begin(), g_firstWaitSites.end());
        g_firstWaitSites.clear();
    }
    std::sort(sites.begin(), sites.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string top;
    for (size_t i = 0; i < std::min<size_t>(sites.size(), 4); ++i)
    {
        top += Fmt::format(" {:08x} x{}", sites[i].first, sites[i].second);
    }
    const uint32_t early = g_earlyJobs.exchange(0), late = g_lateJobs.exchange(0);
    LOG("Animation: per frame advance {:.2f} ms on the worker, game waited {:.2f} ms ({:.1f} waits); first waits at{}{}",
        g_advanceTicks.exchange(0) * ms, g_waitTicks.exchange(0) * ms,
        static_cast<double>(g_waits.exchange(0)) / std::max<uint32_t>(frames, 1), top.empty() ? " -" : top,
        g_early ? Fmt::format("; started after the world view {}, at the frame start {}", early, late) : std::string());
    g_jobs.exchange(0);
    frames = 0;
}
