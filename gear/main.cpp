#include "automation.h"
#include "gear.h"
#include "hooks.h"
#include "instance.h"
#include "log.h"
#include "lua_host.h"
#include "sig.h"
#include "game/gear_addr.h"

#include <windows.h>
#include <cstring>
#include <cwchar>
#include <string>

// Sacred Gear's entry: DllMain only finds the game's code and installs the hooks, which is safe under the loader
// lock and at any time (process start through an ASI loader or SacredBild, or later through an injector). Lua starts
// on the engine thread with the first in-game frame (lua_host.cpp), not here.

namespace
{
#include "game/gear_sigs.inc"

    std::wstring exePath()
    {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        return path;
    }

    std::wstring directoryOf(const std::wstring& path)
    {
        return path.substr(0, path.find_last_of(L"\\/"));
    }

    // Only the game itself: not gameserver.exe (which sacred.exe starts for LAN games, and which an injector or
    // SacredBild's own injection might reach), the config tool or any other process.
    bool isGame(const std::wstring& exe)
    {
        const size_t slash = exe.find_last_of(L"\\/");
        const wchar_t* name = exe.c_str() + (slash == std::wstring::npos ? 0 : slash + 1);
        return _wcsicmp(name, L"sacred.exe") == 0 || _wcsicmp(name, L"SacredReborn.exe") == 0;
    }

    // One Sacred Gear per process: with an ASI loader and SacredBild both present, SacredGear.asi and
    // SacredGear.dll are two modules. The first one claims the process (a variable in its environment, tagged with
    // the process id because child processes inherit the environment); later ones stay loaded but do nothing.
    constexpr const wchar_t* kInstanceVariable = L"SACREDGEAR_CLAIM";

    bool claimProcess(HMODULE module)
    {
        wchar_t value[64] = {};
        const std::wstring mine = std::to_wstring(GetCurrentProcessId()) + L":" +
            std::to_wstring(reinterpret_cast<uintptr_t>(module));
        const DWORD n = GetEnvironmentVariableW(kInstanceVariable, value, static_cast<DWORD>(std::size(value)));
        if (n > 0 && n < std::size(value))
        {
            const std::wstring current(value, n);
            if (current.substr(0, current.find(L':')) == std::to_wstring(GetCurrentProcessId()))
            {
                return false;
            }
        }
        SetEnvironmentVariableW(kInstanceVariable, mine.c_str());
        return true;
    }

    // Steam's exe is the GOG one wrapped by SteamStub, whose entry point lies outside .text; its code is encoded
    // until the wrapper ran, so a Sacred Gear loaded at process start finds nothing there.
    bool wrappedExe()
    {
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        const uintptr_t entry = base + nt->OptionalHeader.AddressOfEntryPoint;
        const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
        {
            if (std::memcmp(section->Name, ".text", 6) == 0)
            {
                const uintptr_t first = base + section->VirtualAddress;
                return entry < first || entry >= first + section->Misc.VirtualSize;
            }
        }
        return false;
    }

    void start(HMODULE module)
    {
        const std::wstring exe = exePath();
        if (!isGame(exe))
        {
            return;
        }
        if (!claimProcess(module))
        {
            // The first instance logs; this one stays silent so its log does not replace that one's.
            return;
        }
        const std::wstring gameDir = directoryOf(exe);
        Log::init(Instance::logPath(gameDir, L"SacredGear.log").c_str());
        LOG("Sacred Gear {} " __DATE__ " " __TIME__, Gear::kVersion);
        Automation::installInstance(gameDir);
        const size_t missing = Sig::resolve(kGearSigs);
        if (missing)
        {
            LOG("{} of {} game addresses not found; the features that need them stay off", missing,
                std::size(kGearSigs));
        }
        if (!Gear::Addr::cEngine_updateZoom)
        {
            LOG("The frame tick was not found: Sacred Gear stays off");
            if (wrappedExe())
            {
                LOG("The exe is wrapped (Steam): its code may still be encoded when Sacred Gear loads; load it "
                    "through SacredBild, which waits for the game's own code");
            }
            return;
        }
        LuaHost::configure(module, gameDir);
        if (!Hooks::install())
        {
            LOG("Hooks not installed: Sacred Gear stays off");
            return;
        }
        LOG("Hooks installed; mods load with the first in-game frame");
        Automation::install(gameDir);
    }
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
        start(instance);
    }
    return TRUE;
}
