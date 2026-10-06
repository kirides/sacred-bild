#include "config.h"
#include "crash_dump.h"
#include "log.h"
#include "patch.h"
#include "profiler.h"
#include "proxy.h"
#include "settings_window.h"
#include "game/build.h"
#include "game/gameserver.h"

#include <windows.h>
#include <detours/detours.h>
#include <intrin.h>
#include <cstring>
#include <string>
#include <utility>

namespace
{
    // Physical pixels for GetSystemMetrics and no DWM scaling of the game window.
    void enableDpiAwareness()
    {
        using SetContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        auto set = reinterpret_cast<SetContextFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
        if (set && !set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) && GetLastError() != ERROR_ACCESS_DENIED)
        {
            LOG("SetProcessDpiAwarenessContext failed: {}", GetLastError());
        }
    }

    std::wstring exeDirectory()
    {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring dir = path;
        return dir.substr(0, dir.find_last_of(L"\\/"));
    }

    // From here on everything reads g_config.
    bool startGame(const std::wstring& gameDir)
    {
        if (!Proxy::init(gameDir))
        {
            return false;
        }
        if (Sacred::resolveAddresses())
        {
            Sacred::installHooks();
        }
        else
        {
            LOG("Unsupported sacred.exe build: game hooks disabled, ddraw is passed through");
        }
        return true;
    }

    // The game starts after DllMain: the settings window can't be shown under the loader lock, and a wrapped exe's
    // code is still encoded. Then the settings window, if wanted, and SacredBild with the settings it saved.
    HMODULE g_module = nullptr;
    std::wstring g_gameDir;
    bool g_settingsWindow = false;

    void gameStarts()
    {
        if (g_settingsWindow && !SettingsWindow::show(g_module, g_gameDir))
        {
            ExitProcess(0);
        }
        if (!startGame(g_gameDir))
        {
            ExitProcess(1);
        }
    }

    int(WINAPI* g_entryPoint)() = nullptr;

    int WINAPI entryPoint()
    {
        gameStarts();
        return g_entryPoint();
    }

    bool deferToEntryPoint()
    {
        g_entryPoint = reinterpret_cast<int(WINAPI*)()>(DetourGetEntryPoint(nullptr));
        Patch::begin();
        const bool hooked = Patch::hook(g_entryPoint, entryPoint, "exe entry point");
        return Patch::commit() && hooked;
    }

    // The exe's code section: [first, last).
    std::pair<uintptr_t, uintptr_t> codeSection()
    {
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
        {
            if (std::memcmp(section->Name, ".text", 6) == 0)
            {
                return {base + section->VirtualAddress, base + section->VirtualAddress + section->Misc.VirtualSize};
            }
        }
        return {0, 0};
    }

    // Steam's Sacred.exe is the GOG exe wrapped by SteamStub: its entry point is the wrapper's (section .bind), which
    // decodes .text and then runs the game's own entry point.
    bool wrappedExe()
    {
        const auto [first, last] = codeSection();
        const auto entry = reinterpret_cast<uintptr_t>(DetourGetEntryPoint(nullptr));
        return first && (entry < first || entry >= last);
    }

    // A wrapped exe's code is decoded once its own startup runs: the C runtime's first call is GetVersion (`0x84D98A`),
    // the first call of an import from .text.
    using GetVersionFn = DWORD(WINAPI*)();
    GetVersionFn g_getVersion = nullptr;
    std::pair<uintptr_t, uintptr_t> g_code;

    DWORD WINAPI hookGetVersion()
    {
        const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
        if (caller >= g_code.first && caller < g_code.second)
        {
            Patch::iat("kernel32.dll", "GetVersion", reinterpret_cast<void*>(g_getVersion));
            LOG("Wrapped exe: its code runs (GetVersion from {:08x})", caller);
            gameStarts();
        }
        return g_getVersion();
    }

    bool deferToDecodedCode()
    {
        g_code = codeSection();
        g_getVersion = reinterpret_cast<GetVersionFn>(Patch::iat("kernel32.dll", "GetVersion", reinterpret_cast<void*>(hookGetVersion)));
        return g_getVersion != nullptr;
    }
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
        const std::wstring gameDir = exeDirectory();
        if (GameServer::isHostProcess())
        {
            // Injected by sacred.exe when it starts the gameserver (LanClient): only the network patches run here.
            DetourRestoreAfterWith();
            Log::init((gameDir + L"\\SacredBild-server.log").c_str());
            LOG("SacredBild " __DATE__ " " __TIME__ " in gameserver.exe");
            ConfigFile::load(gameDir);
            CrashDump::install(gameDir, L"SacredBild-server-crash");
            GameServer::installHooks();
            return TRUE;
        }
        Log::init((gameDir + L"\\SacredBild.log").c_str());
        LOG("SacredBild " __DATE__ " " __TIME__);
        ConfigFile::load(gameDir);
        CrashDump::install(gameDir, L"SacredBild-crash");
        enableDpiAwareness();

        // `reserved` is null when the game loads ddraw.dll with LoadLibrary, after its entry point.
        const bool wrapped = reserved && wrappedExe();
        g_settingsWindow = reserved && SettingsWindow::wanted();
        if (wrapped || g_settingsWindow)
        {
            g_module = instance;
            g_gameDir = gameDir;
            if (wrapped)
            {
                LOG("Wrapped exe (Steam): SacredBild starts when the game's own code runs");
            }
            if (wrapped ? deferToDecodedCode() : deferToEntryPoint())
            {
                return TRUE;
            }
            LOG("Cannot defer the start to the game's code: starting now, without the settings window");
        }
        if (!startGame(gameDir))
        {
            return FALSE;
        }
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        Profiler::stop();
        LOG("Unloading");
    }
    return TRUE;
}
