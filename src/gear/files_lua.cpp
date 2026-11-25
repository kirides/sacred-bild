// The Lua host's natives for files: named pipes, the mods' state files, mod data scopes, writing files.
// Split out of lua_host.cpp; the shared helpers are in lua_native.h.
#include "lua_host.h"
#include "automation.h"
#include "chat.h"
#include "world/classes.h"
#include "items/itemuse.h"
#include "items/drops.h"
#include "gamecfg.h"
#include "gear.h"
#include "instance.h"
#include "items/inventory.h"
#include "items/itemtip.h"
#include "items/itemicon.h"
#include "ui/keyblock.h"
#include "world/killstat.h"
#include "ui/input.h"
#include "log.h"
#include "netmsg.h"
#include "items/pickup.h"
#include "items/potionpick.h"
#include "items/shop.h"
#include "realm.h"
#include "ui/replace.h"
#include "ui/rml.h"
#include "overlay.h"
#include "world/buffs.h"
#include "world/pfx.h"
#include "world/party.h"
#include "ui/padnav.h"
#include "pipe.h"
#include "save.h"
#include "savegames.h"
#include "sitehook.h"
#include "items/master.h"
#include "tables.h"
#include "engine/gear_addr.h"
#include "ui/ui.h"
#include "world/world.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <lua.hpp>
#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>
#include <thread>
#include <string>
#include <vector>
#include "lua_native.h"

namespace LuaNative
{
    // native.pipe_open(name) -> handle | nil. The kernel checks the mod's permission first.
    int nativePipeOpen(lua_State* L)
    {
        size_t n = 0;
        const char* name = luaL_checklstring(L, 1, &n);
        const int handle = Pipes::open(std::string(name, n));
        if (!handle)
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushnumber(L, handle);
        return 1;
    }

    int nativePipeClose(lua_State* L)
    {
        Pipes::close(static_cast<int>(luaL_checknumber(L, 1)));
        return 0;
    }

    // native.pipe_send(handle, text) -> bool
    int nativePipeSend(lua_State* L)
    {
        const int handle = static_cast<int>(luaL_checknumber(L, 1));
        size_t n = 0;
        const char* text = luaL_checklstring(L, 2, &n);
        lua_pushboolean(L, Pipes::send(handle, std::string(text, n)));
        return 1;
    }

    // native.pipe_poll(handle, limit) -> {{kind, text}, ...}; kind "connect", "disconnect" or "message"
    int nativePipePoll(lua_State* L)
    {
        const int handle = static_cast<int>(luaL_checknumber(L, 1));
        const lua_Number limit = luaL_optnumber(L, 2, 64);
        const std::vector<Pipes::Event> events = Pipes::poll(handle, limit >= 1 && limit < 4096 ? static_cast<size_t>(limit) : 64);
        lua_createtable(L, static_cast<int>(events.size()), 0);
        for (size_t i = 0; i < events.size(); ++i)
        {
            const Pipes::Event& e = events[i];
            lua_createtable(L, 2, 0);
            lua_pushstring(L, e.kind == Pipes::Event::Connected ? "connect"
                              : e.kind == Pipes::Event::Disconnected ? "disconnect" : "message");
            lua_rawseti(L, -2, 1);
            lua_pushlstring(L, e.text.data(), e.text.size());
            lua_rawseti(L, -2, 2);
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        return 1;
    }

    int nativePipeConnected(lua_State* L)
    {
        lua_pushboolean(L, Pipes::connected(static_cast<int>(luaL_checknumber(L, 1))));
        return 1;
    }

    // native.state_read(which?) -> text | nil. `which` 'realms' reads gear_realms.json (default: gear_mods.json).
    int nativeStateRead(lua_State* L)
    {
        std::string text;
        if (!readFile(stateFile(L, 1), text))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushlstring(L, text.data(), text.size());
        return 1;
    }

    // native.state_write(text, backup) -> bool. Atomic (a temporary file moved over the old one); `backup` true
    // writes gear_mods.json.bak instead (the copy of a file the kernel could not read).
    int nativeStateWrite(lua_State* L)
    {
        size_t n = 0;
        const char* text = luaL_checklstring(L, 1, &n);
        const bool backup = lua_toboolean(L, 2) != 0;
        bool ok = false;
        if (n <= 1024 * 1024)
        {
            const std::wstring path = stateFile(L, 3) + (backup ? L".bak" : L"");
            const std::wstring temp = path + L".tmp";
            HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            DWORD written = 0;
            ok = f != INVALID_HANDLE_VALUE && WriteFile(f, text, static_cast<DWORD>(n), &written, nullptr) &&
                 written == n && FlushFileBuffers(f);
            if (f != INVALID_HANDLE_VALUE)
            {
                CloseHandle(f);
            }
            ok = ok && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            if (!ok)
            {
                DeleteFileW(temp.c_str());
            }
        }
        lua_pushboolean(L, ok);
        return 1;
    }

    // Atomic: written to <path>.tmp, flushed, then moved over the file, so a crash leaves the old file or the new one,
    // never half of one.
    bool writeAtomic(const std::wstring& path, const char* text, size_t n)
    {
        const std::wstring temp = path + L".tmp";
        HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written = 0;
        bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, text, static_cast<DWORD>(n), &written, nullptr) &&
                  written == n && FlushFileBuffers(f);
        if (f != INVALID_HANDLE_VALUE)
        {
            CloseHandle(f);
        }
        ok = ok && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        if (!ok)
        {
            DeleteFileW(temp.c_str());
        }
        return ok;
    }

