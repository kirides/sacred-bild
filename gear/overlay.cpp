#include "overlay.h"
#include "log.h"
#include "patch.h"

#include <windows.h>
#include <lua.hpp>
#include <map>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    struct ModRules
    {
        std::string id;
        std::wstring dir;                                       // the mod's folder
        std::unordered_map<std::string, std::string> files;     // game path (lower case, relative) -> absolute path
        std::unordered_map<std::string, std::string> patches;   // game path -> the patch file (absolute) of a `patch:` rule
        bool saves = false;
        std::string savesRoot;                                  // absolute, no trailing backslash
    };

    std::vector<ModRules> g_rules;
    std::mutex g_servedLock;
    std::unordered_map<std::string, unsigned> g_served;     // game path (lower case) -> opens served from a mod
    std::string g_gameDir;                                      // lower case, no trailing backslash
    bool g_installed = false;

    std::string narrow(const std::wstring& s)
    {
        if (s.empty())
        {
            return {};
        }
        const int n = WideCharToMultiByte(CP_ACP, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_ACP, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
        return out;
    }

    std::wstring widen(const std::string& s)
    {
        if (s.empty())
        {
            return {};
        }
        const int n = MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring out(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
        return out;
    }

    std::string lower(std::string s)
    {
        for (char& c : s)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return s;
    }

    std::string backslashes(std::string s)
    {
        std::replace(s.begin(), s.end(), '/', '\\');
        return s;
    }

    // The game-relative lower case form of a path the game opens ("" if it is not under the game folder).
    std::string relative(const char* path)
    {
        std::string p = lower(backslashes(path));
        while (p.rfind(".\\", 0) == 0)
        {
            p.erase(0, 2);
        }
        if (p.size() >= 2 && p[1] == ':')
        {
            if (p.rfind(g_gameDir, 0) == 0 && (p.size() == g_gameDir.size() || p[g_gameDir.size()] == '\\'))
            {
                p.erase(0, g_gameDir.size());
                while (!p.empty() && p[0] == '\\')
                {
                    p.erase(0, 1);
                }
                return p;
            }
            return {};
        }
        if (!p.empty() && p[0] == '\\')
        {
            return {};
        }
        return p;
    }

    bool exists(const std::string& path)
    {
        return GetFileAttributesW(widen(path).c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    void ensureParents(const std::string& path)
    {
        std::wstring w = widen(path);
        for (size_t i = 3; i < w.size(); ++i)
        {
            if (w[i] == L'\\')
            {
                CreateDirectoryW(w.substr(0, i).c_str(), nullptr);
            }
        }
    }

    // --- delta patches: `game\path = patch:file.lua` ------------------------------------------------------------------------
    // The mod ships only what differs from the game's own file: a Lua data file `return {size = n, crc32 = c, ops = {{offset, 'hex'[, replaced]}, ...}}`
    // (offsets in the original, ascending; `hex` replaces `replaced` bytes, default as many as it has). The first open of the file builds the patched
    // copy in the mod's `cache\` folder (rebuilt when the patch file or the original changed) and serves that; an original of another size or crc32
    // than the patch was made for is served unpatched (logged once).

    std::string g_gameDirReal;                          // as given, no trailing backslash

    uint32_t crc32Of(const unsigned char* data, size_t n)
    {
        static uint32_t table[256];
        static bool ready = false;
        if (!ready)
        {
            for (uint32_t i = 0; i < 256; ++i)
            {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k)
                {
                    c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                }
                table[i] = c;
            }
            ready = true;
        }
        uint32_t c = 0xFFFFFFFFu;
        for (size_t i = 0; i < n; ++i)
        {
            c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
        }
        return c ^ 0xFFFFFFFFu;
    }

    bool readAll(const std::string& path, std::vector<unsigned char>& out)
    {
        const HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        LARGE_INTEGER size{};
        GetFileSizeEx(h, &size);
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        const bool ok = out.empty() || (ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size());
        CloseHandle(h);
        return ok;
    }

    bool writeAll(const std::string& path, const std::vector<unsigned char>& data)
    {
        ensureParents(path);
        const std::string temp = path + ".tmp";
        const HANDLE h = CreateFileW(widen(temp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        DWORD put = 0;
        const bool ok = data.empty() || (WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &put, nullptr) && put == data.size());
        CloseHandle(h);
        if (!ok)
        {
            DeleteFileW(widen(temp).c_str());
            return false;
        }
        return MoveFileExW(widen(temp).c_str(), widen(path).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    }

    int hexValue(char c)
    {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    }

    // A text table (scripts\<lang>\global.res): u32 count; count entries {u32 id, u32 offset (from the file start + 4), u32 flags, u32 length}
    // ascending by id; then the texts, one after the other, UTF-16LE. The patch's `set` gives texts by id (UTF-8, or {text|hex, flags}), `remove` ids.
    bool applyTextPatch(lua_State* L, int patch, const std::vector<unsigned char>& original, std::vector<unsigned char>& out, std::string& why)
    {
        struct Entry
        {
            uint32_t flags = 0;
            std::vector<unsigned char> text;
        };
        std::map<uint32_t, Entry> entries;
        auto u32 = [&](size_t at) {
            uint32_t v = 0;
            std::memcpy(&v, original.data() + at, 4);
            return v;
        };
        if (original.size() < 4)
        {
            why = "the text table is too small";
            return false;
        }
        const uint32_t count = u32(0);
        if (4 + static_cast<uint64_t>(count) * 16 > original.size())
        {
            why = "the text table is damaged";
            return false;
        }
        for (uint32_t i = 0; i < count; ++i)
        {
            const size_t e = 4 + static_cast<size_t>(i) * 16;
            const uint64_t from = static_cast<uint64_t>(u32(e + 4)) + 4, length = u32(e + 12);
            if (from + length > original.size())
            {
                why = "the text table is damaged";
                return false;
            }
            Entry& entry = entries[u32(e)];
            entry.flags = u32(e + 8);
            entry.text.assign(original.begin() + static_cast<size_t>(from), original.begin() + static_cast<size_t>(from + length));
        }
        lua_getfield(L, patch, "remove");
        if (lua_istable(L, -1))
        {
            const int n = static_cast<int>(lua_objlen(L, -1));
            for (int i = 1; i <= n; ++i)
            {
                lua_rawgeti(L, -1, i);
                if (lua_isnumber(L, -1))
                {
                    entries.erase(static_cast<uint32_t>(lua_tonumber(L, -1)));
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        lua_getfield(L, patch, "set");
        if (lua_istable(L, -1))
        {
            lua_pushnil(L);
            while (lua_next(L, -2) != 0)
            {
                if (!lua_isnumber(L, -2))
                {
                    why = "a text id is not a number";
                    return false;
                }
                const uint32_t id = static_cast<uint32_t>(lua_tonumber(L, -2));
                Entry& entry = entries[id];
                std::string utf8;
                bool haveText = false;
                std::vector<unsigned char> raw;
                if (lua_isstring(L, -1) && lua_type(L, -1) == LUA_TSTRING)
                {
                    size_t n = 0;
                    const char* s = lua_tolstring(L, -1, &n);
                    utf8.assign(s, n);
                    haveText = true;
                }
                else if (lua_istable(L, -1))
                {
                    lua_getfield(L, -1, "flags");
                    if (lua_isnumber(L, -1))
                    {
                        entry.flags = static_cast<uint32_t>(lua_tonumber(L, -1));
                    }
                    lua_pop(L, 1);
                    lua_getfield(L, -1, "text");
                    if (lua_type(L, -1) == LUA_TSTRING)
                    {
                        size_t n = 0;
                        const char* s = lua_tolstring(L, -1, &n);
                        utf8.assign(s, n);
                        haveText = true;
                    }
                    lua_pop(L, 1);
                    if (!haveText)
                    {
                        lua_getfield(L, -1, "hex");
                        if (lua_type(L, -1) == LUA_TSTRING)
                        {
                            size_t n = 0;
                            const char* s = lua_tolstring(L, -1, &n);
                            for (size_t k = 0; k + 1 < n; k += 2)
                            {
                                const int hi = hexValue(s[k]), lo = hexValue(s[k + 1]);
                                if (hi < 0 || lo < 0)
                                {
                                    why = "bad hex in a text";
                                    return false;
                                }
                                raw.push_back(static_cast<unsigned char>(hi * 16 + lo));
                            }
                            entry.text = raw;
                        }
                        else
                        {
                            why = "a text has neither text nor hex";
                            return false;
                        }
                        lua_pop(L, 1);
                    }
                }
                if (haveText)
                {
                    const int wide = utf8.empty() ? 0 : MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
                    std::wstring w(static_cast<size_t>(wide), L'\0');
                    if (wide)
                    {
                        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), wide);
                    }
                    entry.text.assign(reinterpret_cast<const unsigned char*>(w.data()), reinterpret_cast<const unsigned char*>(w.data()) + w.size() * 2);
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        out.clear();
        const uint32_t total = static_cast<uint32_t>(entries.size());
        auto put = [&](uint32_t v) {
            const unsigned char* p = reinterpret_cast<const unsigned char*>(&v);
            out.insert(out.end(), p, p + 4);
        };
        put(total);
        uint32_t offset = total * 16;
        for (const auto& [id, entry] : entries)
        {
            put(id);
            put(offset);
            put(entry.flags);
            put(static_cast<uint32_t>(entry.text.size()));
            offset += static_cast<uint32_t>(entry.text.size());
        }
        for (const auto& [id, entry] : entries)
        {
            out.insert(out.end(), entry.text.begin(), entry.text.end());
        }
        return true;
    }

    // Applies the patch file to `original` in memory; false (and a message) if it does not fit.
    bool applyPatch(const std::string& patchFile, const std::vector<unsigned char>& original, std::vector<unsigned char>& out, std::string& why)
    {
        lua_State* L = luaL_newstate();      // no libraries: the file is data
        if (!L)
        {
            why = "no Lua state";
            return false;
        }
        struct Closer
        {
            lua_State* L;
            ~Closer() { lua_close(L); }
        } closer{L};
        if (luaL_loadfile(L, patchFile.c_str()) != 0 || lua_pcall(L, 0, 1, 0) != 0 || !lua_istable(L, -1))
        {
            why = std::string("cannot read the patch: ") + (lua_isstring(L, -1) ? lua_tostring(L, -1) : "no table");
            return false;
        }
        lua_getfield(L, -1, "size");
        const double size = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : -1;
        lua_pop(L, 1);
        lua_getfield(L, -1, "crc32");
        const double crc = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : -1;
        lua_pop(L, 1);
        if (size != static_cast<double>(original.size()) || (crc >= 0 && static_cast<uint32_t>(crc) != crc32Of(original.data(), original.size())))
        {
            why = "the game's file is not the one the patch was made for (size or crc32 differs)";
            return false;
        }
        lua_getfield(L, -1, "kind");
        const bool texts = lua_isstring(L, -1) && std::strcmp(lua_tostring(L, -1), "texts") == 0;
        lua_pop(L, 1);
        if (texts)
        {
            return applyTextPatch(L, lua_gettop(L), original, out, why);
        }
        lua_getfield(L, -1, "ops");
        if (!lua_istable(L, -1))
        {
            why = "the patch has no ops";
            return false;
        }
        out.clear();
        out.reserve(original.size() + 4096);
        size_t at = 0;
        const int count = static_cast<int>(lua_objlen(L, -1));
        for (int i = 1; i <= count; ++i)
        {
            lua_rawgeti(L, -1, i);
            lua_rawgeti(L, -1, 1);
            lua_rawgeti(L, -1 - 1, 2);
            lua_rawgeti(L, -3, 3);
            const double off = lua_isnumber(L, -3) ? lua_tonumber(L, -3) : -1;
            size_t hexLen = 0;
            const char* hex = lua_isstring(L, -2) ? lua_tolstring(L, -2, &hexLen) : nullptr;
            const size_t replaced = lua_isnumber(L, -1) ? static_cast<size_t>(lua_tonumber(L, -1)) : hexLen / 2;
            if (off < 0 || !hex || (hexLen & 1) || static_cast<size_t>(off) < at || static_cast<size_t>(off) + replaced > original.size())
            {
                why = "bad op " + std::to_string(i);
                return false;
            }
            out.insert(out.end(), original.begin() + at, original.begin() + static_cast<size_t>(off));
            for (size_t k = 0; k + 1 < hexLen + 1 && k < hexLen; k += 2)
            {
                const int hi = hexValue(hex[k]), lo = hexValue(hex[k + 1]);
                if (hi < 0 || lo < 0)
                {
                    why = "bad hex in op " + std::to_string(i);
                    return false;
                }
                out.push_back(static_cast<unsigned char>(hi * 16 + lo));
            }
            at = static_cast<size_t>(off) + replaced;
            lua_pop(L, 4);
        }
        out.insert(out.end(), original.begin() + at, original.end());
        return true;
    }

    std::mutex g_patchLock;
    std::unordered_map<std::string, bool> g_patchDone;      // cache path -> built (or fell back)

    // Makes sure the patched copy of `rel` exists (building it); false if the original should be served instead.
    bool ensurePatched(const ModRules& r, const std::string& rel, const std::string& patchFile, const std::string& cache)
    {
        std::lock_guard guard(g_patchLock);
        const auto done = g_patchDone.find(cache);
        if (done != g_patchDone.end())
        {
            return done->second;
        }
        bool ok = false;
        std::vector<unsigned char> original, patchText;
        const std::string originalPath = g_gameDirReal + "\\" + rel;
        if (!readAll(originalPath, original) || !readAll(patchFile, patchText))
        {
            LOG("Overlay: {}: patch {}: cannot read the original or the patch", r.id, rel);
        }
        else
        {
            // the cache stays valid while the patch file and the original are the same
            char key[64];
            std::snprintf(key, sizeof(key), "%08x-%08x-%zu", crc32Of(patchText.data(), patchText.size()), crc32Of(original.data(), original.size()), original.size());
            std::vector<unsigned char> stored;
            const std::string keyFile = cache + ".key";
            if (exists(cache) && readAll(keyFile, stored) && std::string(stored.begin(), stored.end()) == key)
            {
                ok = true;
            }
            else
            {
                std::vector<unsigned char> patched;
                std::string why;
                if (!applyPatch(patchFile, original, patched, why))
                {
                    LOG("Overlay: {}: patch {} not applied: {}", r.id, rel, why);
                }
                else if (!writeAll(cache, patched))
                {
                    LOG("Overlay: {}: patch {}: cannot write {}", r.id, rel, cache);
                }
                else
                {
                    writeAll(keyFile, std::vector<unsigned char>(key, key + std::strlen(key)));
                    LOG("Overlay: {}: patched {} ({} -> {} bytes)", r.id, rel, original.size(), patched.size());
                    ok = true;
                }
            }
        }
        g_patchDone[cache] = ok;
        return ok;
    }

    // The mod path a game path maps to; false if no rule applies. `write`: the call may create the file.
    bool map(const char* path, std::string& out, bool write, bool count = true)
    {
        if (g_rules.empty() || !path || !*path)
        {
            return false;
        }
        const std::string rel = relative(path);
        if (rel.empty())
        {
            return false;
        }
        for (const ModRules& r : g_rules)
        {
            const auto it = r.files.find(rel);
            if (it != r.files.end() && !write)
            {
                const auto patch = r.patches.find(rel);
                if (patch != r.patches.end() && !ensurePatched(r, rel, patch->second, it->second))
                {
                    continue;       // the original is served
                }
            }
            if (it != r.files.end() && (write || exists(it->second)))
            {
                out = it->second;
                if (count)
                {
                    std::lock_guard guard(g_servedLock);
                    ++g_served[rel];
                }
                return true;
            }
            if (r.saves && (rel == "save" || rel.rfind("save\\", 0) == 0))
            {
                out = r.savesRoot + (rel.size() > 4 ? rel.substr(4) : std::string{});
                // keep the case the game used for the part below the folder
                if (rel.size() > 5)
                {
                    std::string orig = backslashes(path);
                    const size_t at = lower(orig).find("save\\");
                    if (at != std::string::npos)
                    {
                        out = r.savesRoot + "\\" + orig.substr(at + 5);
                    }
                }
                if (write)
                {
                    ensureParents(out);
                }
                if (count)
                {
                    std::lock_guard guard(g_servedLock);
                    ++g_served[rel];
                }
                return true;
            }
        }
        return false;
    }

    using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    using GetFileAttributesAFn = DWORD(WINAPI*)(LPCSTR);
    using GetFileAttributesExAFn = BOOL(WINAPI*)(LPCSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
    using FindFirstFileAFn = HANDLE(WINAPI*)(LPCSTR, LPWIN32_FIND_DATAA);
    using CopyFileAFn = BOOL(WINAPI*)(LPCSTR, LPCSTR, BOOL);
    using DeleteFileAFn = BOOL(WINAPI*)(LPCSTR);
    using CreateDirectoryAFn = BOOL(WINAPI*)(LPCSTR, LPSECURITY_ATTRIBUTES);
    using SetFileAttributesAFn = BOOL(WINAPI*)(LPCSTR, DWORD);

    CreateFileAFn g_createFile = nullptr;
    GetFileAttributesAFn g_getAttributes = nullptr;
    GetFileAttributesExAFn g_getAttributesEx = nullptr;
    FindFirstFileAFn g_findFirst = nullptr;
    CopyFileAFn g_copyFile = nullptr;
    DeleteFileAFn g_deleteFile = nullptr;
    CreateDirectoryAFn g_createDirectory = nullptr;
    SetFileAttributesAFn g_setAttributes = nullptr;

    HANDLE WINAPI hookCreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                                  DWORD flags, HANDLE templ)
    {
        std::string mapped;
        const bool write = (access & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0 ||
                           (disposition != OPEN_EXISTING && disposition != TRUNCATE_EXISTING);
        if (map(name, mapped, write))
        {
            return g_createFile(mapped.c_str(), access, share, sa, disposition, flags, templ);
        }
        return g_createFile(name, access, share, sa, disposition, flags, templ);
    }

    DWORD WINAPI hookGetFileAttributesA(LPCSTR name)
    {
        std::string mapped;
        return map(name, mapped, false) ? g_getAttributes(mapped.c_str()) : g_getAttributes(name);
    }

    BOOL WINAPI hookGetFileAttributesExA(LPCSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID info)
    {
        std::string mapped;
        return map(name, mapped, false) ? g_getAttributesEx(mapped.c_str(), level, info) : g_getAttributesEx(name, level, info);
    }

    HANDLE WINAPI hookFindFirstFileA(LPCSTR name, LPWIN32_FIND_DATAA data)
    {
        std::string mapped;
        return map(name, mapped, false) ? g_findFirst(mapped.c_str(), data) : g_findFirst(name, data);
    }

    BOOL WINAPI hookCopyFileA(LPCSTR from, LPCSTR to, BOOL failIfExists)
    {
        std::string a, b;
        const bool ma = map(from, a, false), mb = map(to, b, true);
        return g_copyFile(ma ? a.c_str() : from, mb ? b.c_str() : to, failIfExists);
    }

    BOOL WINAPI hookDeleteFileA(LPCSTR name)
    {
        std::string mapped;
        return map(name, mapped, false) ? g_deleteFile(mapped.c_str()) : g_deleteFile(name);
    }

    BOOL WINAPI hookCreateDirectoryA(LPCSTR name, LPSECURITY_ATTRIBUTES sa)
    {
        std::string mapped;
        return map(name, mapped, true) ? g_createDirectory(mapped.c_str(), sa) : g_createDirectory(name, sa);
    }

    BOOL WINAPI hookSetFileAttributesA(LPCSTR name, DWORD attributes)
    {
        std::string mapped;
        return map(name, mapped, false) ? g_setAttributes(mapped.c_str(), attributes) : g_setAttributes(name, attributes);
    }

    std::string trim(const std::string& s)
    {
        const size_t b = s.find_first_not_of(" \t\r");
        return b == std::string::npos ? std::string{} : s.substr(b, s.find_last_not_of(" \t\r") - b + 1);
    }

    // The mods the run uses: `-gear-mods=a,b` on the command line, else all.
    std::vector<std::string> selectedMods()
    {
        std::string cmd = GetCommandLineA();
        const size_t at = cmd.find("-gear-mods=");
        std::vector<std::string> out;
        if (at == std::string::npos)
        {
            return out;
        }
        std::string list = cmd.substr(at + 11);
        list = list.substr(0, list.find_first_of(" \t"));
        std::stringstream ss(list);
        std::string item;
        while (std::getline(ss, item, ','))
        {
            if (!item.empty())
            {
                out.push_back(item);
            }
        }
        return out;
    }

    // Reads mods\<id>\overlay.txt.
    bool readRules(const std::wstring& modDir, const std::string& id, ModRules& rules)
    {
        std::ifstream file(modDir + L"\\overlay.txt");
        if (!file)
        {
            return false;
        }
        rules.id = id;
        rules.dir = modDir;
        const std::string dir = narrow(modDir);
        std::string line;
        int lineNo = 0;
        while (std::getline(file, line))
        {
            ++lineNo;
            const size_t hash = line.find('#');
            if (hash != std::string::npos)
            {
                line.erase(hash);
            }
            line = trim(line);
            if (line.empty())
            {
                continue;
            }
            const size_t eq = line.find('=');
            if (eq == std::string::npos)
            {
                LOG("Overlay: {}\\overlay.txt:{}: no '=': {}", id, lineNo, line);
                continue;
            }
            const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
            if (value.find("..") != std::string::npos || (value.size() > 1 && value[1] == ':') || value.rfind('\\', 0) == 0)
            {
                LOG("Overlay: {}\\overlay.txt:{}: the target must be inside the mod's folder: {}", id, lineNo, value);
                continue;
            }
            if (lower(key) == "saves")
            {
                rules.saves = true;
                rules.savesRoot = dir + "\\" + backslashes(value);
                while (!rules.savesRoot.empty() && rules.savesRoot.back() == '\\')
                {
                    rules.savesRoot.pop_back();
                }
                ensureParents(rules.savesRoot + "\\x");
                CreateDirectoryW(widen(rules.savesRoot).c_str(), nullptr);
            }
            else
            {
                std::string game = lower(backslashes(key));
                while (game.rfind(".\\", 0) == 0)
                {
                    game.erase(0, 2);
                }
                if (lower(value).rfind("patch:", 0) == 0)
                {
                    // a delta patch: the patch file is in the mod, the patched copy is made in cache\<game path>
                    const std::string patchFile = trim(value.substr(6));
                    rules.patches[game] = dir + "\\" + backslashes(patchFile);
                    rules.files[game] = dir + "\\cache\\" + game;
                }
                else
                {
                    rules.files[game] = dir + "\\" + backslashes(value);
                }
            }
        }
        return !rules.files.empty() || rules.saves;
    }
}

bool Overlay::install(const std::wstring& gameDir)
{
    if (g_installed)
    {
        return true;
    }
    g_gameDirReal = narrow(gameDir);
    while (!g_gameDirReal.empty() && g_gameDirReal.back() == '\\')
    {
        g_gameDirReal.pop_back();
    }
    g_gameDir = lower(narrow(gameDir));
    while (!g_gameDir.empty() && g_gameDir.back() == '\\')
    {
        g_gameDir.pop_back();
    }
    const std::vector<std::string> selected = selectedMods();
    WIN32_FIND_DATAW fd;
    const HANDLE h = FindFirstFileW((gameDir + L"\\mods\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            const std::wstring name = fd.cFileName;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || name == L"." || name == L"..")
            {
                continue;
            }
            const std::string id = narrow(name);
            if (!selected.empty() && std::find(selected.begin(), selected.end(), id) == selected.end())
            {
                continue;
            }
            ModRules rules;
            if (readRules(gameDir + L"\\mods\\" + name, id, rules))
            {
                LOG("Overlay: mod {}: {} file rule(s){}", id, rules.files.size(),
                    rules.saves ? ", saves in " + rules.savesRoot : std::string{});
                g_rules.push_back(std::move(rules));
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (g_rules.empty())
    {
        return false;
    }
    // The exe's imports, and SacredBild's (ddraw.dll reads the language files itself): the same kernel32 functions.
    auto hook = [](const char* name, void* replacement) {
        void* previous = Patch::iat("KERNEL32.dll", name, replacement);
        // SacredBild reads the language files with these two
        if (std::strcmp(name, "CreateFileA") == 0 || std::strcmp(name, "GetFileAttributesA") == 0)
        {
            Patch::iat("ddraw.dll", "KERNEL32.dll", name, replacement);
        }
        return previous;
    };
    g_createFile = reinterpret_cast<CreateFileAFn>(hook("CreateFileA", reinterpret_cast<void*>(&hookCreateFileA)));
    g_getAttributes = reinterpret_cast<GetFileAttributesAFn>(hook("GetFileAttributesA", reinterpret_cast<void*>(&hookGetFileAttributesA)));
    g_getAttributesEx = reinterpret_cast<GetFileAttributesExAFn>(hook("GetFileAttributesExA", reinterpret_cast<void*>(&hookGetFileAttributesExA)));
    g_findFirst = reinterpret_cast<FindFirstFileAFn>(hook("FindFirstFileA", reinterpret_cast<void*>(&hookFindFirstFileA)));
    g_copyFile = reinterpret_cast<CopyFileAFn>(hook("CopyFileA", reinterpret_cast<void*>(&hookCopyFileA)));
    g_deleteFile = reinterpret_cast<DeleteFileAFn>(hook("DeleteFileA", reinterpret_cast<void*>(&hookDeleteFileA)));
    g_createDirectory = reinterpret_cast<CreateDirectoryAFn>(hook("CreateDirectoryA", reinterpret_cast<void*>(&hookCreateDirectoryA)));
    g_setAttributes = reinterpret_cast<SetFileAttributesAFn>(hook("SetFileAttributesA", reinterpret_cast<void*>(&hookSetFileAttributesA)));
    g_installed = g_createFile && g_getAttributes && g_findFirst;
    if (!g_installed)
    {
        LOG("Overlay: the game's file functions are not imported as expected: overlay off");
        g_rules.clear();
        return false;
    }
    LOG("Overlay: the game's file functions are hooked ({} mod(s))", g_rules.size());
    return true;
}

unsigned Overlay::served(const std::string& path)
{
    std::string rel = lower(backslashes(path));
    while (rel.rfind(".\\", 0) == 0)
    {
        rel.erase(0, 2);
    }
    std::lock_guard guard(g_servedLock);
    const auto it = g_served.find(rel);
    return it == g_served.end() ? 0 : it->second;
}

std::string Overlay::resolve(const std::string& path)
{
    std::string mapped;
    return map(path.c_str(), mapped, false, false) ? mapped : path;
}

bool Overlay::info(const std::string& id, size_t& files, std::string& saves)
{
    for (const ModRules& r : g_rules)
    {
        if (r.id == id)
        {
            files = r.files.size();
            saves = r.saves ? r.savesRoot : std::string{};
            return true;
        }
    }
    return false;
}
