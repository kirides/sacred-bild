#include "game/file_io_stats.h"
#include "game/d3d_stats.h"
#include "log.h"
#include "patch.h"

#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cstdint>
#include <string>

namespace
{
    using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    using ReadFileFn = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);

    CreateFileAFn g_createFile = nullptr;
    ReadFileFn g_readFile = nullptr;

    constexpr double kSlowMs = 2.0;
    std::atomic<uint32_t> g_slowLogged{0};
    std::atomic<DWORD> g_slowLogSecond{0};

    double ticksPerMs()
    {
        static const double perMs = [] {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            return static_cast<double>(f.QuadPart) / 1000.0;
        }();
        return perMs;
    }

    int64_t qpc()
    {
        LARGE_INTEGER v;
        QueryPerformanceCounter(&v);
        return v.QuadPart;
    }

    // At most 20 slow calls logged per second.
    bool mayLog()
    {
        const DWORD second = GetTickCount() / 1000;
        if (g_slowLogSecond.exchange(second) != second)
        {
            g_slowLogged = 0;
        }
        return g_slowLogged++ < 20;
    }

    std::string pathOf(HANDLE file)
    {
        char path[MAX_PATH] = {};
        const DWORD n = GetFinalPathNameByHandleA(file, path, MAX_PATH, FILE_NAME_NORMALIZED);
        return n && n < MAX_PATH ? std::string(path) : std::string("?");
    }

    void account(int64_t tscStart)
    {
        if (D3DStats::isRenderThread())
        {
            D3DStats::addTime(D3DStats::TFileIo, D3DStats::now() - tscStart);
        }
    }

    HANDLE WINAPI hookCreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
        DWORD disposition, DWORD flags, HANDLE templateFile)
    {
        const int64_t start = qpc();
        const int64_t tscStart = D3DStats::now();
        const HANDLE h = g_createFile(name, access, share, security, disposition, flags, templateFile);
        account(tscStart);
        const double ms = static_cast<double>(qpc() - start) / ticksPerMs();
        if (ms >= kSlowMs && mayLog())
        {
            LOG("File I/O: CreateFileA {:.1f} ms ({} thread) {} from {:08x}", ms,
                D3DStats::isRenderThread() ? "render" : "other", name ? name : "?",
                reinterpret_cast<uintptr_t>(_ReturnAddress()));
        }
        return h;
    }

    BOOL WINAPI hookReadFile(HANDLE file, LPVOID buffer, DWORD bytes, LPDWORD read, LPOVERLAPPED overlapped)
    {
        const int64_t start = qpc();
        const int64_t tscStart = D3DStats::now();
        const BOOL ok = g_readFile(file, buffer, bytes, read, overlapped);
        account(tscStart);
        const double ms = static_cast<double>(qpc() - start) / ticksPerMs();
        if (ms >= kSlowMs && mayLog())
        {
            LOG("File I/O: ReadFile {:.1f} ms, {} bytes ({} thread) {} from {:08x}", ms, bytes,
                D3DStats::isRenderThread() ? "render" : "other", pathOf(file), reinterpret_cast<uintptr_t>(_ReturnAddress()));
        }
        return ok;
    }

    void patch(const char* importer)
    {
        Patch::iat(importer, "KERNEL32.dll", "CreateFileA", reinterpret_cast<void*>(&hookCreateFileA));
        Patch::iat(importer, "KERNEL32.dll", "ReadFile", reinterpret_cast<void*>(&hookReadFile));
    }
}

void FileIoStats::install()
{
    // The originals first, so a hook never runs without them.
    g_createFile = reinterpret_cast<CreateFileAFn>(GetProcAddress(GetModuleHandleA("KERNEL32.dll"), "CreateFileA"));
    g_readFile = reinterpret_cast<ReadFileFn>(GetProcAddress(GetModuleHandleA("KERNEL32.dll"), "ReadFile"));
    if (!g_createFile || !g_readFile)
    {
        return;
    }
    patch(nullptr);
    if (GetModuleHandleA("mss32.dll"))
    {
        patch("mss32.dll");
    }
    LOG("File I/O: CreateFileA and ReadFile of sacred.exe{} timed", GetModuleHandleA("mss32.dll") ? " and mss32.dll" : "");
}
