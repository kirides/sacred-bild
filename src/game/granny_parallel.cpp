#include "game/granny_parallel.h"
#include "game/granny_mesh.h"
#include "config.h"
#include "log.h"
#include "patch.h"
#include "profiler.h"

#include <windows.h>
#include <float.h>
#include <intrin.h>
#include <xmmintrin.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace
{
    // granny.dll internals (image base 0x10000000).
    namespace Scene
    {
        constexpr uintptr_t time = 0x08;           // double, already advanced when the control walk runs
        constexpr uintptr_t controls = 0x20;       // list sentinel: node {next, prev, control | flags}
    }
    namespace Control
    {
        constexpr size_t size = 0x88;              // both kinds (operator new(0x88))
        constexpr uintptr_t finished = 0x10;       // byte: delete once it is no longer active
        constexpr uintptr_t animationTarget = 0x68; // animation control: the skeleton it blends into
        constexpr uintptr_t poseTarget = 0x70;     // pose control: the same
        constexpr uint32_t accumulateSlot = 2;     // vtable slot of the sampling/blending method
    }
    namespace Skeleton
    {
        constexpr uintptr_t boneCount = GrannyMesh::Skeleton::boneCount;
        constexpr uintptr_t bones = 0x18;          // pointer to 300-byte bone states
        constexpr size_t boneSize = 300;
    }
    // An animation handle while a control samples it: two 16-bit reference counts.
    constexpr uintptr_t kHandleRefs = 8;
    constexpr uintptr_t kHandleLocks = 10;

    // thiscall targets are hooked and called as fastcall with an unused EDX parameter.
    using ControlsFn = void(__fastcall*)(uint8_t* scene, void* edx, uint32_t oldLo, uint32_t oldHi);
    using ActiveFn = uint32_t(__fastcall*)(void* control, void* edx, double time);     // result in AL
    using SampleFn = void(__fastcall*)(void* control, void* edx, double oldTime, double newTime);
    using UnlinkFn = void(__fastcall*)(void* list, void* edx, uint32_t* out, void** node);
    using LockFn = void(__fastcall*)(uint32_t* lock, void* edx, const uint32_t* handle);
    using ReleaseFn = void(__fastcall*)(uint32_t* lock);
    using DeleteFn = void(__fastcall*)(void* object, void* edx, uint32_t flags);

    ControlsFn g_origControls = nullptr;
    ActiveFn g_active = nullptr;
    SampleFn g_sample = nullptr;
    UnlinkFn g_unlink = nullptr;
    LockFn g_origLock = nullptr;
    ReleaseFn g_origRelease = nullptr;
    uintptr_t g_animationAccumulate = 0;
    uintptr_t g_poseAccumulate = 0;
    uintptr_t g_clockAccumulate = 0;        // the scene's own timekeeping control: blends nothing
    // Group key of the controls that write no bones (any thread may take them).
    const uint8_t* const kNoBones = reinterpret_cast<const uint8_t*>(1);
    uintptr_t g_grannyBase = 0;
    uintptr_t g_loggedUnknown = 0;

    std::atomic<bool> g_parallel{false};    // the sampling phase runs on several threads
    std::atomic<bool> g_inWalk{false};
    bool g_enabled = false;

    // A target skeleton's controls, in list order: g_grouped[first, first + count).
    struct Group
    {
        const uint8_t* bones;
        uint32_t first, count;
    };
    // The walk's controls by group, one array for all groups: rebuilt every walk without allocating (one walk at a
    // time, g_inWalk).
    std::vector<void*> g_grouped;

    std::span<void* const> controlsOf(const Group& group)
    {
        return {g_grouped.data() + group.first, group.count};
    }

    // The phase the threads share.
    struct Job
    {
        const std::vector<Group>* groups = nullptr;
        const uint32_t* order = nullptr;    // groups, largest first
        double oldTime = 0, newTime = 0;
        unsigned fpu = 0, mxcsr = 0;
        std::atomic<uint32_t> next{0};
        std::atomic<uint32_t> done{0};
    };
    Job g_job;
    HANDLE g_start = nullptr;               // semaphore: one count per helper and phase
    std::vector<DWORD> g_helperIds;

    // Statistics ([Debug] D3DStats): counted by the thread that runs the advance, read by the presenting one.
    struct Stats
    {
        std::atomic<uint32_t> walks{0}, serialWalks{0}, unknownWalks{0}, controls{0}, groups{0}, checks{0}, checkFailures{0};
        std::atomic<int64_t> parallelTicks{0}, totalTicks{0};
    };
    Stats g_stats;
    uint32_t g_frames = 0;
    std::atomic<uint32_t> g_lostReferences{0};
    DWORD g_nextCheck = 0;

    int64_t qpc()
    {
        LARGE_INTEGER v;
        QueryPerformanceCounter(&v);
        return v.QuadPart;
    }

    template <class T>
    T field(const void* p, uintptr_t offset)
    {
        T v;
        std::memcpy(&v, static_cast<const uint8_t*>(p) + offset, sizeof(T));
        return v;
    }

    // The handle lock and release a control's sampling makes (granny.dll 0x100043B0 / 0x10005370): plain 16-bit
    // increments and decrements on the animation handle, which other skeletons' controls share. Atomic while the
    // threads sample; the control keeps its own reference, so a release never takes a count to zero there.
    void __fastcall hookLock(uint32_t* lock, void* edx, const uint32_t* handle)
    {
        if (!g_parallel.load(std::memory_order_relaxed))
        {
            g_origLock(lock, edx, handle);
            return;
        }
        auto* p = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(*handle));
        lock[0] = *handle;
        lock[2] = *handle;
        _InterlockedIncrement16(reinterpret_cast<volatile short*>(p + kHandleRefs));
        _InterlockedIncrement16(reinterpret_cast<volatile short*>(p + kHandleLocks));
        lock[1] = field<uint32_t>(p, 0);
        lock[3] = field<uint32_t>(p, 4) & ~3u;
    }

    void __fastcall hookRelease(uint32_t* lock)
    {
        if (!g_parallel.load(std::memory_order_relaxed))
        {
            g_origRelease(lock);
            return;
        }
        auto* p = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(lock[0]));
        _InterlockedDecrement16(reinterpret_cast<volatile short*>(p + kHandleLocks));
        if (_InterlockedDecrement16(reinterpret_cast<volatile short*>(p + kHandleRefs)) == 0)
        {
            // The last reference: the original would free the handle, which can't happen on these threads. Keep it
            // (a leak, logged) rather than free it here.
            _InterlockedIncrement16(reinterpret_cast<volatile short*>(p + kHandleRefs));
            g_lostReferences.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // What the original walk does per control before its expiry check.
    void sample(void* control, double oldTime, double newTime)
    {
        if (g_active(control, nullptr, oldTime) & 0xFF)
        {
            g_sample(control, nullptr, oldTime, newTime);
        }
    }

    void runGroups(Job& job)
    {
        const auto& groups = *job.groups;
        for (uint32_t i; (i = job.next.fetch_add(1, std::memory_order_relaxed)) < groups.size();)
        {
            for (void* control : controlsOf(groups[job.order[i]]))
            {
                sample(control, job.oldTime, job.newTime);
            }
        }
    }

    DWORD WINAPI helper(LPVOID)
    {
        for (;;)
        {
            WaitForSingleObject(g_start, INFINITE);
            _control87(g_job.fpu, _MCW_PC | _MCW_RC | _MCW_EM);
            _mm_setcsr(g_job.mxcsr);
            runGroups(g_job);
            g_job.done.fetch_add(1, std::memory_order_release);
        }
    }

    // Samples every group on the calling thread and the helpers.
    void runParallel(const std::vector<Group>& groups, const std::vector<uint32_t>& order, double oldTime,
        double newTime)
    {
        Job& job = g_job;
        job.groups = &groups;
        job.order = order.data();
        job.oldTime = oldTime;
        job.newTime = newTime;
        job.fpu = _control87(0, 0);
        job.mxcsr = _mm_getcsr();
        job.next.store(0, std::memory_order_relaxed);
        job.done.store(0, std::memory_order_relaxed);
        const auto helpers = static_cast<uint32_t>(std::min<size_t>(g_helperIds.size(), groups.size() - 1));
        g_parallel.store(true, std::memory_order_release);
        ReleaseSemaphore(g_start, static_cast<LONG>(helpers), nullptr);
        runGroups(job);
        // The helpers that woke late find no groups left and report right away.
        for (uint32_t spins = 0; job.done.load(std::memory_order_acquire) < helpers; ++spins)
        {
            if (spins < 4096)
            {
                _mm_pause();
            }
            else
            {
                SwitchToThread();
            }
        }
        g_parallel.store(false, std::memory_order_release);
    }

    // [Debug] AnimationCheck: the bytes the sampling may change (the controls, their skeletons' bones).
    struct Snapshot
    {
        std::vector<uint8_t> controls;
        std::vector<std::vector<uint8_t>> bones;
    };

    // The bytes of a control's target bones (all controls of a group share them).
    size_t boneBytes(const void* control)
    {
        const uintptr_t accumulate = (*static_cast<uintptr_t* const*>(control))[Control::accumulateSlot];
        const auto* skeleton = field<const uint8_t*>(control, accumulate == g_animationAccumulate ? Control::animationTarget
                                                                                                  : Control::poseTarget);
        return size_t(field<uint32_t>(skeleton, Skeleton::boneCount)) * Skeleton::boneSize;
    }

    void capture(Snapshot& s, const std::vector<Group>& groups, const std::vector<void*>& controls)
    {
        s.controls.resize(controls.size() * Control::size);
        for (size_t i = 0; i < controls.size(); ++i)
        {
            std::memcpy(s.controls.data() + i * Control::size, controls[i], Control::size);
        }
        s.bones.resize(groups.size());
        for (size_t g = 0; g < groups.size(); ++g)
        {
            if (groups[g].bones == kNoBones)
            {
                s.bones[g].clear();
                continue;
            }
            const size_t bytes = boneBytes(g_grouped[groups[g].first]);
            s.bones[g].assign(groups[g].bones, groups[g].bones + bytes);
        }
    }

    void restore(const Snapshot& s, const std::vector<Group>& groups, const std::vector<void*>& controls)
    {
        for (size_t i = 0; i < controls.size(); ++i)
        {
            std::memcpy(controls[i], s.controls.data() + i * Control::size, Control::size);
        }
        for (size_t g = 0; g < groups.size(); ++g)
        {
            if (!s.bones[g].empty())
            {
                std::memcpy(const_cast<uint8_t*>(groups[g].bones), s.bones[g].data(), s.bones[g].size());
            }
        }
    }

    // Samples on one thread from the state before, then on the threads from the same state, and compares: a difference
    // means some state is shared after all, and the walk stays on one thread from then on.
    bool check(const std::vector<Group>& groups, const std::vector<uint32_t>& order, const std::vector<void*>& controls,
        double oldTime, double newTime)
    {
        Snapshot before, serial, parallel;
        capture(before, groups, controls);
        for (const Group& group : groups)
        {
            for (void* control : controlsOf(group))
            {
                sample(control, oldTime, newTime);
            }
        }
        capture(serial, groups, controls);
        restore(before, groups, controls);
        runParallel(groups, order, oldTime, newTime);
        capture(parallel, groups, controls);
        g_stats.checks.fetch_add(1, std::memory_order_relaxed);
        size_t controlDiffs = 0, boneDiffs = 0;
        for (size_t i = 0; i < controls.size(); ++i)
        {
            controlDiffs += std::memcmp(serial.controls.data() + i * Control::size,
                parallel.controls.data() + i * Control::size, Control::size) != 0;
        }
        for (size_t g = 0; g < groups.size(); ++g)
        {
            boneDiffs += serial.bones[g] != parallel.bones[g];
        }
        if (!controlDiffs && !boneDiffs)
        {
            return true;
        }
        g_stats.checkFailures.fetch_add(1, std::memory_order_relaxed);
        LOG("Animation threads: check failed ({} of {} controls, {} of {} skeletons differ from one thread), staying on "
            "one thread", controlDiffs, controls.size(), boneDiffs, groups.size());
        return false;
    }

    // The original walk's second part per control: unlinked and deleted once it is no longer active and finished.
    void removeExpired(uint8_t* scene, double newTime)
    {
        auto* sentinel = reinterpret_cast<void**>(scene + Scene::controls);
        auto* node = static_cast<void**>(*sentinel);
        while (node != sentinel)
        {
            auto* next = static_cast<void**>(node[0]);
            auto* control = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(node[2]) & ~3u);
            if (!(g_active(control, nullptr, newTime) & 0xFF) && control[Control::finished])
            {
                uint32_t old = 0;
                void* current = node;
                g_unlink(sentinel, nullptr, &old, &current);
                if ((old & 1) == 1)
                {
                    auto* object = reinterpret_cast<void*>(old & ~3u);
                    if (object)
                    {
                        auto destroy = reinterpret_cast<DeleteFn>((*static_cast<uintptr_t* const*>(object))[0]);
                        destroy(object, nullptr, (old & 2) == 2 ? 3 : 1);
                    }
                }
            }
            node = next;
        }
    }

    void __fastcall hookControls(uint8_t* scene, void* edx, uint32_t oldLo, uint32_t oldHi)
    {
        if (!g_enabled || g_inWalk.exchange(true, std::memory_order_acquire))
        {
            g_origControls(scene, edx, oldLo, oldHi);
            return;
        }
        const int64_t start = qpc();
        double oldTime;
        const uint32_t oldParts[2] = {oldLo, oldHi};
        std::memcpy(&oldTime, oldParts, sizeof(oldTime));
        const double newTime = field<double>(scene, Scene::time);

        // The controls in list order, grouped by the bones they write.
        static std::vector<void*> controls;
        static std::vector<std::pair<const uint8_t*, uint32_t>> keys;   // bones, index into controls
        controls.clear();
        keys.clear();
        auto* sentinel = reinterpret_cast<void**>(scene + Scene::controls);
        bool known = true;
        for (auto* node = static_cast<void**>(*sentinel); node != sentinel; node = static_cast<void**>(node[0]))
        {
            auto* control = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(node[2]) & ~3u);
            const uintptr_t accumulate = (*reinterpret_cast<uintptr_t* const*>(control))[Control::accumulateSlot];
            const uint8_t* skeleton = accumulate == g_animationAccumulate ? field<const uint8_t*>(control, Control::animationTarget)
                : accumulate == g_poseAccumulate ? field<const uint8_t*>(control, Control::poseTarget)
                                                 : nullptr;
            const uint8_t* bones = accumulate == g_clockAccumulate ? kNoBones
                : skeleton ? field<const uint8_t*>(skeleton, Skeleton::bones) : nullptr;
            if (!bones)
            {
                // Another kind of control (IK controls read a second skeleton's bones): the original walk this time.
                known = false;
                g_stats.unknownWalks.fetch_add(1, std::memory_order_relaxed);
                if (accumulate != g_loggedUnknown)
                {
                    g_loggedUnknown = accumulate;
                    LOG("Animation threads: a control of another kind (sampling at granny.dll + {:x}{}), its walks stay "
                        "on one thread", accumulate - g_grannyBase, skeleton ? "" : ", no target");
                }
                break;
            }
            keys.emplace_back(bones, static_cast<uint32_t>(controls.size()));
            controls.push_back(control);
        }
        static std::vector<Group> groups;
        static std::vector<uint32_t> order;
        groups.clear();
        g_grouped.clear();
        if (known)
        {
            // By bones, in list order within (the index breaks the ties): no stable_sort, it allocates a buffer.
            std::sort(keys.begin(), keys.end());
            for (size_t i = 0; i < keys.size(); ++i)
            {
                if (i == 0 || keys[i].first != keys[i - 1].first)
                {
                    groups.push_back({keys[i].first, static_cast<uint32_t>(g_grouped.size()), 0});
                }
                g_grouped.push_back(controls[keys[i].second]);
                ++groups.back().count;
            }
        }
        g_stats.walks.fetch_add(1, std::memory_order_relaxed);
        if (!known || groups.size() < 2)
        {
            g_stats.serialWalks.fetch_add(1, std::memory_order_relaxed);
            g_inWalk.store(false, std::memory_order_release);
            g_origControls(scene, edx, oldLo, oldHi);
            g_stats.totalTicks.fetch_add(qpc() - start, std::memory_order_relaxed);
            return;
        }
        order.resize(groups.size());
        for (uint32_t i = 0; i < order.size(); ++i)
        {
            order[i] = i;
        }
        std::sort(order.begin(), order.end(), [](uint32_t a, uint32_t b)
            { return groups[a].count != groups[b].count ? groups[a].count > groups[b].count : a < b; });

        const int64_t parallelStart = qpc();
        if (g_config.animationCheck && static_cast<int>(GetTickCount() - g_nextCheck) >= 0)
        {
            g_nextCheck = GetTickCount() + 2000;
            if (!check(groups, order, controls, oldTime, newTime))
            {
                g_enabled = false;
            }
        }
        else
        {
            runParallel(groups, order, oldTime, newTime);
        }
        g_stats.parallelTicks.fetch_add(qpc() - parallelStart, std::memory_order_relaxed);
        removeExpired(scene, newTime);
        g_stats.controls.fetch_add(static_cast<uint32_t>(controls.size()), std::memory_order_relaxed);
        g_stats.groups.fetch_add(static_cast<uint32_t>(groups.size()), std::memory_order_relaxed);
        g_stats.totalTicks.fetch_add(qpc() - start, std::memory_order_relaxed);
        g_inWalk.store(false, std::memory_order_release);
    }
}

