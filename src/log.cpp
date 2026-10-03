#include "log.h"

#include <windows.h>
#include <cstdio>
#include <mutex>

namespace
{
    std::mutex g_mutex;
    FILE* g_file = nullptr;
}

void Log::init(const wchar_t* path)
{
    std::scoped_lock lock(g_mutex);
    if (!g_file)
    {
        // Keep the two previous runs' logs (SacredBild.log -> .prev.log -> .prev2.log).
        auto with = [&](const wchar_t* suffix) {
            std::wstring name = path;
            const size_t dot = name.rfind(L'.');
            name.insert(dot == std::wstring::npos ? name.size() : dot, suffix);
            return name;
        };
        MoveFileExW(with(L".prev").c_str(), with(L".prev2").c_str(), MOVEFILE_REPLACE_EXISTING);
        MoveFileExW(path, with(L".prev").c_str(), MOVEFILE_REPLACE_EXISTING);
        g_file = _wfsopen(path, L"w", _SH_DENYWR);
    }
}

void Log::write(std::string_view line)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::scoped_lock lock(g_mutex);
    if (!g_file)
    {
        return;
    }
    std::fprintf(g_file, "%02u:%02u:%02u.%03u [%5lu] %.*s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
        GetCurrentThreadId(), static_cast<int>(line.size()), line.data());
    std::fflush(g_file);
}
