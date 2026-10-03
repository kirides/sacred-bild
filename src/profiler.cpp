#include "profiler.h"
#include "config.h"
#include "log.h"

#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr int kStackDwords = 1024;  // scanned for return addresses
    constexpr int kMaxFrames = 24;      // return addresses kept per sample

    struct Module
    {
        uintptr_t lo, hi;
        std::string name;
    };

    std::atomic<bool> g_running{false};
    HANDLE g_thread = nullptr;
    std::wstring g_reportPath;

    // Target thread; swapped by retarget() while the sampler runs.
    std::atomic<HANDLE> g_target{nullptr};
    std::atomic<DWORD> g_targetId{0};

    uintptr_t g_exeLo = 0, g_exeHi = 0;     // sacred.exe .text
    std::vector<Module> g_modules;

    struct ThreadProfile
    {
        // EIP -> samples (sacred.exe), and module!caller -> samples (outside sacred.exe).
        std::unordered_map<uintptr_t, uint32_t> exclusive;
        std::unordered_map<uint64_t, uint32_t> external;
        // module << 32 | EIP offset in the module -> samples (outside sacred.exe).
        std::unordered_map<uint64_t, uint32_t> externalEip;
        // Return address -> samples it appeared in (approximate inclusive time).
        std::unordered_map<uintptr_t, uint32_t> inclusive;
        uint32_t samples = 0;
    };
    std::unordered_map<DWORD, ThreadProfile> g_profiles;     // only touched by the sampler thread

    void loadModules()
    {
        g_modules.clear();
        HMODULE mods[512];
        DWORD needed = 0;
        if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed))
        {
            return;
        }
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&loadModules), &self);
        for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 512; ++i)
        {
            MODULEINFO mi = {};
            char name[MAX_PATH] = {};
            GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi));
            GetModuleBaseNameA(GetCurrentProcess(), mods[i], name, MAX_PATH);
            const auto lo = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
            // Our ddraw.dll and the system's share a name.
            g_modules.push_back({lo, lo + mi.SizeOfImage, mods[i] == self ? std::string("SacredBild") : std::string(name)});
        }
    }

    int moduleIndex(uintptr_t addr)
    {
        for (size_t i = 0; i < g_modules.size(); ++i)
        {
            if (addr >= g_modules[i].lo && addr < g_modules[i].hi) return static_cast<int>(i);
        }
        return -1;
    }

    bool inExe(uintptr_t a) { return a >= g_exeLo && a < g_exeHi; }

    // Heuristic: the bytes before `ra` encode a call instruction.
    bool isReturnAddress(uintptr_t ra)
    {
        if (!inExe(ra) || ra < g_exeLo + 7) return false;
        const auto* p = reinterpret_cast<const uint8_t*>(ra);
        if (p[-5] == 0xE8) return true;                                    // call rel32
        if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) return true;          // call [disp32] / [reg+disp32]
        if (p[-3] == 0xFF && (p[-2] & 0xF8) == 0x50) return true;          // call [reg+disp8]
        if (p[-2] == 0xFF && ((p[-1] & 0xF8) == 0xD0 || (p[-1] & 0xF8) == 0x10)) return true;  // call reg / [reg]
        if (p[-7] == 0xFF && p[-6] == 0x14) return true;                   // call [sib+disp32]
        if (p[-4] == 0xFF && p[-3] == 0x54) return true;                   // call [sib+disp8]
        return false;
    }

    struct Sample
    {
        uintptr_t eip;
        int frames;
        uintptr_t ra[kMaxFrames];
    };

    bool takeSample(HANDLE target, Sample& s, uint32_t* stack)
    {
        CONTEXT ctx = {};
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (SuspendThread(target) == static_cast<DWORD>(-1)) return false;
        bool ok = GetThreadContext(target, &ctx) != FALSE;
        size_t words = 0;
        if (ok)
        {
            MEMORY_BASIC_INFORMATION mbi = {};
            if (VirtualQuery(reinterpret_cast<void*>(ctx.Esp), &mbi, sizeof(mbi)))
            {
                const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
                words = std::min<size_t>(kStackDwords, (end - ctx.Esp) / 4);
                memcpy(stack, reinterpret_cast<void*>(ctx.Esp), words * 4);
            }
        }
        ResumeThread(target);
        if (!ok) return false;

        s.eip = ctx.Eip;
        s.frames = 0;
        for (size_t i = 0; i < words && s.frames < kMaxFrames; ++i)
        {
            if (isReturnAddress(stack[i])) s.ra[s.frames++] = stack[i];
        }
        return true;
    }

    void record(ThreadProfile& p, const Sample& s)
    {
        ++p.samples;
        if (inExe(s.eip))
        {
            ++p.exclusive[s.eip];
        }
        else
        {
            int mod = moduleIndex(s.eip);
            if (mod < 0)
            {
                loadModules();
                mod = moduleIndex(s.eip);
            }
            const uintptr_t caller = s.frames ? s.ra[0] : 0;
            ++p.external[(static_cast<uint64_t>(static_cast<uint32_t>(mod)) << 32) | caller];
            if (mod >= 0)
            {
                ++p.externalEip[(static_cast<uint64_t>(mod) << 32) | (s.eip - g_modules[mod].lo)];
            }
        }
        // Count each return address once per sample.
        for (int i = 0; i < s.frames; ++i)
        {
            bool seen = false;
            for (int j = 0; j < i; ++j) seen |= s.ra[j] == s.ra[i];
            if (!seen) ++p.inclusive[s.ra[i]];
        }
    }

    void writeReport();

    DWORD WINAPI samplerThread(LPVOID)
    {
        static uint32_t stack[kStackDwords];
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        const LONGLONG interval = -10LL * std::max(100, g_config.profilerIntervalUs);
        ULONGLONG lastReport = GetTickCount64();
        while (g_running.load())
        {
            // Reports are written here, never from DllMain: exiting can kill this thread mid-allocation.
            if (GetTickCount64() - lastReport > 15000)
            {
                writeReport();
                lastReport = GetTickCount64();
            }
            if (timer)
            {
                LARGE_INTEGER due;
                due.QuadPart = interval;
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(timer, INFINITE);
            }
            else
            {
                Sleep(1);
            }
            Sample s;
            HANDLE target = g_target.load();
            if (target && takeSample(target, s, stack)) record(g_profiles[g_targetId.load()], s);
        }
        if (timer) CloseHandle(timer);
        writeReport();
        return 0;
    }

    template <class Map>
    auto sorted(const Map& m)
    {
        std::vector<std::pair<typename Map::key_type, uint32_t>> v(m.begin(), m.end());
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
        return v;
    }

    void writeReport()
    {
        FILE* f = _wfopen(g_reportPath.c_str(), L"w");
        if (!f) return;
        std::fprintf(f, "# SacredBild profile, interval %d us. Map addresses with tools/profile_report.py\n",
            g_config.profilerIntervalUs);
        for (auto& [tid, p] : g_profiles)
        {
            std::fprintf(f, "[thread %lu samples %u]\n", tid, p.samples);
            std::fprintf(f, "[exclusive]\n");
            for (auto& [eip, n] : sorted(p.exclusive)) std::fprintf(f, "%08x %u\n", static_cast<unsigned>(eip), n);
            std::fprintf(f, "[external]\n");
            for (auto& [key, n] : sorted(p.external))
            {
                const int mod = static_cast<int>(key >> 32);
                const char* name = mod >= 0 && mod < static_cast<int>(g_modules.size()) ? g_modules[mod].name.c_str() : "?";
                std::fprintf(f, "%s %08x %u\n", name, static_cast<unsigned>(key & 0xFFFFFFFF), n);
            }
            std::fprintf(f, "[external_eip]\n");
            for (auto& [key, n] : sorted(p.externalEip))
            {
                const int mod = static_cast<int>(key >> 32);
                const char* name = mod >= 0 && mod < static_cast<int>(g_modules.size()) ? g_modules[mod].name.c_str() : "?";
                std::fprintf(f, "%s %08x %u\n", name, static_cast<unsigned>(key & 0xFFFFFFFF), n);
            }
            std::fprintf(f, "[inclusive]\n");
            for (auto& [ra, n] : sorted(p.inclusive)) std::fprintf(f, "%08x %u\n", static_cast<unsigned>(ra), n);
        }
        std::fclose(f);
    }
}

