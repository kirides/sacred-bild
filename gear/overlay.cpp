#include "overlay.h"
#include "log.h"
#include "patch.h"
#include "instance.h"

#include <windows.h>
#include <lua.hpp>
#include <map>
#include <detours/detours.h>
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
    bool g_server = false;      // inside gameserver.exe: the client's mods' file rules only (no saves, no CreateProcess hook)
    constexpr const char* kHandoff = "SACREDGEAR_OVERLAY_MODS";

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
    // ascending by id; then the texts, one after the other, UTF-16LE. An id can have several entries (in file order). The patch's `set` gives
    // texts by id (UTF-8, or {text|hex, flags}, or {list = {entry, ...}} for several), `remove` ids.
    struct TextEntry
    {
        uint32_t flags = 0;
        std::vector<unsigned char> text;
    };

    // One entry from the Lua value on top of the stack (a string, or {text|hex, flags}); `current` supplies the default flags.
    bool readTextEntry(lua_State* L, const TextEntry& current, TextEntry& entry, std::string& why)
    {
        entry = current;
        std::string utf8;
        bool haveText = false;
        if (lua_type(L, -1) == LUA_TSTRING)
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
                if (lua_type(L, -1) != LUA_TSTRING)
                {
                    lua_pop(L, 1);
                    why = "a text has neither text nor hex";
                    return false;
                }
                size_t n = 0;
                const char* s = lua_tolstring(L, -1, &n);
                entry.text.clear();
                for (size_t k = 0; k + 1 < n; k += 2)
                {
                    const int hi = hexValue(s[k]), lo = hexValue(s[k + 1]);
                    if (hi < 0 || lo < 0)
                    {
                        lua_pop(L, 1);
                        why = "bad hex in a text";
                        return false;
                    }
                    entry.text.push_back(static_cast<unsigned char>(hi * 16 + lo));
                }
                lua_pop(L, 1);
            }
        }
        else
        {
            why = "a text is neither a string nor a table";
            return false;
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
        return true;
    }

    bool applyTextPatch(lua_State* L, int patch, const std::vector<unsigned char>& original, std::vector<unsigned char>& out, std::string& why)
    {
        std::map<uint32_t, std::vector<TextEntry>> entries;
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
            TextEntry entry;
            entry.flags = u32(e + 8);
            entry.text.assign(original.begin() + static_cast<size_t>(from), original.begin() + static_cast<size_t>(from + length));
            entries[u32(e)].push_back(std::move(entry));
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
                const TextEntry current = entries.count(id) && !entries[id].empty() ? entries[id].front() : TextEntry{};
                std::vector<TextEntry> list;
                bool multiple = false;
                if (lua_istable(L, -1))
                {
                    lua_getfield(L, -1, "list");
                    multiple = lua_istable(L, -1);
                    if (multiple)
                    {
                        const int n = static_cast<int>(lua_objlen(L, -1));
                        for (int k = 1; k <= n; ++k)
                        {
                            lua_rawgeti(L, -1, k);
                            TextEntry entry;
                            if (!readTextEntry(L, TextEntry{}, entry, why))
                            {
                                return false;
                            }
                            list.push_back(std::move(entry));
                            lua_pop(L, 1);
                        }
                    }
                    lua_pop(L, 1);
                }
                if (!multiple)
                {
                    TextEntry entry;
                    if (!readTextEntry(L, current, entry, why))
                    {
                        return false;
                    }
                    list.push_back(std::move(entry));
                }
                entries[id] = std::move(list);
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        out.clear();
        uint32_t total = 0;
        for (const auto& [id, list] : entries)
        {
            total += static_cast<uint32_t>(list.size());
        }
        auto put = [&](uint32_t v) {
            const unsigned char* p = reinterpret_cast<const unsigned char*>(&v);
            out.insert(out.end(), p, p + 4);
        };
        put(total);
        uint32_t offset = total * 16;
        for (const auto& [id, list] : entries)
        {
            for (const TextEntry& entry : list)
            {
                put(id);
                put(offset);
                put(entry.flags);
                put(static_cast<uint32_t>(entry.text.size()));
                offset += static_cast<uint32_t>(entry.text.size());
            }
        }
        for (const auto& [id, list] : entries)
        {
            for (const TextEntry& entry : list)
            {
                out.insert(out.end(), entry.text.begin(), entry.text.end());
            }
        }
        return true;
    }

    // A word table of records with named fields: `records = {first = word index of record 0, words = words per record, fields = {name = word in the
    // record, ...}}` and `edits = {{record = n | records = {from, to}, <field> = value, ...}, ...}` (a value is an unsigned integer or {f = float};
    // names from constants/*.lua are numbers by then). Every field of an edit is written in the record(s) it names.
    bool wordValue(lua_State* L, int index, uint32_t& word)
    {
        if (lua_isnumber(L, index))
        {
            word = static_cast<uint32_t>(static_cast<int64_t>(lua_tonumber(L, index)));
            return true;
        }
        if (lua_istable(L, index))
        {
            lua_getfield(L, index, "f");
            const bool ok = lua_isnumber(L, -1);
            const float f = static_cast<float>(ok ? lua_tonumber(L, -1) : 0.0);
            lua_pop(L, 1);
            std::memcpy(&word, &f, 4);
            return ok;
        }
        return false;
    }

    bool hasWords(lua_State* L, int patch)         // a patch made of `sections` only
    {
        lua_getfield(L, patch, "sections");
        const bool has = lua_istable(L, -1);
        lua_pop(L, 1);
        return has;
    }

    bool applyWordRecords(lua_State* L, int patch, std::vector<unsigned char>& out, std::string& why)       // `patch`: the patch or one of its `sections`
    {
        lua_getfield(L, patch, "records");
        if (!lua_istable(L, -1))
        {
            why = "a patch with edits needs a records layout";
            return false;
        }
        const int layout = lua_gettop(L);
        auto layoutNumber = [&](const char* field) {
            lua_getfield(L, layout, field);
            const double v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : -1;
            lua_pop(L, 1);
            return v;
        };
        const double first = layoutNumber("first"), words = layoutNumber("words");
        lua_getfield(L, layout, "fields");
        if (first < 0 || words < 1 || !lua_istable(L, -1))
        {
            why = "bad records layout";
            return false;
        }
        const int fields = lua_gettop(L);
        lua_getfield(L, patch, "edits");
        const int count = static_cast<int>(lua_objlen(L, -1));
        for (int i = 1; i <= count; ++i)
        {
            lua_rawgeti(L, -1, i);
            const int edit = lua_gettop(L);
            double from = -1, to = -1;
            lua_getfield(L, edit, "record");
            if (lua_isnumber(L, -1))
            {
                from = to = lua_tonumber(L, -1);
            }
            lua_pop(L, 1);
            lua_getfield(L, edit, "records");
            if (lua_istable(L, -1))
            {
                lua_rawgeti(L, -1, 1);
                lua_rawgeti(L, -2, 2);
                from = lua_isnumber(L, -2) ? lua_tonumber(L, -2) : -1;
                to = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : -1;
                lua_pop(L, 2);
            }
            lua_pop(L, 1);
            if (from < 0 || to < from)
            {
                why = "edit " + std::to_string(i) + ": no record";
                return false;
            }
            lua_pushnil(L);
            while (lua_next(L, edit) != 0)
            {
                const char* key = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : "";
                if (std::strcmp(key, "record") != 0 && std::strcmp(key, "records") != 0)
                {
                    lua_getfield(L, fields, key);
                    const double field = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : -1;
                    lua_pop(L, 1);
                    uint32_t word = 0;
                    if (field < 0 || !wordValue(L, lua_gettop(L), word))
                    {
                        why = "edit " + std::to_string(i) + ": the field " + key + " is not in the layout or its value is not a number";
                        return false;
                    }
                    for (double r = from; r <= to; r += 1)
                    {
                        const size_t at = static_cast<size_t>(first + r * words + field) * 4;
                        if (at + 4 > out.size())
                        {
                            why = "edit " + std::to_string(i) + " is past the end of the file";
                            return false;
                        }
                        std::memcpy(out.data() + at, &word, 4);
                    }
                }
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 3);
        return true;
    }

    // A table of 32 bit words (balance.bin, sets.bin ...): `ops = {{first word index, {value, ...}}, ...}`, a value is an unsigned integer or {f = float}.
    bool applyWordPatch(lua_State* L, int patch, const std::vector<unsigned char>& original, std::vector<unsigned char>& out, std::string& why)
    {
        if (original.size() % 4)
        {
            why = "the file is not a table of 32 bit words";
            return false;
        }
        out = original;
        lua_getfield(L, patch, "edits");
        const bool records = lua_istable(L, -1);
        lua_pop(L, 1);
        if (records && !applyWordRecords(L, patch, out, why))
        {
            return false;
        }
        lua_getfield(L, patch, "sections");
        if (lua_istable(L, -1))
        {
            const int sections = static_cast<int>(lua_objlen(L, -1));
            for (int k = 1; k <= sections; ++k)
            {
                lua_rawgeti(L, -1, k);
                if (!applyWordRecords(L, lua_gettop(L), out, why))
                {
                    return false;
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        lua_getfield(L, patch, "ops");
        if (!lua_istable(L, -1) && (records || hasWords(L, patch)))
        {
            lua_pop(L, 1);
            return true;
        }
        if (!lua_istable(L, -1))
        {
            why = "the patch has no ops";
            return false;
        }
        const int count = static_cast<int>(lua_objlen(L, -1));
        for (int i = 1; i <= count; ++i)
        {
            lua_rawgeti(L, -1, i);
            lua_rawgeti(L, -1, 1);
            const double first = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : -1;
            lua_pop(L, 1);
            lua_rawgeti(L, -1, 2);
            if (first < 0 || !lua_istable(L, -1))
            {
                why = "bad op " + std::to_string(i);
                return false;
            }
            const int n = static_cast<int>(lua_objlen(L, -1));
            if ((static_cast<size_t>(first) + static_cast<size_t>(n)) * 4 > out.size())
            {
                why = "op " + std::to_string(i) + " is past the end of the file";
                return false;
            }
            for (int k = 1; k <= n; ++k)
            {
                lua_rawgeti(L, -1, k);
                uint32_t word = 0;
                if (lua_isnumber(L, -1))
                {
                    word = static_cast<uint32_t>(static_cast<int64_t>(lua_tonumber(L, -1)));
                }
                else if (lua_istable(L, -1))
                {
                    lua_getfield(L, -1, "f");
                    const float f = static_cast<float>(lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 0.0);
                    lua_pop(L, 1);
                    std::memcpy(&word, &f, 4);
                }
                else
                {
                    why = "bad value in op " + std::to_string(i);
                    return false;
                }
                lua_pop(L, 1);
                std::memcpy(out.data() + (static_cast<size_t>(first) + static_cast<size_t>(k) - 1) * 4, &word, 4);
            }
            lua_pop(L, 2);
        }
        lua_pop(L, 1);
        return true;
    }

    // Compiled script files (FunkCode.bin, StartCode.bin): a stream of instructions `u16 opcode, u16 length (including these 4 bytes), operands`.
    // FunkCode.bin is cut into functions by the table of Vectoren.bin next to it (u32 count, then 84 byte records: char name[64], i32 start,
    // i32 length, ...). The patch (`kind = 'script'`, `file = 'funk' | 'start'`) lists edits; the names in them (CREATURE.X, OPCODE.X) come from the
    // mod's `constants/*.lua` (see loadConstants). Every edit is checked (function unique, opcode, the old bytes) before it is written:
    //   {fn, op, sector = {x, y}, [set_sector = {x, y}], [spawn = {old = {{creature, weight, group}, ...}, new = {...}}]}
    //       the FillSector instruction of the function for that sector (0x34 operand): its spawn table (the last operands, 13 bytes per entry: tag 0x19
    //       and three int32, after a 40 byte header) and/or the sector it fills;
    //   {op, var = 'Name', old = n, new = n}   the SetVar instruction `01 name\0 0B int32` of that variable;
    //   {fn, op, ins = n, at, old = 'hex', new = 'hex'}   a byte range in the n-th instruction.
    struct ScriptInstruction
    {
        size_t offset;
        unsigned opcode;
        size_t length;
    };

    bool scriptInstructions(const std::vector<unsigned char>& data, size_t begin, size_t end, std::vector<ScriptInstruction>& out)
    {
        out.clear();
        size_t at = begin;
        while (at < end)
        {
            if (at + 4 > end || at + 4 > data.size())
            {
                return false;
            }
            const unsigned opcode = data[at] | (data[at + 1] << 8);
            const size_t length = data[at + 2] | (data[at + 3] << 8);
            if (length < 4 || at + length > end)
            {
                return false;
            }
            out.push_back({at, opcode, length});
            at += length;
        }
        return true;
    }

    int32_t le32(const unsigned char* p)
    {
        return static_cast<int32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24));
    }

    void putLe32(std::vector<unsigned char>& out, int32_t value)
    {
        const uint32_t v = static_cast<uint32_t>(value);
        out.insert(out.end(), {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8), static_cast<unsigned char>(v >> 16),
                               static_cast<unsigned char>(v >> 24)});
    }

    constexpr size_t kFillSectorHeader = 40;
    constexpr size_t kSectorAt = 10;
    constexpr size_t kSpawnEntry = 13;

    // A FillSector instruction with a spawn table: its sector.
    bool fillSectorOf(const std::vector<unsigned char>& data, const ScriptInstruction& ins, int32_t& x, int32_t& y)
    {
        if (ins.length <= kFillSectorHeader || (ins.length - kFillSectorHeader) % kSpawnEntry || data[ins.offset + 9] != 0x34 ||
            data[ins.offset + 18] != 0x35)
        {
            return false;
        }
        x = le32(data.data() + ins.offset + kSectorAt);
        y = le32(data.data() + ins.offset + kSectorAt + 4);
        return true;
    }

    // A SetVar instruction `01 name\0 0B int32`: its name and where the int32 is.
    bool setVarOf(const std::vector<unsigned char>& data, const ScriptInstruction& ins, std::string& name, size_t& valueAt)
    {
        if (ins.length < 11 || data[ins.offset + 4] != 0x01)
        {
            return false;
        }
        size_t z = ins.offset + 5;
        while (z < ins.offset + ins.length && data[z] != 0)
        {
            ++z;
        }
        if (z + 6 != ins.offset + ins.length || data[z + 1] != 0x0B)
        {
            return false;
        }
        name.assign(reinterpret_cast<const char*>(data.data()) + ins.offset + 5, z - (ins.offset + 5));
        valueAt = z + 2;
        return true;
    }

    // The bytes of the spawn table `field` (`old` / `new`) of the edit at the stack top: entries {creature, weight, group} (names or numbers).
    bool spawnBytes(lua_State* L, int edit, const char* field, std::vector<unsigned char>& out, std::string& why)
    {
        out.clear();
        lua_getfield(L, edit, "spawn");
        if (!lua_istable(L, -1))
        {
            lua_pop(L, 1);
            why = "no spawn table";
            return false;
        }
        lua_getfield(L, -1, field);
        if (!lua_istable(L, -1))
        {
            lua_pop(L, 2);
            why = std::string("spawn.") + field + " is not a table";
            return false;
        }
        const int count = static_cast<int>(lua_objlen(L, -1));
        for (int i = 1; i <= count; ++i)
        {
            lua_rawgeti(L, -1, i);
            out.push_back(0x19);
            for (const char* key : {"creature", "weight", "group"})
            {
                lua_getfield(L, -1, key);
                if (!lua_isnumber(L, -1))
                {
                    lua_pop(L, 4);
                    why = std::string("a spawn entry has no number for ") + key + " (a name that is not in constants?)";
                    return false;
                }
                putLe32(out, static_cast<int32_t>(lua_tonumber(L, -1)));
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 2);
        return true;
    }

    bool pairField(lua_State* L, int edit, const char* field, int32_t& a, int32_t& b)
    {
        lua_getfield(L, edit, field);
        bool ok = lua_istable(L, -1);
        if (ok)
        {
            lua_rawgeti(L, -1, 1);
            lua_rawgeti(L, -2, 2);
            ok = lua_isnumber(L, -2) && lua_isnumber(L, -1);
            a = static_cast<int32_t>(ok ? lua_tonumber(L, -2) : 0);
            b = static_cast<int32_t>(ok ? lua_tonumber(L, -1) : 0);
            lua_pop(L, 2);
        }
        lua_pop(L, 1);
        return ok;
    }

    bool hasField(lua_State* L, int edit, const char* field)
    {
        lua_getfield(L, edit, field);
        const bool has = !lua_isnil(L, -1);
        lua_pop(L, 1);
        return has;
    }

    bool hexField(lua_State* L, int edit, const char* field, std::vector<unsigned char>& out)
    {
        out.clear();
        lua_getfield(L, edit, field);
        size_t n = 0;
        const char* hex = lua_isstring(L, -1) ? lua_tolstring(L, -1, &n) : nullptr;
        bool ok = hex && !(n & 1);
        for (size_t k = 0; ok && k < n; k += 2)
        {
            const int hi = hexValue(hex[k]), lo = hexValue(hex[k + 1]);
            ok = hi >= 0 && lo >= 0;
            out.push_back(static_cast<unsigned char>(hi * 16 + lo));
        }
        lua_pop(L, 1);
        return ok;
    }

    // Checks that `original` holds `expect` at `at` and writes `replacement` there (equal sizes, inside [begin, end)).
    bool replaceBytes(const std::vector<unsigned char>& original, std::vector<unsigned char>& out, size_t at, size_t limit,
                      const std::vector<unsigned char>& expect, const std::vector<unsigned char>& replacement, std::string& why)
    {
        if (expect.size() != replacement.size() || at + expect.size() > limit)
        {
            why = "the edit does not fit in the instruction";
            return false;
        }
        if (std::memcmp(original.data() + at, expect.data(), expect.size()) != 0)
        {
            why = "the file has other bytes than the edit expects";
            return false;
        }
        std::memcpy(out.data() + at, replacement.data(), replacement.size());
        return true;
    }

    bool applyScriptPatch(lua_State* L, int patch, const std::vector<unsigned char>& original, const std::string& originalPath,
                          std::vector<unsigned char>& out, std::string& why)
    {
        out = original;
        lua_getfield(L, patch, "file");
        const std::string file = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
        lua_pop(L, 1);
        const bool funk = file == "funk";
        if (!funk && file != "start")
        {
            why = "a script patch needs file = 'funk' or 'start'";
            return false;
        }
        struct Function
        {
            size_t start, length;
        };
        std::unordered_multimap<std::string, Function> functions;
        if (funk)
        {
            std::vector<unsigned char> table;
            const std::string path = originalPath.substr(0, originalPath.find_last_of('\\') + 1) + "Vectoren.bin";
            if (!readAll(path, table) || table.size() < 4)
            {
                why = "cannot read Vectoren.bin next to the file";
                return false;
            }
            const size_t count = static_cast<size_t>(le32(table.data()));
            if (4 + count * 84 > table.size())
            {
                why = "Vectoren.bin is shorter than its function count";
                return false;
            }
            for (size_t i = 0; i < count; ++i)
            {
                const unsigned char* rec = table.data() + 4 + i * 84;
                const std::string name(reinterpret_cast<const char*>(rec), strnlen(reinterpret_cast<const char*>(rec), 64));
                functions.emplace(name, Function{static_cast<size_t>(le32(rec + 64)), static_cast<size_t>(le32(rec + 68))});
            }
        }
        std::vector<ScriptInstruction> wholeFile;       // StartCode.bin: one stream
        if (!funk && !scriptInstructions(original, 0, original.size(), wholeFile))
        {
            why = "the file is not a stream of instructions";
            return false;
        }
        lua_getfield(L, patch, "edits");
        if (!lua_istable(L, -1))
        {
            why = "the patch has no edits";
            return false;
        }
        const int count = static_cast<int>(lua_objlen(L, -1));
        for (int i = 1; i <= count; ++i)
        {
            lua_rawgeti(L, -1, i);
            const int edit = lua_gettop(L);
            const std::string label = "edit " + std::to_string(i);
            auto number = [&](const char* field) {
                lua_getfield(L, edit, field);
                const double v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : -1;
                lua_pop(L, 1);
                return v;
            };
            std::vector<ScriptInstruction> list;
            const std::vector<ScriptInstruction>* instructions = &wholeFile;
            if (funk)
            {
                lua_getfield(L, edit, "fn");
                const std::string name = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
                lua_pop(L, 1);
                if (functions.count(name) != 1)
                {
                    why = label + ": function " + name + " found " + std::to_string(functions.count(name)) + " times";
                    return false;
                }
                const Function f = functions.find(name)->second;
                if (f.start > original.size() || f.start + f.length > original.size() || !scriptInstructions(original, f.start, f.start + f.length, list))
                {
                    why = label + ": function " + name + " is not a run of instructions";
                    return false;
                }
                instructions = &list;
            }
            const unsigned op = static_cast<unsigned>(number("op"));
            std::string problem;
            if (hasField(L, edit, "sector"))
            {
                int32_t x = 0, y = 0;
                if (!pairField(L, edit, "sector", x, y))
                {
                    why = label + ": bad sector";
                    return false;
                }
                const double ins = number("ins");
                size_t found = 0, index = 0;
                for (size_t k = 0; k < instructions->size(); ++k)
                {
                    int32_t sx = 0, sy = 0;
                    const ScriptInstruction& s = (*instructions)[k];
                    if (s.opcode == op && fillSectorOf(original, s, sx, sy) && sx == x && sy == y && (ins < 1 || static_cast<size_t>(ins) == k + 1))
                    {
                        ++found;
                        index = k;
                    }
                }
                if (found != 1)
                {
                    why = label + ": sector " + std::to_string(x) + "/" + std::to_string(y) + " found " + std::to_string(found) + " times";
                    return false;
                }
                const ScriptInstruction& s = (*instructions)[index];
                if (hasField(L, edit, "spawn"))
                {
                    std::vector<unsigned char> oldBytes, newBytes;
                    if (!spawnBytes(L, edit, "old", oldBytes, why) || !spawnBytes(L, edit, "new", newBytes, why))
                    {
                        why = label + ": " + why;
                        return false;
                    }
                    if (s.length - kFillSectorHeader != oldBytes.size() ||
                        !replaceBytes(original, out, s.offset + kFillSectorHeader, s.offset + s.length, oldBytes, newBytes, problem))
                    {
                        why = label + ": the spawn table: " + (problem.empty() ? "another size in the file" : problem);
                        return false;
                    }
                }
                int32_t nx = 0, ny = 0;
                if (hasField(L, edit, "set_sector"))
                {
                    std::vector<unsigned char> oldBytes, newBytes;
                    putLe32(oldBytes, x);
                    putLe32(oldBytes, y);
                    if (!pairField(L, edit, "set_sector", nx, ny))
                    {
                        why = label + ": bad set_sector";
                        return false;
                    }
                    putLe32(newBytes, nx);
                    putLe32(newBytes, ny);
                    if (!replaceBytes(original, out, s.offset + kSectorAt, s.offset + s.length, oldBytes, newBytes, problem))
                    {
                        why = label + ": the sector: " + problem;
                        return false;
                    }
                }
            }
            else if (hasField(L, edit, "var"))
            {
                lua_getfield(L, edit, "var");
                const std::string var = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
                lua_pop(L, 1);
                size_t found = 0, index = 0, valueAt = 0;
                for (size_t k = 0; k < instructions->size(); ++k)
                {
                    std::string name;
                    size_t at = 0;
                    if ((*instructions)[k].opcode == op && setVarOf(original, (*instructions)[k], name, at) && name == var)
                    {
                        ++found;
                        index = k;
                        valueAt = at;
                    }
                }
                if (found != 1 || number("old") < -2147483648.0 || number("new") < -2147483648.0)
                {
                    why = label + ": SetVar " + var + " found " + std::to_string(found) + " times";
                    return false;
                }
                std::vector<unsigned char> oldBytes, newBytes;
                putLe32(oldBytes, static_cast<int32_t>(number("old")));
                putLe32(newBytes, static_cast<int32_t>(number("new")));
                const ScriptInstruction& s = (*instructions)[index];
                if (!replaceBytes(original, out, valueAt, s.offset + s.length, oldBytes, newBytes, problem))
                {
                    why = label + ": the value of " + var + ": " + problem;
                    return false;
                }
            }
            else
            {
                const double ins = number("ins"), at = number("at");
                if (ins < 1 || at < 0 || static_cast<size_t>(ins) > instructions->size() || (*instructions)[static_cast<size_t>(ins) - 1].opcode != op)
                {
                    why = label + ": the instruction is not there or has another opcode";
                    return false;
                }
                const ScriptInstruction& s = (*instructions)[static_cast<size_t>(ins) - 1];
                std::vector<unsigned char> oldBytes, newBytes;
                if (!hexField(L, edit, "old", oldBytes) || !hexField(L, edit, "new", newBytes))
                {
                    why = label + ": bad hex";
                    return false;
                }
                if (!replaceBytes(original, out, s.offset + static_cast<size_t>(at), s.offset + s.length, oldBytes, newBytes, problem))
                {
                    why = label + ": " + problem;
                    return false;
                }
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        return true;
    }

    // The mod's `constants/*.lua`: each file returns a table of names to numbers and becomes the global of its upper case name (creature.lua ->
    // CREATURE) for the patch files. They are plain data too (no libraries).
    std::vector<unsigned char> constantsFiles(const std::string& modDir)
    {
        std::vector<unsigned char> all;
        WIN32_FIND_DATAW fd;
        const HANDLE h = FindFirstFileW(widen(modDir + "\\constants\\*.lua").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
        {
            return all;
        }
        do
        {
            std::vector<unsigned char> text;
            if (readAll(modDir + "\\constants\\" + narrow(fd.cFileName), text))
            {
                const std::string name = narrow(fd.cFileName);
                all.insert(all.end(), name.begin(), name.end());
                all.insert(all.end(), text.begin(), text.end());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return all;
    }

    bool loadConstants(lua_State* L, const std::string& modDir, std::string& why)
    {
        WIN32_FIND_DATAW fd;
        const HANDLE h = FindFirstFileW(widen(modDir + "\\constants\\*.lua").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
        {
            return true;
        }
        bool ok = true;
        do
        {
            std::string name = narrow(fd.cFileName);
            const std::string path = modDir + "\\constants\\" + name;
            name.erase(name.size() - 4);
            for (char& c : name)
            {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            if (luaL_loadfile(L, path.c_str()) != 0 || lua_pcall(L, 0, 1, 0) != 0 || !lua_istable(L, -1))
            {
                why = "cannot read constants\\" + narrow(fd.cFileName) + ": " + (lua_isstring(L, -1) ? lua_tostring(L, -1) : "no table");
                ok = false;
                break;
            }
            lua_setglobal(L, name.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return ok;
    }

    // Applies the patch file to `original` in memory; false (and a message) if it does not fit.
    bool applyPatch(const std::string& patchFile, const std::string& originalPath, const std::string& modDir, const std::vector<unsigned char>& original,
                    std::vector<unsigned char>& out, std::string& why)
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
        if (!loadConstants(L, modDir, why))
        {
            return false;
        }
        if (luaL_loadfile(L, patchFile.c_str()) != 0 || lua_pcall(L, 0, 1, 0) != 0 || !lua_istable(L, -1))
        {
            why = std::string("cannot read the patch: ") + (lua_isstring(L, -1) ? lua_tostring(L, -1) : "no table");
            return false;
        }
        lua_getfield(L, -1, "kind");
        const std::string patchKind = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
        lua_pop(L, 1);
        if (patchKind == "script")
        {
            // no size or crc32: every edit is checked against the bytes it replaces
            return applyScriptPatch(L, lua_gettop(L), original, originalPath, out, why);
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
        const std::string kind = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
        lua_pop(L, 1);
        if (kind == "texts")
        {
            return applyTextPatch(L, lua_gettop(L), original, out, why);
        }
        if (kind == "words")
        {
            return applyWordPatch(L, lua_gettop(L), original, out, why);
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
            char key[96];
            const std::vector<unsigned char> constants = constantsFiles(narrow(r.dir));
            std::snprintf(key, sizeof(key), "%08x-%08x-%zu-%08x", crc32Of(patchText.data(), patchText.size()), crc32Of(original.data(), original.size()),
                          original.size(), crc32Of(constants.data(), constants.size()));
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
                if (!applyPatch(patchFile, originalPath, narrow(r.dir), original, patched, why))
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
                    if (++g_served[rel] == 1 && g_server)
                    {
                        LOG("Overlay: the gameserver opened {} -> {} (mod {})", rel, out, r.id);
                    }
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

    std::string readAll(const std::wstring& path)
    {
        std::ifstream file(path, std::ios::binary);
        std::stringstream ss;
        ss << file.rdbuf();
        return ss.str();
    }

    // The player's flag for `id` in gear_mods.json (the run's own copy in an automation run): 1 on, 0 off, -1 none.
    // A scan, not a JSON parser: finds "mods", then the object of `id`, then its own "enabled" (depth 1 of that object).
    int stateFlag(const std::wstring& gameDir, const std::string& id)
    {
        const std::wstring path = Instance::automation()
            ? Instance::directory(gameDir) + L"\\gear_mods.json"
            : gameDir + L"\\mods\\gear_mods.json";
        const std::string text = readAll(path);
        size_t at = text.find("\"mods\"");
        if (at == std::string::npos)
        {
            return -1;
        }
        at = text.find("\"" + id + "\"", at);
        if (at == std::string::npos)
        {
            return -1;
        }
        at = text.find('{', at);
        if (at == std::string::npos)
        {
            return -1;
        }
        int depth = 0;
        for (size_t i = at; i < text.size(); ++i)
        {
            const char c = text[i];
            if (c == '"')
            {
                const size_t end = [&] {
                    size_t j = i + 1;
                    while (j < text.size() && text[j] != '"')
                    {
                        j += text[j] == '\\' ? 2 : 1;
                    }
                    return j;
                }();
                if (depth == 1 && text.compare(i, 9, "\"enabled\"") == 0)
                {
                    const size_t colon = text.find(':', end);
                    const size_t v = colon == std::string::npos ? colon : text.find_first_not_of(" \t\r\n", colon + 1);
                    if (v != std::string::npos)
                    {
                        return text.compare(v, 4, "true") == 0 ? 1 : text.compare(v, 5, "false") == 0 ? 0 : -1;
                    }
                }
                i = end;
            }
            else if (c == '{' || c == '[')
            {
                ++depth;
            }
            else if (c == '}' || c == ']')
            {
                if (--depth == 0)
                {
                    break;
                }
            }
        }
        return -1;
    }

    // Whether the mod is switched on, as the mod manager sees it: the player's flag, else mod.lua's top-level `enabled = false`.
    // A mod that is off gets no overlay: its files are not served and its save folder is not used.
    bool modEnabled(const std::wstring& gameDir, const std::wstring& modDir, const std::string& id)
    {
        const int flag = stateFlag(gameDir, id);
        if (flag >= 0)
        {
            return flag == 1;
        }
        std::stringstream lines(readAll(modDir + L"\\mod.lua"));
        std::string line;
        while (std::getline(lines, line))
        {
            const size_t b = line.find_first_not_of(" \t");
            if (b != std::string::npos && b <= 4 && line.compare(b, 7, "enabled") == 0)
            {
                const size_t eq = line.find('=', b + 7);
                const size_t v = eq == std::string::npos ? eq : line.find_first_not_of(" \t", eq + 1);
                if (v != std::string::npos && line.compare(v, 5, "false") == 0)
                {
                    return false;
                }
            }
        }
        return true;
    }

    using CreateProcessAFn = BOOL(WINAPI*)(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD,
                                           LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);
    CreateProcessAFn g_createProcess = nullptr;
    std::string g_handoff;      // the mods with file rules, comma separated

    // The gameserver that sacred.exe starts for a LAN game or a hosted game reads the data files itself (balance.bin,
    // the scripts): it must see the same ones as the client. Gear gets into it as SacredBild does, by Detours, and
    // hands it the ids of the mods the client overlays through the environment (the child inherits it).
    BOOL WINAPI hookCreateProcessA(LPCSTR app, LPSTR cmd, LPSECURITY_ATTRIBUTES processAttr, LPSECURITY_ATTRIBUTES threadAttr,
                                   BOOL inherit, DWORD flags, LPVOID env, LPCSTR dir, LPSTARTUPINFOA startup,
                                   LPPROCESS_INFORMATION info)
    {
        const char* name = app ? std::strrchr(app, '\\') : nullptr;
        if (app && _stricmp(name ? name + 1 : app, "GameServer.exe") == 0)
        {
            if (env)
            {
                LOG("Overlay: the gameserver gets its own environment: the mods' files are not handed to it");
            }
            else
            {
                SetEnvironmentVariableA(kHandoff, g_handoff.c_str());
                HMODULE self = nullptr;
                char dll[MAX_PATH] = {};
                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(&hookCreateProcessA), &self);
                GetModuleFileNameA(self, dll, MAX_PATH);
                if (DetourCreateProcessWithDllExA(app, cmd, processAttr, threadAttr, inherit, flags, env, dir, startup, info,
                                                  dll, g_createProcess))
                {
                    LOG("Overlay: {} started with Sacred Gear, mods: {}", app, g_handoff);
                    return TRUE;
                }
                LOG("Overlay: starting {} with Sacred Gear failed ({}); starting it without", app, GetLastError());
            }
        }
        return g_createProcess(app, cmd, processAttr, threadAttr, inherit, flags, env, dir, startup, info);
    }

    void hookServerLaunch()
    {
        g_handoff.clear();
        for (const ModRules& r : g_rules)
        {
            if (!r.files.empty())
            {
                g_handoff += (g_handoff.empty() ? "" : ",") + r.id;
            }
        }
        if (g_handoff.empty())
        {
            return;
        }
        g_createProcess = reinterpret_cast<CreateProcessAFn>(
            Patch::iat("KERNEL32.dll", "CreateProcessA", reinterpret_cast<void*>(&hookCreateProcessA)));
        if (!g_createProcess)
        {
            LOG("Overlay: CreateProcessA is not imported: a gameserver will not get the mods' files");
        }
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
                if (g_server)
                {
                    continue;       // the gameserver has no savegames
                }
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
    std::vector<std::string> selected = selectedMods();
    if (g_server)
    {
        // The client decided which mods apply (the ones it overlays itself): the same ids, read from the same folder.
        selected.clear();
        char list[2048] = {};
        GetEnvironmentVariableA(kHandoff, list, sizeof(list) - 1);
        std::stringstream ss(list);
        std::string item;
        while (std::getline(ss, item, ','))
        {
            if (!item.empty())
            {
                selected.push_back(item);
            }
        }
        if (selected.empty())
        {
            return false;
        }
    }
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
            if (!g_server && !modEnabled(gameDir, gameDir + L"\\mods\\" + name, id))
            {
                LOG("Overlay: mod {} is switched off: no file rules, saves untouched", id);
                continue;
            }
            ModRules rules;
            if (readRules(gameDir + L"\\mods\\" + name, id, rules) && !(g_server && rules.files.empty()))
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
        // gameserver.exe lacks most of these imports: not worth a log line there
        void* previous = Patch::iat("KERNEL32.dll", name, replacement, g_server);
        // SacredBild reads the language files with these two
        if (!g_server && (std::strcmp(name, "CreateFileA") == 0 || std::strcmp(name, "GetFileAttributesA") == 0))
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
    // gameserver.exe imports only CreateFileA, FindFirstFileA and DeleteFileA of these: it needs just the open.
    g_installed = g_createFile && (g_server || (g_getAttributes && g_findFirst));
    if (!g_installed)
    {
        LOG("Overlay: the game's file functions are not imported as expected: overlay off");
        g_rules.clear();
        return false;
    }
    LOG("Overlay: the {} file functions are hooked ({} mod(s))", g_server ? "gameserver's" : "game's", g_rules.size());
    if (!g_server)
    {
        hookServerLaunch();
    }
    return true;
}

DWORD Overlay::startGameServerForTest(int seconds)
{
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string app = exe;
    app.erase(app.find_last_of('\\') + 1);
    app += "GameServer.exe";
    std::string dir = app.substr(0, app.find_last_of('\\'));
    STARTUPINFOA startup = {sizeof(startup)};
    PROCESS_INFORMATION info = {};
    // As the game starts it: through the import's hook when the overlay installed one, else the plain call.
    const BOOL ok = g_createProcess
        ? hookCreateProcessA(app.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &startup, &info)
        : CreateProcessA(app.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &startup, &info);
    if (!ok)
    {
        return 0;
    }
    WaitForSingleObject(info.hProcess, static_cast<DWORD>(seconds) * 1000);
    TerminateProcess(info.hProcess, 0);
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return info.dwProcessId;
}

bool Overlay::installServer(const std::wstring& gameDir)
{
    g_server = true;
    return install(gameDir);
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
