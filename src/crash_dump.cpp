#include "crash_dump.h"
#include "config.h"
#include "log.h"
#include "patch.h"

#include <windows.h>
#include <dbghelp.h>
#include <format>

namespace
{
    using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
        PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    using SetFilterFn = LPTOP_LEVEL_EXCEPTION_FILTER(WINAPI*)(LPTOP_LEVEL_EXCEPTION_FILTER);

    MiniDumpWriteDumpFn g_writeDump = nullptr;
    SetFilterFn g_origSetFilter = nullptr;
    LPTOP_LEVEL_EXCEPTION_FILTER g_next = nullptr;     // the filter the game (its CRT) set, called after the dump
    std::wstring g_dir;
    std::wstring g_prefix;
    volatile LONG g_crashed = 0;

    struct Crash
    {
        EXCEPTION_POINTERS* info;
        DWORD threadId;
    };

    std::string narrow(const std::wstring& s)
    {
        std::string out;
        for (wchar_t c : s)
        {
            out += c < 0x80 ? static_cast<char>(c) : '?';
        }
        return out;
    }

    // "module+offset" of a code address.
    std::string where(const void* address)
    {
        HMODULE module = nullptr;
        wchar_t path[MAX_PATH] = {};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                static_cast<LPCWSTR>(address), &module) && GetModuleFileNameW(module, path, MAX_PATH))
        {
            const wchar_t* name = wcsrchr(path, L'\\');
            return std::format("{}+{:x}", narrow(name ? name + 1 : path),
                reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module));
        }
        return std::format("{}", address);
    }

    // Runs on its own thread: MiniDumpWriteDump wants the crashed thread stopped, and a stack overflow leaves
    // that thread no stack to call it from.
    DWORD WINAPI writeDump(void* param)
    {
        const Crash& crash = *static_cast<Crash*>(param);
        SYSTEMTIME t;
        GetLocalTime(&t);
        const std::wstring path = std::format(L"{}\\{}-{:04}{:02}{:02}-{:02}{:02}{:02}.dmp", g_dir, g_prefix, t.wYear,
            t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        const DWORD full = MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithThreadInfo |
            MiniDumpWithUnloadedModules;
        const DWORD small = MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithHandleData | MiniDumpWithThreadInfo |
            MiniDumpWithUnloadedModules;
        bool written = false;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            MINIDUMP_EXCEPTION_INFORMATION exception = {crash.threadId, crash.info, FALSE};
            written = g_writeDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                static_cast<MINIDUMP_TYPE>(g_config.crashDump >= 2 ? full : small), &exception, nullptr, nullptr);
            CloseHandle(file);
            if (!written)
            {
                DeleteFileW(path.c_str());
            }
        }
        // After the dump: the crashed thread may hold the log's lock.
        const EXCEPTION_RECORD& record = *crash.info->ExceptionRecord;
        LOG("CRASH: exception {:08x} at {} (thread {}); {} {}", record.ExceptionCode, where(record.ExceptionAddress),
            crash.threadId, written ? "dump written to" : "no dump written to", narrow(path));
        return 0;
    }

    LONG WINAPI onCrash(EXCEPTION_POINTERS* info)
    {
        if (InterlockedExchange(&g_crashed, 1) == 0)
        {
            Crash crash = {info, GetCurrentThreadId()};
            if (HANDLE thread = CreateThread(nullptr, 0, &writeDump, &crash, 0, nullptr))
            {
                WaitForSingleObject(thread, 60000);
                CloseHandle(thread);
            }
        }
        return g_next ? g_next(info) : EXCEPTION_CONTINUE_SEARCH;
    }

    // Filters set after ours (the game's CRT sets one at startup) run after the dump instead of replacing it.
    LPTOP_LEVEL_EXCEPTION_FILTER WINAPI hookSetFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter)
    {
        return reinterpret_cast<LPTOP_LEVEL_EXCEPTION_FILTER>(
            InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&g_next), reinterpret_cast<void*>(filter)));
    }
}

void CrashDump::install(const std::wstring& dir, const wchar_t* prefix)
{
    if (g_config.crashDump <= 0)
    {
        return;
    }
    wchar_t system[MAX_PATH] = {};
    GetSystemDirectoryW(system, MAX_PATH);
    HMODULE dbghelp = LoadLibraryW((std::wstring(system) + L"\\dbghelp.dll").c_str());
    g_writeDump = dbghelp ? reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
    if (!g_writeDump)
    {
        LOG("Crash dumps: dbghelp.dll's MiniDumpWriteDump not found");
        return;
    }
    g_dir = dir;
    g_prefix = prefix;
    g_next = SetUnhandledExceptionFilter(&onCrash);
    g_origSetFilter = &SetUnhandledExceptionFilter;
    Patch::begin();
    Patch::hook(g_origSetFilter, &hookSetFilter, "SetUnhandledExceptionFilter");
    Patch::commit();
    LOG("Crash dumps: {} dumps to {}\\{}-*.dmp", g_config.crashDump >= 2 ? "full" : "small", narrow(dir), narrow(prefix));
}