void GrannyParallel::install()
{
    int threads = g_config.animationThreads;
    if (threads == 0)
    {
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        threads = std::clamp(static_cast<int>(info.dwNumberOfProcessors) - 2, 1, 4);
    }
    threads = std::clamp(threads, 1, 16);
    if (threads <= 1)
    {
        return;
    }
    const char* who = "Animation threads";
    // The control walk (thiscall on the scene (double old time), ret 8) and what it calls.
    const uintptr_t walk = GrannyMesh::find(who, "Granny's control walk",
        "55 8B EC 83 E4 F8 83 EC 0C 53 8D 41 20 56 57 8B 38 89 4C 24 0C 3B F8", 0);
    // An animation control's sampling and blending (vtable slot 2), and the handle lock and release in it.
    const uintptr_t animation = GrannyMesh::find(who, "Granny's animation control sampling",
        "64 A1 00 00 00 00 6A FF 68 ?? ?? ?? ?? 50 64 89 25 00 00 00 00 83 EC 50 56 8B F1 8B 46 68 8A 40 6C 84 C0", 0);
    // A pose control's blending (vtable slot 2).
    const uintptr_t pose = GrannyMesh::find(who, "Granny's pose control blending",
        "83 EC 48 53 55 8B E9 8B 45 70 8A 48 6C 84 C9 0F 84", 0);
    // The constructor of the scene's timekeeping control: mov dword ptr [esi], vtable.
    const uintptr_t clockVtable = GrannyMesh::find(who, "Granny's timekeeping control",
        "33 C0 8B CE 50 89 44 24 14 89 46 60 89 46 64 C7 06 ?? ?? ?? ?? E8", 17);
    if (!walk || !animation || !pose || !clockVtable)
    {
        return;
    }
    g_grannyBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"granny.dll"));
    g_clockAccumulate = (*reinterpret_cast<const uintptr_t* const*>(clockVtable))[Control::accumulateSlot];
    g_active = reinterpret_cast<ActiveFn>(GrannyMesh::callTarget(walk + 0x33));
    g_sample = reinterpret_cast<SampleFn>(GrannyMesh::callTarget(walk + 0x52));
    g_unlink = reinterpret_cast<UnlinkFn>(GrannyMesh::callTarget(walk + 0x86));
    const uintptr_t active2 = GrannyMesh::callTarget(walk + 0x61);
    const uintptr_t lock = GrannyMesh::callTarget(animation + 0x34);
    const uintptr_t release = GrannyMesh::callTarget(animation + 0x135);
    if (!g_active || !g_sample || !g_unlink || active2 != reinterpret_cast<uintptr_t>(g_active) || !lock || !release)
    {
        LOG("{}: off, granny.dll's control walk is not the one expected", who);
        return;
    }
    g_animationAccumulate = animation;
    g_poseAccumulate = pose;

    g_start = CreateSemaphoreW(nullptr, 0, threads, nullptr);
    if (!g_start)
    {
        return;
    }
    for (int i = 1; i < threads; ++i)
    {
        DWORD id = 0;
        HANDLE thread = CreateThread(nullptr, 0, &helper, nullptr, 0, &id);
        if (!thread)
        {
            break;
        }
        SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL);
        CloseHandle(thread);
        g_helperIds.push_back(id);
        if (g_config.profiler)
        {
            Profiler::addThread(id);
        }
    }
    if (g_helperIds.empty())
    {
        return;
    }
    Patch::hook(g_origControls, walk, &hookControls, "granny control walk");
    Patch::hook(g_origLock, lock, &hookLock, "granny animation handle lock");
    Patch::hook(g_origRelease, release, &hookRelease, "granny animation handle release");
    g_enabled = true;
    LOG("{}: Granny's animation controls sampled on {} threads{}", who, g_helperIds.size() + 1,
        g_config.animationCheck ? ", checked against one thread every 2 s (AnimationCheck)" : "");
}

