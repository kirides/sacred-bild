#include "game_data.h"

#include <windows.h>
#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>

namespace
{
    constexpr size_t kUiStride = 0x54;
    constexpr size_t kTmpHeader = 0x118, kTmpStride = 0x50, kEntryHeader = 0x50;

    struct TextureRecord
    {
        std::string name;
        int width = 0, height = 0;
        uint8_t type = 0;
        uint8_t slot = 0;
        uint32_t offset = 0;
    };

    std::vector<GameData::UiImage> g_ui;
    std::vector<TextureRecord> g_textures;
    std::map<std::string, int> g_byName;
    std::wstring g_pakDir;
    std::mutex g_fileLock;
    std::map<int, HANDLE> g_archives;      // slot -> file

    std::string upper(std::string s)
    {
        for (char& c : s)
        {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        return s;
    }

    bool readWhole(const std::wstring& path, std::vector<uint8_t>& out)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f)
        {
            return false;
        }
        f.seekg(0, std::ios::end);
        out.resize(static_cast<size_t>(f.tellg()));
        f.seekg(0);
        f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
        return static_cast<bool>(f);
    }

    template <class T>
    T at(const std::vector<uint8_t>& d, size_t offset)
    {
        T v{};
        std::memcpy(&v, d.data() + offset, sizeof(T));
        return v;
    }

    std::string cstr(const uint8_t* p, size_t max)
    {
        size_t n = 0;
        while (n < max && p[n])
        {
            ++n;
        }
        return std::string(reinterpret_cast<const char*>(p), n);
    }

    // The table: "INVALID" (id 0) ... "INVALID_MAX", ids counting up from 0 in +0x00.
    bool readUiTable(const std::vector<uint8_t>& exe)
    {
        static const char kEnd[] = "INVALID_MAX";
        const auto endIt = std::search(exe.begin(), exe.end(), kEnd, kEnd + sizeof(kEnd));    // with its NUL
        if (endIt == exe.end())
        {
            return false;
        }
        const size_t endRecord = static_cast<size_t>(endIt - exe.begin()) - 8;
        const uint32_t count = at<uint32_t>(exe, endRecord);
        if (count < 100 || count > 8192 || endRecord < count * kUiStride)
        {
            return false;
        }
        const size_t base = endRecord - count * kUiStride;
        if (at<uint32_t>(exe, base) != 0 || cstr(exe.data() + base + 8, 0x20) != "INVALID")
        {
            return false;
        }
        g_ui.assign(count, {});
        for (uint32_t i = 0; i < count; ++i)
        {
            const size_t r = base + i * kUiStride;
            if (i < 100 && at<uint32_t>(exe, r) != i)
            {
                return false;   // a few late records carry another id in +0x00 (authoring slips); the position is the id
            }
            GameData::UiImage& u = g_ui[i];
            u.sheet = cstr(exe.data() + r + 8, 0x20);
            u.left = static_cast<int>(at<float>(exe, r + 0x2C));
            u.top = static_cast<int>(at<float>(exe, r + 0x30));
            u.right = static_cast<int>(at<float>(exe, r + 0x34));
            u.bottom = static_cast<int>(at<float>(exe, r + 0x38));
            if (i > 0 && u.right >= u.left && u.bottom >= u.top)
            {
                u.width = u.right - u.left + 1;
                u.height = u.bottom - u.top + 1;
            }
        }
        return true;
    }

    std::wstring archivePath(int slot)
    {
        if (slot == 0)
        {
            return g_pakDir + L"texture.pak";
        }
        wchar_t name[32];
        swprintf_s(name, L"Texture%02d.pak", slot - 1);
        return g_pakDir + name;
    }

    bool readAt(int slot, uint64_t offset, void* out, size_t size)
    {
        std::scoped_lock lock(g_fileLock);
        HANDLE& h = g_archives[slot];
        if (!h)
        {
            h = CreateFileW(archivePath(slot).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
        if (h == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        LARGE_INTEGER pos;
        pos.QuadPart = static_cast<LONGLONG>(offset);
        DWORD got = 0;
        return SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) && ReadFile(h, out, static_cast<DWORD>(size), &got, nullptr) && got == size;
    }

    uint32_t from4444(uint16_t v)
    {
        const uint32_t a = (v >> 12) & 15, r = (v >> 8) & 15, g = (v >> 4) & 15, b = v & 15;
        return (a * 17) << 24 | (r * 17) << 16 | (g * 17) << 8 | (b * 17);
    }
}

bool GameData::open(const std::wstring& gameDir, std::string& error)
{
    std::wstring dir = gameDir;
    if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/')
    {
        dir += L'\\';
    }
    std::vector<uint8_t> exe, tmp;
    if (!readWhole(dir + L"Sacred.exe", exe))
    {
        error = "cannot read Sacred.exe";
        return false;
    }
    if (!readUiTable(exe))
    {
        error = "no UI image table in Sacred.exe";
        return false;
    }
    g_pakDir = dir + L"PAK\\";
    if (!readWhole(g_pakDir + L"Texture.TMP", tmp) || tmp.size() < kTmpHeader)
    {
        error = "cannot read PAK\\Texture.TMP";
        return false;
    }
    const size_t count = (tmp.size() - kTmpHeader) / kTmpStride;
    g_textures.assign(count, {});
    for (size_t i = 0; i < count; ++i)
    {
        const size_t r = kTmpHeader + i * kTmpStride;
        TextureRecord& t = g_textures[i];
        t.name = cstr(tmp.data() + r, 0x20);
        t.width = at<uint16_t>(tmp, r + 0x20);
        t.height = at<uint16_t>(tmp, r + 0x22);
        t.type = tmp[r + 0x24];
        t.slot = tmp[r + 0x29];
        t.offset = at<uint32_t>(tmp, r + 0x2A);
        if (!t.name.empty())
        {
            g_byName.emplace(upper(t.name), static_cast<int>(i));
        }
    }
    return true;
}

const std::vector<GameData::UiImage>& GameData::uiImages()
{
    return g_ui;
}

int GameData::textureIndex(const std::string& name)
{
    const auto it = g_byName.find(upper(name));
    return it == g_byName.end() ? -1 : it->second;
}

int GameData::textureCount()
{
    return static_cast<int>(g_textures.size());
}

std::string GameData::textureName(int index)
{
    return index >= 0 && index < textureCount() ? g_textures[index].name : std::string();
}

bool GameData::textureSize(int index, int& width, int& height)
{
    if (index < 0 || index >= textureCount() || !g_textures[index].width)
    {
        return false;
    }
    width = g_textures[index].width;
    height = g_textures[index].height;
    return true;
}

bool GameData::loadTexture(int index, Pixels& out, std::string& error)
{
    if (index < 0 || index >= textureCount() || !g_textures[index].width || !g_textures[index].height)
    {
        error = "no such texture";
        return false;
    }
    const TextureRecord& t = g_textures[index];
    uint8_t header[kEntryHeader];
    if (!readAt(t.slot, t.offset, header, sizeof(header)))
    {
        error = "cannot read the archive entry";
        return false;
    }
    uint32_t encoded = 0;
    std::memcpy(&encoded, header + 0x25, 4);
    const uint8_t type = header[0x24];
    const size_t pixels = static_cast<size_t>(t.width) * t.height;
    out.width = t.width;
    out.height = t.height;
    out.argb.assign(pixels, 0);
    if (type == 0 || type == 1 || type == 2)
    {
        std::vector<uint16_t> raw(pixels);
        if (!readAt(t.slot, t.offset + kEntryHeader, raw.data(), raw.size() * 2))
        {
            error = "truncated texture";
            return false;
        }
        std::transform(raw.begin(), raw.end(), out.argb.begin(), from4444);
        return true;
    }
    if (type == 6)
    {
        if (!readAt(t.slot, t.offset + kEntryHeader, out.argb.data(), pixels * 4))
        {
            error = "truncated texture";
            return false;
        }
        return true;
    }
    if (type == 4)
    {
        // The stream ends where it ends: `encoded` may hold the unpacked size instead of the packed one, so read a generous amount (an
        // inflate stops at its end marker) and accept a read that is cut short by the end of the file.
        size_t want = std::max<size_t>(encoded, 1024);
        std::vector<uint16_t> raw(pixels);
        for (int attempt = 0; attempt < 3; ++attempt, want *= 2)
        {
            std::vector<uint8_t> packed(want);
            size_t have = want;
            while (have > 0 && !readAt(t.slot, t.offset + kEntryHeader, packed.data(), have))
            {
                have /= 2;     // near the end of the archive
            }
            z_stream z{};
            if (inflateInit(&z) != Z_OK)
            {
                break;
            }
            z.next_in = packed.data();
            z.avail_in = static_cast<uInt>(have);
            z.next_out = reinterpret_cast<Bytef*>(raw.data());
            z.avail_out = static_cast<uInt>(raw.size() * 2);
            const int rc = inflate(&z, Z_FINISH);
            const bool full = z.avail_out == 0;
            inflateEnd(&z);
            if (rc == Z_STREAM_END || full)
            {
                std::transform(raw.begin(), raw.end(), out.argb.begin(), from4444);
                return true;
            }
            if (rc != Z_BUF_ERROR)
            {
                break;
            }
        }
        error = "cannot inflate the texture";
        return false;
    }
    error = "storage type " + std::to_string(type) + " is not supported";
    return false;
}
