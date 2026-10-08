#include "game/file_cache.h"
#include "config/render.h"
#include "log.h"

#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace
{
    struct File
    {
        std::wstring path;
        uint64_t size = 0;
        int rank = 0;       // lower first
    };

    // Sound samples are read most often while playing, music streams least.
    int rankOf(const std::wstring& dir, const std::wstring& name)
    {
        std::wstring lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
        if (dir == L"PAK" && lower.rfind(L"sound", 0) == 0) return 0;
        if (dir == L"PAK" || dir == L"World") return 1;
        return 2;
    }

    void collect(const std::wstring& root, const std::wstring& dir, std::vector<File>& out)
    {
        WIN32_FIND_DATAW fd;
        const std::wstring base = root + dir + L"\\";
        HANDLE find = FindFirstFileW((base + L"*").c_str(), &fd);
        if (find == INVALID_HANDLE_VALUE)
        {
            return;
        }
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                continue;
            }
            const uint64_t size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            out.push_back({base + fd.cFileName, size, rankOf(dir, fd.cFileName)});
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }

    DWORD WINAPI warmThread(LPVOID)
    {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);   // low CPU and I/O priority

        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring root = exe;
        root = root.substr(0, root.find_last_of(L"\\/") + 1);
        std::vector<File> files;
        for (const wchar_t* dir : {L"PAK", L"World", L"mp3"})
        {
            collect(root, dir, files);
        }
        std::stable_sort(files.begin(), files.end(), [](const File& a, const File& b) { return a.rank < b.rank; });

        MEMORYSTATUSEX mem = {};
        mem.dwLength = sizeof(mem);
        GlobalMemoryStatusEx(&mem);
        const uint64_t budget = mem.ullAvailPhys / 4;

        const DWORD start = GetTickCount();
        uint64_t total = 0;
        int count = 0, skipped = 0;
        std::vector<uint8_t> buffer(1 << 20);
        for (const File& f : files)
        {
            if (total + f.size > budget)
            {
                ++skipped;
                continue;
            }
            HANDLE h = CreateFileW(f.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                continue;
            }
            DWORD read = 0;
            while (ReadFile(h, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read)
            {
                total += read;
            }
            CloseHandle(h);
            ++count;
        }
        LOG("File cache: {} files ({} MB) read into Windows' file cache in {:.1f} s{}", count, total >> 20,
            (GetTickCount() - start) / 1000.0,
            skipped ? Fmt::format(", {} left out (budget {} MB)", skipped, budget >> 20) : std::string());
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
        return 0;
    }
}

void FileCache::install()
{
    if (!Config::render.warmFileCache)
    {
        return;
    }
    if (HANDLE thread = CreateThread(nullptr, 0, &warmThread, nullptr, 0, nullptr))
    {
        CloseHandle(thread);
    }
}