    // Per savegame data (gear.savegame): saves\<scope>\<name> of a mod's folder. A scope and a file name are made of
    // letters, digits, '_', '-' and '.'.
    bool plainName(const char* s, size_t n)
    {
        if (n == 0 || n > 96 || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.'))
        {
            return false;
        }
        for (size_t i = 0; i < n; ++i)
        {
            const char ch = s[i];
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.'))
            {
                return false;
            }
        }
        return true;
    }

    std::wstring scopeDir(const char* dir, size_t dirLen, const char* scope, size_t scopeLen)
    {
        if (!plainName(scope, scopeLen))
        {
            return {};
        }
        const std::string rel = "saves/" + std::string(scope, scopeLen);
        return modPath(dir, dirLen, rel.data(), rel.size());
    }

    // Every missing directory of a path inside the mods folder.
    void makeDirs(const std::wstring& path)
    {
        for (size_t i = 3; i < path.size(); ++i)
        {
            if (path[i] == L'\\')
            {
                CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
            }
        }
        CreateDirectoryW(path.c_str(), nullptr);
    }

    bool dataExtension(std::string name)
    {
        for (char& ch : name)
        {
            ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
        }
        auto ends = [&name](const char* e) { const size_t k = std::strlen(e); return name.size() > k && name.compare(name.size() - k, k, e) == 0; };
        return ends(".json") || ends(".txt");
    }

    // native.scope_read(dir, scope, name) -> text | nil, message
    int nativeScopeRead(lua_State* L)
    {
        size_t dl = 0, sl = 0, nl = 0;
        const char* dir = luaL_checklstring(L, 1, &dl);
        const char* scope = luaL_checklstring(L, 2, &sl);
        const char* name = luaL_checklstring(L, 3, &nl);
        const std::wstring root = scopeDir(dir, dl, scope, sl);
        if (root.empty() || !plainName(name, nl))
        {
            return pushFailure(L, "bad scope or file name");
        }
        std::string text;
        if (!readFile(root + L"\\" + widen(name, nl), text))
        {
            return pushFailure(L, "cannot read the file");
        }
        lua_pushlstring(L, text.data(), text.size());
        return 1;
    }

    // native.scope_write(dir, scope, name, text) -> true | nil, message
    int nativeScopeWrite(lua_State* L)
    {
        size_t dl = 0, sl = 0, nl = 0, n = 0;
        const char* dir = luaL_checklstring(L, 1, &dl);
        const char* scope = luaL_checklstring(L, 2, &sl);
        const char* name = luaL_checklstring(L, 3, &nl);
        const char* text = luaL_checklstring(L, 4, &n);
        const std::wstring root = scopeDir(dir, dl, scope, sl);
        if (root.empty() || !plainName(name, nl) || !dataExtension(std::string(name, nl)))
        {
            return pushFailure(L, "bad scope or file name (.json or .txt)");
        }
        if (n > 8 * 1024 * 1024)
        {
            return pushFailure(L, "the text is larger than 8 MB");
        }
        makeDirs(root);
        if (!writeAtomic(root + L"\\" + widen(name, nl), text, n))
        {
            return pushFailure(L, "cannot write the file");
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    // Deletes the files directly in `root` (not the directory).
    void clearDir(const std::wstring& root)
    {
        WIN32_FIND_DATAW fd{};
        HANDLE find = FindFirstFileW((root + L"\\*").c_str(), &fd);
        if (find == INVALID_HANDLE_VALUE)
        {
            return;
        }
        do
        {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                DeleteFileW((root + L"\\" + fd.cFileName).c_str());
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }

    // native.scope_clear(dir, scope) -> true | nil, message: removes the scope's files.
    int nativeScopeClear(lua_State* L)
    {
        size_t dl = 0, sl = 0;
        const char* dir = luaL_checklstring(L, 1, &dl);
        const char* scope = luaL_checklstring(L, 2, &sl);
        const std::wstring root = scopeDir(dir, dl, scope, sl);
        if (root.empty())
        {
            return pushFailure(L, "bad scope name");
        }
        clearDir(root);
        RemoveDirectoryW(root.c_str());
        lua_pushboolean(L, 1);
        return 1;
    }

    // native.scope_copy(dir, from, to) -> count | nil, message: `to` gets exactly the files of `from` (an absent `from`
    // empties it).
    int nativeScopeCopy(lua_State* L)
    {
        size_t dl = 0, fl = 0, tl = 0;
        const char* dir = luaL_checklstring(L, 1, &dl);
        const char* from = luaL_checklstring(L, 2, &fl);
        const char* to = luaL_checklstring(L, 3, &tl);
        const std::wstring src = scopeDir(dir, dl, from, fl), dst = scopeDir(dir, dl, to, tl);
        if (src.empty() || dst.empty())
        {
            return pushFailure(L, "bad scope name");
        }
        if (_wcsicmp(src.c_str(), dst.c_str()) == 0)
        {
            lua_pushnumber(L, 0);
            return 1;
        }
        clearDir(dst);
        int count = 0;
        WIN32_FIND_DATAW fd{};
        HANDLE find = FindFirstFileW((src + L"\\*").c_str(), &fd);
        if (find != INVALID_HANDLE_VALUE)
        {
            makeDirs(dst);
            do
            {
                const std::wstring name = fd.cFileName;
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (name.size() > 4 && name.compare(name.size() - 4, 4, L".tmp") == 0))
                {
                    continue;
                }
                if (CopyFileW((src + L"\\" + name).c_str(), (dst + L"\\" + name).c_str(), FALSE))
                {
                    ++count;
                }
            } while (FindNextFileW(find, &fd));
            FindClose(find);
        }
        else
        {
            RemoveDirectoryW(dst.c_str());
        }
        lua_pushnumber(L, count);
        return 1;
    }

    // native.scope_of(slot) / native.hero_scope_of(number) -> the scope name of a savegame / network hero file.
    int nativeScopeOf(lua_State* L)
    {
        const std::string name = Savegames::scopeOf(static_cast<uint32_t>(luaL_checknumber(L, 1)));
        lua_pushlstring(L, name.data(), name.size());
        return 1;
    }

    int nativeHeroScopeOf(lua_State* L)
    {
        const std::string name = Savegames::heroScopeOf(static_cast<uint32_t>(luaL_checknumber(L, 1)));
        lua_pushlstring(L, name.data(), name.size());
        return 1;
    }

    // native.game_command() -> kind, number, scope | nil: what the game was told to do (src/gear/savegames.cpp), oldest
    // first: 'load' (a savegame slot), 'hero' (a network hero file), 'new' (a new game).
    int nativeGameCommand(lua_State* L)
    {
        Savegames::Command c;
        if (!Savegames::take(c))
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushstring(L, c.kind == Savegames::Command::Kind::Load ? "load"
                          : c.kind == Savegames::Command::Kind::LoadHero ? "hero" : "new");
        lua_pushnumber(L, c.number);
        lua_pushstring(L, c.scope.c_str());
        return 3;
    }

    // native.write_file(dir, relative, text) -> true | nil, message. A mod's own folder only, and only files named
    // *.json, *.txt or *.lua that are not mod.lua or main.lua (a mod keeps its settings, it does not rewrite itself).
    int nativeWriteFile(lua_State* L)
    {
        size_t dirLen = 0, relLen = 0, n = 0;
        const char* dir = luaL_checklstring(L, 1, &dirLen);
        const char* rel = luaL_checklstring(L, 2, &relLen);
        const char* text = luaL_checklstring(L, 3, &n);
        const char* error = nullptr;
        {
            const std::string r(rel, relLen);
            std::string lower = r;
            for (char& ch : lower)
            {
                ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
            }
            auto endsWith = [&lower](const char* s) {
                const size_t k = std::strlen(s);
                return lower.size() >= k && lower.compare(lower.size() - k, k, s) == 0;
            };
            const std::wstring path = modPath(dir, dirLen, rel, relLen);
            if (path.empty() || r.find_first_of("\\/") != std::string::npos)
            {
                error = "only files directly in the mod's folder";
            }
            else if (!(endsWith(".json") || endsWith(".txt") || endsWith(".lua")) || lower == "mod.lua" || lower == "main.lua")
            {
                error = "only .json, .txt and .lua files other than mod.lua and main.lua";
            }
            else if (n > 8 * 1024 * 1024)
            {
                error = "the text is larger than 8 MB";
            }
            else
            {
                if (!writeAtomic(path, text, n))
                {
                    error = "cannot write the file";
                }
            }
        }
        if (error)
        {
            return pushFailure(L, error);
        }
        lua_pushboolean(L, 1);
        return 1;
    }


    void registerFiles(lua_State* L, bool automation)
    {
        static constexpr luaL_Reg kNatives[] = {
            {"pipe_open", nativePipeOpen},
            {"pipe_close", nativePipeClose},
            {"pipe_send", nativePipeSend},
            {"pipe_poll", nativePipePoll},
            {"pipe_connected", nativePipeConnected},
            {"state_read", nativeStateRead},
            {"state_write", nativeStateWrite},
            {"scope_read", nativeScopeRead},
            {"scope_write", nativeScopeWrite},
            {"scope_clear", nativeScopeClear},
            {"scope_copy", nativeScopeCopy},
            {"scope_of", nativeScopeOf},
            {"hero_scope_of", nativeHeroScopeOf},
            {"game_command", nativeGameCommand},
            {"write_file", nativeWriteFile},
            {nullptr, nullptr},
        };
        setFunctions(L, kNatives);
        (void)automation;
    }
}
