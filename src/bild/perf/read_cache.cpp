#include "read_cache.h"
#include "settings/config/debug.h"
#include "settings/config/render.h"
#include "log.h"
#include "patch.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <list>
#include <memory>
#include <unordered_map>

namespace
{
    using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    using ReadFileFn = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
    using SetFilePointerFn = DWORD(WINAPI*)(HANDLE, LONG, PLONG, DWORD);
    using CloseHandleFn = BOOL(WINAPI*)(HANDLE);

    // What was in the import slots before (another hook, or kernel32).
    CreateFileAFn g_createFile = nullptr;
    ReadFileFn g_readFile = nullptr;
    SetFilePointerFn g_setFilePointer = nullptr;
    CloseHandleFn g_closeHandle = nullptr;

    constexpr uint32_t kBlockBits = 16;                     // 64 KB blocks
    constexpr uint32_t kBlockSize = 1u << kBlockBits;
    constexpr DWORD kCachedReadMax = 16 * 1024;             // larger reads go to Windows
    constexpr size_t kMaxBlocks = (32u << 20) / kBlockSize; // 32 MB for all files

    struct Block
    {
        HANDLE file;
        uint64_t index;
        uint32_t size;                                      // less than kBlockSize at the end of the file
        std::unique_ptr<uint8_t[]> data;
    };

    struct File
    {
        uint64_t position = 0;
        uint64_t size = 0;
        bool osPositionValid = true;                        // Windows' file position is `position`
        std::unordered_map<uint64_t, std::list<Block>::iterator> blocks;
    };

    // One lock for all: the game reads these files from its render thread and its loader threads, never much at once.
    SRWLOCK g_lock = SRWLOCK_INIT;
    std::unordered_map<HANDLE, File> g_files;
    std::list<Block> g_lru;                                 // most recently used first

    std::atomic<uint32_t> g_memoryReads{0}, g_systemReads{0}, g_seeks{0};
    std::atomic<ULONGLONG> g_nextLog{0};

    // [Debug] D3DStats: what the cache did, every ten seconds.
    void maybeLog()
    {
        if (!Config::debug.d3dStats)
        {
            return;
        }
        const ULONGLONG now = GetTickCount64();
        ULONGLONG next = g_nextLog.load();
        if (now >= next && g_nextLog.compare_exchange_strong(next, now + 10000))
        {
            ReadCache::logStats();
        }
    }

    void dropBlocks(File& file)
    {
        for (auto& [index, it] : file.blocks)
        {
            g_lru.erase(it);
        }
        file.blocks.clear();
    }

    // The block `index` of `file`, read on a miss (one positioned read); null if Windows refuses. Lock held.
    const Block* block(HANDLE handle, File& file, uint64_t index)
    {
        if (const auto it = file.blocks.find(index); it != file.blocks.end())
        {
            g_lru.splice(g_lru.begin(), g_lru, it->second);
            return &*it->second;
        }
        const uint64_t offset = index << kBlockBits;
        const uint32_t size = static_cast<uint32_t>(std::min<uint64_t>(kBlockSize, file.size - offset));
        Block b{handle, index, size, std::make_unique<uint8_t[]>(size)};
        OVERLAPPED at = {};
        at.Offset = static_cast<DWORD>(offset);
        at.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD read = 0;
        ++g_systemReads;
        if (!g_readFile(handle, b.data.get(), size, &read, &at) || read != size)
        {
            return nullptr;
        }
        file.osPositionValid = false;                       // a positioned read moves Windows' position too
        if (g_lru.size() >= kMaxBlocks)
        {
            const Block& old = g_lru.back();
            g_files[old.file].blocks.erase(old.index);
            g_lru.pop_back();
        }
        g_lru.push_front(std::move(b));
        file.blocks[index] = g_lru.begin();
        return &g_lru.front();
    }

    // Windows' position set to ours (before a read that goes to Windows). Lock held.
    bool syncPosition(HANDLE handle, File& file)
    {
        if (file.osPositionValid)
        {
            return true;
        }
        LONG high = static_cast<LONG>(file.position >> 32);
        const DWORD low = g_setFilePointer(handle, static_cast<LONG>(file.position), &high, FILE_BEGIN);
        if (low == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR)
        {
            return false;
        }
        file.osPositionValid = true;
        return true;
    }