void GrannyParallel::onFrame()
{
    if (g_helperIds.empty() || !g_config.d3dStats)
    {
        return;
    }
    ++g_frames;
    static DWORD nextLog = 0;
    const DWORD now = GetTickCount();
    if (!nextLog)
    {
        nextLog = now + 5000;
        return;
    }
    if (static_cast<int>(now - nextLog) < 0)
    {
        return;
    }
    nextLog = now + 5000;
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    const double f = std::max<double>(g_frames, 1);
    const double ms = 1000.0 / static_cast<double>(freq.QuadPart);
    const uint32_t walks = g_stats.walks.exchange(0), serial = g_stats.serialWalks.exchange(0);
    const uint32_t parallelWalks = walks - serial;
    const double controls = g_stats.controls.exchange(0), groups = g_stats.groups.exchange(0);
    LOG("Animation threads: {:.0f} controls on {:.0f} skeletons per frame, sampled in {:.2f} ms, whole walk {:.2f} ms; "
        "{} of {} walks on one thread ({} with another kind of control); {} checks, {} failed; {} references kept",
        parallelWalks ? controls / parallelWalks : 0.0, parallelWalks ? groups / parallelWalks : 0.0,
        g_stats.parallelTicks.exchange(0) * ms / f, g_stats.totalTicks.exchange(0) * ms / f, serial, walks, g_stats.unknownWalks.exchange(0),
        g_stats.checks.exchange(0), g_stats.checkFailures.exchange(0), g_lostReferences.exchange(0));
    g_frames = 0;
}
