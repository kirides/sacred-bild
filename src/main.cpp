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
#include <string>

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

    // The settings window waits for the exe's entry point: DllMain runs under the loader lock, where no window may
    // be shown. The game then starts with the settings it saved.
    HMODULE g_module = nullptr;
    std::wstring g_gameDir;
    int(WINAPI* g_entryPoint)() = nullptr;

    int WINAPI entryPoint()
    {
        if (!SettingsWindow::show(g_module, g_gameDir))
        {
            ExitProcess(0);
        }
        if (!startGame(g_gameDir))
        {
            ExitProcess(1);
        }
        return g_entryPoint();
    }

    bool deferToEntryPoint()
    {
        g_entryPoint = reinterpret_cast<int(WINAPI*)()>(DetourGetEntryPoint(nullptr));
        Patch::begin();
        const bool hooked = Patch::hook(g_entryPoint, entryPoint, "exe entry point");
        return Patch::commit() && hooked;
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
        if (reserved && SettingsWindow::wanted())
        {
            g_module = instance;
            g_gameDir = gameDir;
            if (deferToEntryPoint())
            {
                return TRUE;
            }
            LOG("Settings window: cannot hook the exe's entry point, the game starts without it");
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
