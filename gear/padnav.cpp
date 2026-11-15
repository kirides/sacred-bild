#include "padnav.h"

#include <windows.h>
#include <mutex>

namespace
{
    using SetStopsFn = void(__cdecl*)(const PadNav::Stop* stops, int count, int flags);
    using TakePadFn = int(__cdecl*)(int* nudge, int* back, int* menu);
    SetStopsFn g_setStops = nullptr;
    TakePadFn g_takePad = nullptr;
    using PostPadFn = void(__cdecl*)(int nudge, int back, int menu);
    using NavStateFn = int(__cdecl*)(int* flags);
    PostPadFn g_postPad = nullptr;
    NavStateFn g_navState = nullptr;
    std::once_flag g_once;

    void find()
    {
        std::call_once(g_once, [] {
            if (HMODULE bild = GetModuleHandleW(L"ddraw.dll"))
            {
                g_setStops = reinterpret_cast<SetStopsFn>(GetProcAddress(bild, "SacredBild_UiSetNavStops"));
                g_takePad = reinterpret_cast<TakePadFn>(GetProcAddress(bild, "SacredBild_UiTakePad"));
                g_postPad = reinterpret_cast<PostPadFn>(GetProcAddress(bild, "SacredBild_UiPostPad"));
                g_navState = reinterpret_cast<NavStateFn>(GetProcAddress(bild, "SacredBild_UiNavState"));
            }
        });
    }
}

bool PadNav::setStops(const std::vector<Stop>& stops, int flags)
{
    find();
    if (!g_setStops)
    {
        return false;
    }
    g_setStops(stops.data(), static_cast<int>(stops.size()), flags);
    return true;
}

PadNav::Events PadNav::take()
{
    find();
    Events e;
    if (g_takePad)
    {
        g_takePad(&e.nudge, &e.back, &e.menu);
    }
    return e;
}

void PadNav::post(int nudge, int back, int menu)
{
    find();
    if (g_postPad)
    {
        g_postPad(nudge, back, menu);
    }
}

int PadNav::navState(int& flags)
{
    find();
    flags = 0;
    return g_navState ? g_navState(&flags) : -2;
}
