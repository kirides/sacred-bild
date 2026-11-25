#include "vfs.h"

#include <windows.h>

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <map>
#include <mutex>

namespace Gear::Vfs
{
    namespace
    {
        uint16_t rd16(const uint8_t* p)
        {
            return static_cast<uint16_t>(p[0] | p[1] << 8);
        }
        uint32_t rd32(const uint8_t* p)
        {
            return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
        }

        std::string key(std::string rel)
        {
            for (char& c : rel)
            {
                c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            while (rel.size() > 1 && rel[0] == '.' && rel[1] == '/')
            {
                rel.erase(0, 2);
            }
            return rel;
        }

        constexpr size_t kMaxFile = 256u * 1024 * 1024;     // what one file of a pack may unpack to
    }

    Pack::~Pack()
    {
        if (m_data)
        {
            UnmapViewOfFile(m_data);
        }
        if (m_mapping)
        {
            CloseHandle(static_cast<HANDLE>(m_mapping));
        }
        if (m_file && m_file != INVALID_HANDLE_VALUE)
        {
            CloseHandle(static_cast<HANDLE>(m_file));
        }
    }

    std::unique_ptr<Pack> Pack::open(const std::filesystem::path& file, std::string& error)
    {
        std::unique_ptr<Pack> pack(new Pack());
        HANDLE f = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE)
        {
            error = "cannot open";
            return nullptr;
        }
        pack->m_file = f;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(f, &size) || size.QuadPart < 22 || size.QuadPart > 0xFFFFFFF0ll)
        {
            error = "bad size";
            return nullptr;
        }
        HANDLE mapping = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping)
        {
            error = "cannot map";
            return nullptr;
        }
        pack->m_mapping = mapping;
        pack->m_data = static_cast<const uint8_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
        if (!pack->m_data)
        {
            error = "cannot map";
            return nullptr;
        }
        pack->m_size = static_cast<size_t>(size.QuadPart);
        const uint8_t* d = pack->m_data;
        // end of central directory: the last 22 + comment bytes
        size_t eocd = std::string::npos;
        for (size_t i = pack->m_size - 22, stop = pack->m_size > 22 + 65535 ? pack->m_size - 22 - 65535 : 0;; --i)
        {
            if (rd32(d + i) == 0x06054b50)
            {
                eocd = i;
                break;
            }
            if (i == stop)
            {
                break;
            }
        }
        if (eocd == std::string::npos)
        {
            error = "not a ZIP file";
            return nullptr;
        }
        const uint32_t entries = rd16(d + eocd + 10), dirSize = rd32(d + eocd + 12), dirOffset = rd32(d + eocd + 16);
        if (entries == 0xFFFF || dirOffset == 0xFFFFFFFF || size_t(dirOffset) + dirSize > pack->m_size)
        {
            error = "ZIP64 or damaged";
            return nullptr;
        }
        size_t p = dirOffset;
        for (uint32_t i = 0; i < entries; ++i)
        {
            if (p + 46 > pack->m_size || rd32(d + p) != 0x02014b50)
            {
                error = "damaged central directory";
                return nullptr;
            }
            const uint16_t flags = rd16(d + p + 8), method = rd16(d + p + 10), nameLen = rd16(d + p + 28), extraLen = rd16(d + p + 30),
                           commentLen = rd16(d + p + 32);
            Entry e;
            e.method = method;
            e.compressed = rd32(d + p + 20);
            e.size = rd32(d + p + 24);
            e.headerOffset = rd32(d + p + 42);
            if (p + 46 + nameLen > pack->m_size)
            {
                error = "damaged central directory";
                return nullptr;
            }
            std::string name(reinterpret_cast<const char*>(d + p + 46), nameLen);
            p += 46 + size_t(nameLen) + extraLen + commentLen;
            if ((flags & 1) || name.empty() || name.back() == '/' || (method != 0 && method != 8))
            {
                continue;       // encrypted, a folder or an unsupported method: not served
            }
            pack->m_entries[key(name)] = e;
        }
        return pack;
    }

    bool Pack::contains(const std::string& rel) const
    {
        return m_entries.count(key(rel)) != 0;
    }

    bool Pack::read(const std::string& rel, std::string& out) const
    {
        const auto it = m_entries.find(key(rel));
        if (it == m_entries.end())
        {
            return false;
        }
        const Entry& e = it->second;
        const size_t h = e.headerOffset;
        if (h + 30 > m_size || rd32(m_data + h) != 0x04034b50)
        {
            return false;
        }
        const size_t start = h + 30 + rd16(m_data + h + 26) + rd16(m_data + h + 28);
        if (start + e.compressed > m_size || e.size > kMaxFile)
        {
            return false;
        }
        if (e.method == 0)
        {
            if (e.compressed != e.size)
            {
                return false;
            }
            out.assign(reinterpret_cast<const char*>(m_data + start), e.size);
            return true;
        }
        out.resize(e.size);
        z_stream z{};
        if (inflateInit2(&z, -15) != Z_OK)
        {
            return false;
        }
        z.next_in = const_cast<Bytef*>(m_data + start);
        z.avail_in = e.compressed;
        z.next_out = reinterpret_cast<Bytef*>(out.data());
        z.avail_out = e.size;
        const int rc = inflate(&z, Z_FINISH);
        inflateEnd(&z);
        return rc == Z_STREAM_END && z.total_out == e.size;
    }

    namespace
    {
        std::mutex g_lock;
        std::map<std::wstring, std::vector<std::unique_ptr<Pack>>> g_mounted;     // lower-cased root -> its packs

        std::wstring rootKey(const std::filesystem::path& root)
        {
            std::wstring s = root.lexically_normal().wstring();
            for (wchar_t& c : s)
            {
                c = c == L'/' ? L'\\' : static_cast<wchar_t>(towlower(c));
            }
            while (!s.empty() && s.back() == L'\\')
            {
                s.pop_back();
            }
            return s;
        }

        // The relative path of `file` inside `root`, "" if it is not inside.
        std::string relPath(const std::filesystem::path& root, const std::filesystem::path& file)
        {
            const std::filesystem::path rel = file.lexically_normal().lexically_relative(root.lexically_normal());
            if (rel.empty() || rel.is_absolute() || *rel.begin() == "..")
            {
                return {};
            }
            return rel.generic_string();
        }

        std::vector<std::unique_ptr<Pack>>& mount(const std::filesystem::path& root)
        {
            auto [it, fresh] = g_mounted.try_emplace(rootKey(root));
            if (fresh)
            {
                std::error_code ec;
                std::vector<std::filesystem::path> files;
                for (std::filesystem::directory_iterator dir(root, ec), end; !ec && dir != end; dir.increment(ec))
                {
                    if (dir->is_regular_file(ec) && dir->path().extension() == L".gpak")
                    {
                        files.push_back(dir->path());
                    }
                }
                std::sort(files.begin(), files.end());
                for (const auto& f : files)
                {
                    std::string error;
                    if (auto pack = Pack::open(f, error))
                    {
                        it->second.push_back(std::move(pack));
                    }
                }
            }
            return it->second;
        }
    }

    bool read(const std::filesystem::path& root, const std::filesystem::path& file, std::string& out)
    {
        const std::string rel = relPath(root, file);
        if (rel.empty())
        {
            return false;
        }
        std::scoped_lock lock(g_lock);
        for (const auto& pack : mount(root))
        {
            if (pack->read(rel, out))
            {
                return true;
            }
        }
        return false;
    }

    bool contains(const std::filesystem::path& root, const std::filesystem::path& file)
    {
        const std::string rel = relPath(root, file);
        if (rel.empty())
        {
            return false;
        }
        std::scoped_lock lock(g_lock);
        for (const auto& pack : mount(root))
        {
            if (pack->contains(rel))
            {
                return true;
            }
        }
        return false;
    }

    void unmount(const std::filesystem::path& root)
    {
        std::scoped_lock lock(g_lock);
        if (root.empty())
        {
            g_mounted.clear();
        }
        else
        {
            g_mounted.erase(rootKey(root));
        }
    }
}
