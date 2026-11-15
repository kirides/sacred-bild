#include "overlay.h"
#include "log.h"
#include "patch.h"

#include <windows.h>
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