    HANDLE WINAPI hookCreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
        DWORD disposition, DWORD flags, HANDLE templateFile)
    {
        const HANDLE h = g_createFile(name, access, share, security, disposition, flags, templateFile);
        // Files opened to read only, synchronously, that exist: their contents don't change under the game.
        const DWORD writeAccess = GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA;
        if (h != INVALID_HANDLE_VALUE && !(access & writeAccess) && (access & (GENERIC_READ | FILE_READ_DATA)) &&
            disposition == OPEN_EXISTING && !(flags & (FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING)) &&
            GetFileType(h) == FILE_TYPE_DISK)
        {
            LARGE_INTEGER size = {};
            if (GetFileSizeEx(h, &size))
            {
                AcquireSRWLockExclusive(&g_lock);
                File& file = g_files[h];
                dropBlocks(file);        // a handle value used before
                file = File{};
                file.size = static_cast<uint64_t>(size.QuadPart);
                ReleaseSRWLockExclusive(&g_lock);
            }
        }
        return h;
    }

    BOOL WINAPI hookReadFile(HANDLE handle, LPVOID buffer, DWORD bytes, LPDWORD read, LPOVERLAPPED overlapped)
    {
        if (overlapped || !read || !buffer)
        {
            return g_readFile(handle, buffer, bytes, read, overlapped);
        }
        AcquireSRWLockExclusive(&g_lock);
        const auto it = g_files.find(handle);
        if (it == g_files.end())
        {
            ReleaseSRWLockExclusive(&g_lock);
            return g_readFile(handle, buffer, bytes, read, overlapped);
        }
        File& file = it->second;
        if (bytes <= kCachedReadMax)
        {
            const uint64_t available = file.position < file.size ? file.size - file.position : 0;
            const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(bytes, available));
            uint32_t done = 0;
            while (done < n)
            {
                const uint64_t at = file.position + done;
                const Block* b = block(handle, file, at >> kBlockBits);
                if (!b)
                {
                    break;
                }
                const uint32_t offset = static_cast<uint32_t>(at & (kBlockSize - 1));
                const uint32_t part = std::min(n - done, b->size - offset);
                std::memcpy(static_cast<uint8_t*>(buffer) + done, b->data.get() + offset, part);
                done += part;
            }
            if (done == n)
            {
                file.position += n;
                file.osPositionValid = file.osPositionValid && n == 0;
                *read = n;
                ++g_memoryReads;
                ReleaseSRWLockExclusive(&g_lock);
                maybeLog();
                return TRUE;
            }
        }
        // To Windows, from our position.
        BOOL ok = FALSE;
        if (syncPosition(handle, file))
        {
            ok = g_readFile(handle, buffer, bytes, read, nullptr);
            file.position += ok ? *read : 0;
            ++g_systemReads;
        }
        ReleaseSRWLockExclusive(&g_lock);
        return ok;
    }

    DWORD WINAPI hookSetFilePointer(HANDLE handle, LONG distance, PLONG distanceHigh, DWORD method)
    {
        AcquireSRWLockExclusive(&g_lock);
        const auto it = g_files.find(handle);
        if (it == g_files.end() || method > FILE_END)
        {
            ReleaseSRWLockExclusive(&g_lock);
            return g_setFilePointer(handle, distance, distanceHigh, method);
        }
        File& file = it->second;
        const int64_t move = distanceHigh ? (static_cast<int64_t>(*distanceHigh) << 32) | static_cast<uint32_t>(distance)
                                          : static_cast<int64_t>(distance);
        const int64_t base = method == FILE_BEGIN ? 0 : method == FILE_CURRENT ? static_cast<int64_t>(file.position)
                                                                                : static_cast<int64_t>(file.size);
        const int64_t target = base + move;
        if (target < 0)
        {
            ReleaseSRWLockExclusive(&g_lock);
            SetLastError(ERROR_NEGATIVE_SEEK);
            return INVALID_SET_FILE_POINTER;
        }
        if (static_cast<uint64_t>(target) != file.position)
        {
            file.position = static_cast<uint64_t>(target);
            file.osPositionValid = false;
        }
        ++g_seeks;
        ReleaseSRWLockExclusive(&g_lock);
        if (distanceHigh)
        {
            *distanceHigh = static_cast<LONG>(static_cast<uint64_t>(target) >> 32);
        }
        SetLastError(NO_ERROR);
        return static_cast<DWORD>(target);
    }

    BOOL WINAPI hookCloseHandle(HANDLE handle)
    {
        AcquireSRWLockExclusive(&g_lock);
        if (const auto it = g_files.find(handle); it != g_files.end())
        {
            dropBlocks(it->second);
            g_files.erase(it);
        }
        ReleaseSRWLockExclusive(&g_lock);
        return g_closeHandle(handle);
    }
}

void ReadCache::install()
{
    if (!Config::render.readCache)
    {
        return;
    }
    const HMODULE kernel = GetModuleHandleA("KERNEL32.dll");
    // The slots' current targets first: a hook never runs without them.
    g_createFile = reinterpret_cast<CreateFileAFn>(Patch::importTarget("KERNEL32.dll", "CreateFileA"));
    g_readFile = reinterpret_cast<ReadFileFn>(Patch::importTarget("KERNEL32.dll", "ReadFile"));
    g_setFilePointer = reinterpret_cast<SetFilePointerFn>(Patch::importTarget("KERNEL32.dll", "SetFilePointer"));
    g_closeHandle = reinterpret_cast<CloseHandleFn>(Patch::importTarget("KERNEL32.dll", "CloseHandle"));
    if (!kernel || !g_createFile || !g_readFile || !g_setFilePointer || !g_closeHandle)
    {
        LOG("Read cache: sacred.exe's file imports not found; reads go to Windows");
        return;
    }
    Patch::iat("KERNEL32.dll", "CreateFileA", reinterpret_cast<void*>(&hookCreateFileA));
    Patch::iat("KERNEL32.dll", "ReadFile", reinterpret_cast<void*>(&hookReadFile));
    Patch::iat("KERNEL32.dll", "SetFilePointer", reinterpret_cast<void*>(&hookSetFilePointer));
    Patch::iat("KERNEL32.dll", "CloseHandle", reinterpret_cast<void*>(&hookCloseHandle));
    LOG("Read cache: small reads of sacred.exe's read-only files from 64 KB blocks in memory");
}

void ReadCache::logStats()
{
    const uint32_t memory = g_memoryReads.exchange(0), system = g_systemReads.exchange(0), seeks = g_seeks.exchange(0);
    if (memory || system)
    {
        LOG("Read cache: {} reads from memory, {} seeks kept, {} reads from Windows", memory, seeks, system);
    }
}
