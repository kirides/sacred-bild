#include "config.h"
#include "log.h"
#include "profiler.h"
#include "proxy.h"
#include "game/build.h"

#include <windows.h>
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
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
        const std::wstring gameDir = exeDirectory();
        Log::init((gameDir + L"\\SacredBild.log").c_str());
        LOG("SacredBild " __DATE__ " " __TIME__);
        ConfigFile::load(gameDir);

        if (!Proxy::init(gameDir))
        {
            return FALSE;
        }
        enableDpiAwareness();
        if (Sacred::isSupportedBuild())
        {
            Sacred::installHooks();
        }
        else
        {
            LOG("Unsupported sacred.exe build: game hooks disabled, ddraw is passed through");
        }
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        Profiler::stop();
        LOG("Unloading");
    }
    return TRUE;
}