void Profiler::retarget(unsigned long threadId)
{
    HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, threadId);
    if (!handle)
    {
        LOG("Profiler: OpenThread({}) failed", threadId);
        return;
    }
    // The sampler may still be using the old handle; it is leaked rather than closed under it.
    g_targetId = threadId;
    g_target = handle;
    if (g_running.exchange(true))
    {
        LOG("Profiler: now sampling thread {}", threadId);
        return;
    }

    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_reportPath = exe;
    g_reportPath = g_reportPath.substr(0, g_reportPath.find_last_of(L"\\/") + 1) + L"SacredBild-profile.txt";

    const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto* sec = IMAGE_FIRST_SECTION(nt);
    g_exeLo = reinterpret_cast<uintptr_t>(base) + sec->VirtualAddress;
    g_exeHi = g_exeLo + sec->Misc.VirtualSize;
    loadModules();

    g_thread = CreateThread(nullptr, 0, &samplerThread, nullptr, 0, nullptr);
    SetThreadPriority(g_thread, THREAD_PRIORITY_TIME_CRITICAL);
    LOG("Profiler: sampling thread {} every {} us", threadId, g_config.profilerIntervalUs);
}

void Profiler::stop()
{
    // The sampler writes a final report when it notices; on process exit it may already be gone.
    g_running = false;
}
