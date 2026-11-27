#include "rhi/pipeline_cache.h"
#include "log.h"

#include <windows.h>

#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace
{
    constexpr uint32_t kMagic = 0x43504253;     // "SBPC"
    constexpr uint32_t kVersion = 1;

    struct Header
    {
        uint32_t magic, version;
        uint32_t buildTime, buildSize;          // this ddraw.dll's link time stamp and image size
        uint32_t api, vendor, device, pad;
        uint64_t driver, extra;
        uint64_t size, hash;                    // the data after the header
    };
    static_assert(sizeof(Header) == 64);

    // This module's PE header: its link time stamp and size change with every build.
    void build(uint32_t& time, uint32_t& size)
    {
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&build), &module);
        const auto* base = reinterpret_cast<const uint8_t*>(module);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        time = nt->FileHeader.TimeDateStamp;
        size = nt->OptionalHeader.SizeOfImage;
    }

    std::wstring path(uint32_t api)
    {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir(exe);
        const size_t slash = dir.find_last_of(L"\\/");
        dir = slash == std::wstring::npos ? L"" : dir.substr(0, slash + 1);
        return dir + (api == 1 ? L"SacredBild-pipelines-vulkan.bin" : L"SacredBild-pipelines-d3d12.bin");
    }

    Header headerFor(const Rhi::PipelineCache::Identity& id)
    {
        Header h = {};
        h.magic = kMagic;
        h.version = kVersion;
        build(h.buildTime, h.buildSize);
        h.api = id.api;
        h.vendor = id.vendor;
        h.device = id.device;
        h.driver = id.driver;
        h.extra = id.extra;
        return h;
    }

    std::mutex g_writeLock;     // one write at a time
}

uint64_t Rhi::PipelineCache::hash(const void* data, size_t size)
{
    uint64_t h = 1469598103934665603ull;
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i)
    {
        h = (h ^ bytes[i]) * 1099511628211ull;
    }
    return h;
}

std::vector<uint8_t> Rhi::PipelineCache::load(const Identity& id)
{
    const std::wstring file = path(id.api);
    HANDLE f = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
    {
        return {};
    }
    std::vector<uint8_t> data;
    Header h = {};
    DWORD read = 0;
    LARGE_INTEGER size = {};
    const char* why = nullptr;
    if (!GetFileSizeEx(f, &size) || size.QuadPart < static_cast<LONGLONG>(sizeof(Header)) || size.QuadPart > (256ll << 20) ||
        !ReadFile(f, &h, sizeof(h), &read, nullptr) || read != sizeof(h))
    {
        why = "unreadable";
    }
    else
    {
        const Header want = headerFor(id);
        if (h.magic != kMagic || h.version != kVersion)
        {
            why = "not a cache of this version";
        }
        else if (h.buildTime != want.buildTime || h.buildSize != want.buildSize)
        {
            why = "made by another SacredBild build";
        }
        else if (h.api != want.api || h.vendor != want.vendor || h.device != want.device || h.driver != want.driver ||
            h.extra != want.extra)
        {
            why = "made for another GPU or driver";
        }
        else if (h.size != static_cast<uint64_t>(size.QuadPart) - sizeof(Header))
        {
            why = "incomplete";
        }
        else
        {
            data.resize(static_cast<size_t>(h.size));
            if (!ReadFile(f, data.data(), static_cast<DWORD>(data.size()), &read, nullptr) || read != data.size() ||
                hash(data.data(), data.size()) != h.hash)
            {
                why = "damaged";
                data.clear();
            }
        }
    }
    CloseHandle(f);
    if (why)
    {
        LOG("RHI: pipeline cache discarded ({}), made again as pipelines are needed", why);
        DeleteFileW(file.c_str());
        return {};
    }
    LOG("RHI: pipeline cache loaded ({} KB)", data.size() / 1024);
    return data;
}

void Rhi::PipelineCache::save(const Identity& id, std::vector<uint8_t> data)
{
    Header h = headerFor(id);
    h.size = data.size();
    h.hash = hash(data.data(), data.size());
    std::thread([h, data = std::move(data)] {
        std::scoped_lock lock(g_writeLock);
        const std::wstring file = path(h.api);
        const std::wstring temp = file + L".tmp";
        HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE)
        {
            return;     // a read-only game folder: no cache
        }
        DWORD written = 0, written2 = 0;
        const bool ok = WriteFile(f, &h, sizeof(h), &written, nullptr) && written == sizeof(h) &&
            WriteFile(f, data.data(), static_cast<DWORD>(data.size()), &written2, nullptr) && written2 == data.size() &&
            FlushFileBuffers(f);
        CloseHandle(f);
        if (!ok || !MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            DeleteFileW(temp.c_str());
            return;
        }
        LOG("RHI: pipeline cache saved ({} KB)", data.size() / 1024);
    }).detach();
}
