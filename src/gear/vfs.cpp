#include "vfs.h"

#include <windows.h>

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <vector>

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

        // A path as the index keys it: lower case, forward slashes, no leading "./" or "/".
        std::string key(std::string rel)
        {
            for (char& c : rel)
            {
                c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            size_t start = 0;
            while (start < rel.size() && (rel[start] == '/' || rel.compare(start, 2, "./") == 0))
            {
                start += rel[start] == '/' ? 1 : 2;
            }
            return rel.substr(start);
        }

        // "a/b" -> "a/b/" ("" and "/" -> "")
        std::string mountPrefix(const std::string& mount)
        {
            std::string k = key(mount);
            while (!k.empty() && k.back() == '/')
            {
                k.pop_back();
            }
            return k.empty() ? k : k + "/";
        }

        constexpr size_t kMaxFile = 256u * 1024 * 1024;     // what one file of a pack may unpack to
    }

    Pack::~Pack()
    {
        if (m_file && m_file != INVALID_HANDLE_VALUE)
        {
            CloseHandle(static_cast<HANDLE>(m_file));
        }
    }

    // `n` bytes at `offset` of the pack file: a positioned read on the open handle (nothing of the file stays mapped into the 32-bit address space).
    bool Pack::readAt(uint64_t offset, void* buffer, size_t n) const
    {
        std::scoped_lock lock(m_readLock);      // the position is part of the handle
        auto* out = static_cast<uint8_t*>(buffer);
        while (n > 0)
        {
            OVERLAPPED at{};
            at.Offset = static_cast<DWORD>(offset);
            at.OffsetHigh = static_cast<DWORD>(offset >> 32);
            DWORD got = 0;
            if (!ReadFile(static_cast<HANDLE>(m_file), out, static_cast<DWORD>(std::min<size_t>(n, 1u << 24)), &got, &at) || got == 0)
            {
                return false;
            }
            out += got;
            offset += got;
            n -= got;
        }
        return true;
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
        pack->m_size = static_cast<uint64_t>(size.QuadPart);
        // end of central directory: in the last 22 + comment bytes
        const size_t tailSize = static_cast<size_t>(std::min<uint64_t>(pack->m_size, 22 + 65535));
        std::vector<uint8_t> tail(tailSize);
        if (!pack->readAt(pack->m_size - tailSize, tail.data(), tailSize))
        {
            error = "cannot read";
            return nullptr;
        }
        size_t eocd = std::string::npos;
        for (size_t i = tailSize - 22;; --i)
        {
            if (rd32(tail.data() + i) == 0x06054b50)
            {
                eocd = i;
                break;
            }
            if (i == 0)
            {
                break;
            }
        }
        if (eocd == std::string::npos)
        {
            error = "not a ZIP file";
            return nullptr;
        }
        const uint8_t* e0 = tail.data() + eocd;
        const uint32_t entries = rd16(e0 + 10), dirSize = rd32(e0 + 12), dirOffset32 = rd32(e0 + 16);
        if (entries == 0xFFFF || dirOffset32 == 0xFFFFFFFF || uint64_t(dirOffset32) + dirSize > pack->m_size)
        {
            error = "ZIP64 or damaged";
            return nullptr;
        }
        // the central directory is read once and parsed; the file's data stays on disk
        std::vector<uint8_t> dirBytes(dirSize);
        if (dirSize && !pack->readAt(dirOffset32, dirBytes.data(), dirSize))
        {
            error = "cannot read";
            return nullptr;
        }
        const uint8_t* d = dirBytes.data();
        const size_t dirEnd = dirBytes.size();
        size_t p = 0;
        for (uint32_t i = 0; i < entries; ++i)
        {
            if (p + 46 > dirEnd || rd32(d + p) != 0x02014b50)
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
            if (p + 46 + nameLen > dirEnd)
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
        return it != m_entries.end() && read(it->second, out);
    }

    bool Pack::read(const Entry& e, std::string& out) const
    {
        const uint64_t h = e.headerOffset;
        uint8_t header[30];
        if (h + 30 > m_size || !readAt(h, header, 30) || rd32(header) != 0x04034b50)
        {
            return false;
        }
        const uint64_t start = h + 30 + rd16(header + 26) + rd16(header + 28);
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
            out.resize(e.size);
            return e.size == 0 || readAt(start, out.data(), e.size);
        }
        std::vector<uint8_t> packed(e.compressed);
        if (!readAt(start, packed.data(), packed.size()))
        {
            return false;
        }
        out.resize(e.size);
        z_stream z{};
        if (inflateInit2(&z, -15) != Z_OK)
        {
            return false;
        }
        z.next_in = packed.data();
        z.avail_in = e.compressed;
        z.next_out = reinterpret_cast<Bytef*>(out.data());
        z.avail_out = e.size;
        const int rc = inflate(&z, Z_FINISH);
        inflateEnd(&z);
        return rc == Z_STREAM_END && z.total_out == e.size;
    }

    namespace
    {
        // The merged view of a mod's layers: virtual path -> the pack and entry that win.
        struct Hit
        {
            const Pack* pack = nullptr;
            Pack::Entry entry;
        };
        struct Mounted
        {
            std::vector<std::unique_ptr<Pack>> packs;
            std::map<std::string, Hit> index;
        };

        std::mutex g_lock;
        std::map<std::wstring, std::vector<Layer>> g_layers;      // lower-cased root -> the layers mod.lua declared
        std::map<std::wstring, Mounted> g_mounted;

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
            return key(rel.generic_string());
        }

        Mounted& mount(const std::filesystem::path& root)
        {
            const std::wstring k = rootKey(root);
            auto [it, fresh] = g_mounted.try_emplace(k);
            if (!fresh)
            {
                return it->second;
            }
            std::vector<Layer> layers;
            if (const auto declared = g_layers.find(k); declared != g_layers.end())
            {
                layers = declared->second;
            }
            else
            {
                // no `vfs` in mod.lua: the folder's *.gpak files, in name order, at the root
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
                    layers.push_back({f, "/"});
                }
            }
            Mounted& m = it->second;
            for (const Layer& layer : layers)
            {
                std::string error;
                std::unique_ptr<Pack> pack = Pack::open(layer.file, error);
                if (!pack)
                {
                    continue;
                }
                const std::string prefix = mountPrefix(layer.mount);
                for (const auto& [name, entry] : pack->entries())
                {
                    m.index[prefix + name] = Hit{pack.get(), entry};       // a later layer replaces an earlier one
                }
                m.packs.push_back(std::move(pack));
            }
            return m;
        }
    }

    void configure(const std::filesystem::path& root, std::vector<Layer> layers)
    {
        std::scoped_lock lock(g_lock);
        const std::wstring k = rootKey(root);
        g_mounted.erase(k);
        g_layers[k] = std::move(layers);
    }

    void unmount(const std::filesystem::path& root)
    {
        std::scoped_lock lock(g_lock);
        if (root.empty())
        {
            g_mounted.clear();
            g_layers.clear();
        }
        else
        {
            const std::wstring k = rootKey(root);
            g_mounted.erase(k);
            g_layers.erase(k);
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
        const Mounted& m = mount(root);
        const auto it = m.index.find(rel);
        return it != m.index.end() && it->second.pack->read(it->second.entry, out);
    }

    bool contains(const std::filesystem::path& root, const std::filesystem::path& file)
    {
        const std::string rel = relPath(root, file);
        if (rel.empty())
        {
            return false;
        }
        std::scoped_lock lock(g_lock);
        return mount(root).index.count(rel) != 0;
    }

    std::vector<std::string> list(const std::filesystem::path& root, const std::string& dir)
    {
        std::string prefix = key(dir);
        while (!prefix.empty() && prefix.back() == '/')
        {
            prefix.pop_back();
        }
        if (!prefix.empty())
        {
            prefix += '/';
        }
        std::vector<std::string> out;
        std::scoped_lock lock(g_lock);
        const Mounted& m = mount(root);
        for (auto it = m.index.lower_bound(prefix); it != m.index.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it)
        {
            out.push_back(it->first);
        }
        return out;
    }
}
