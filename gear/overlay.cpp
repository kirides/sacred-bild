#include "overlay.h"
#include "log.h"
#include "patch.h"
#include "instance.h"

#include <windows.h>
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
                rules.files[game] = dir + "\\" + backslashes(value);
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
        void* previous = Patch::iat("KERNEL32.dll", name, replacement);
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
