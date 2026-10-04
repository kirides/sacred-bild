#include "proxy.h"
#include "config.h"
#include "log.h"
#include "ddraw9/backend.h"

#include <windows.h>
#include <cstring>

#define DDRAW_PROCS(X) \
    X(AcquireDDThreadLock) \
    X(CompleteCreateSysmemSurface) \
    X(D3DParseUnknownCommand) \
    X(DDGetAttachedSurfaceLcl) \
    X(DDInternalLock) \
    X(DDInternalUnlock) \
    X(DSoundHelp) \
    X(DirectDrawCreate) \
    X(DirectDrawCreateClipper) \
    X(DirectDrawCreateEx) \
    X(DirectDrawEnumerateA) \
    X(DirectDrawEnumerateExA) \
    X(DirectDrawEnumerateExW) \
    X(DirectDrawEnumerateW) \
    X(DllCanUnloadNow) \
    X(DllGetClassObject) \
    X(GetDDSurfaceLocal) \
    X(GetOLEThunkData) \
    X(GetSurfaceFromDC) \
    X(RegisterSpecialCase) \
    X(ReleaseDDThreadLock) \
    X(SetAppCompatData)

namespace Proxy
{
    struct Procs
    {
#define PROC_MEMBER(name) FARPROC name;
        DDRAW_PROCS(PROC_MEMBER)
#undef PROC_MEMBER
    };
    Procs g_procs = {};
}

// Exported (via exports.def) as the real ddraw names; each jumps to the chained implementation.
#define PROC_STUB(name) \
    extern "C" __declspec(naked) void Stub_##name() \
    { \
        __asm jmp Proxy::g_procs.name \
    }
DDRAW_PROCS(PROC_STUB)
#undef PROC_STUB

namespace
{
    std::wstring systemDdrawPath()
    {
        wchar_t dir[MAX_PATH] = {};
        GetSystemDirectoryW(dir, MAX_PATH);
        return std::wstring(dir) + L"\\ddraw.dll";
    }

    std::string narrow(const std::wstring& s)
    {
        std::string out;
        for (wchar_t c : s)
        {
            out += c < 0x80 ? static_cast<char>(c) : '?';
        }
        return out;
    }
}

bool Proxy::init(const std::wstring& gameDir)
{
    HMODULE chain = nullptr;
    if (!g_config.ddrawD3D9 && !g_config.ddrawChain.empty())
    {
        const std::wstring path = gameDir + L"\\" + g_config.ddrawChain;
        chain = LoadLibraryW(path.c_str());
        LOG("Chain ddraw {}: {}", narrow(path), chain ? "loaded" : "not found");
    }

    const std::wstring sysPath = systemDdrawPath();
    HMODULE system = LoadLibraryW(sysPath.c_str());
    if (!system)
    {
        LOG("ERROR: cannot load {}", narrow(sysPath));
        return false;
    }
    if (!chain)
    {
        chain = system;
        if (!g_config.ddrawD3D9)
        {
            LOG("Using system ddraw: {}", narrow(sysPath));
        }
    }

#define RESOLVE(name) \
    g_procs.name = GetProcAddress(chain, #name); \
    if (!g_procs.name) g_procs.name = GetProcAddress(system, #name); \
    if (!g_procs.name) LOG("WARNING: ddraw export {} not found", #name);
    DDRAW_PROCS(RESOLVE)
#undef RESOLVE

    if (g_config.ddrawD3D9)
    {
        // DirectDraw objects come from SacredBild's backend; the system ddraw.dll keeps the remaining exports and
        // takes over if Direct3D 9Ex turns out to be unavailable.
        DDraw9::setFallback(system);
        for (const DDraw9::Export& entry : DDraw9::exports())
        {
#define OVERRIDE(p) if (std::strcmp(entry.name, #p) == 0) g_procs.p = entry.proc;
            DDRAW_PROCS(OVERRIDE)
#undef OVERRIDE
        }
        LOG("ddraw: SacredBild's Direct3D 9 backend");
    }
    return true;
}
