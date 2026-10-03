#include "game/frame_hooks.h"
#include "game/d3d_stats.h"
#include "game/device_proxy.h"
#include "game/resolution.h"
#include "game/sacred_de.h"
#include "game/ui_canvas.h"
#include "config.h"
#include "log.h"
#include "patch.h"
#include "profiler.h"

#include <windows.h>
#include <intrin.h>
#include <cstdint>

namespace
{
    using namespace Sacred;

    template <class T>
    T& member(void* obj, uintptr_t offset)
    {
        return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(obj) + offset);
    }

    // thiscall targets are hooked as fastcall with an unused EDX parameter.
    using InitFn = uint32_t(__fastcall*)(void* self, void* edx, void* devices, void* deviceDesc, void* mode);
    using FlipFn = int(__fastcall*)(void* self, void* edx);
    using LockBackFn = void*(__fastcall*)(void* self, void* edx, void* desc);
    using RenderFn = void(__fastcall*)(void* self, void* edx, void* device);
    using ThreadRunFn = void(__fastcall*)(void* engine);
    using Call2Fn = void(__fastcall*)(void* self, void* edx, void* a, void* b);
    using Call5Fn = void(__fastcall*)(void* self, void* edx, void* a, void* b, void* c, void* d, void* e);

    InitFn g_origInit = reinterpret_cast<InitFn>(Addr::dxDriver7_init);
    FlipFn g_origFlip = reinterpret_cast<FlipFn>(Addr::dxDriver7_flip);
    LockBackFn g_origLockBack = reinterpret_cast<LockBackFn>(Addr::dxDriver7_lockBack);
    RenderFn g_origWorldRender = reinterpret_cast<RenderFn>(Addr::cWorldView0_render);
    RenderFn g_origUiRender = reinterpret_cast<RenderFn>(Addr::cUI_Manager_render);
    ThreadRunFn g_origRenderThreadRun = reinterpret_cast<ThreadRunFn>(Addr::cEngine_renderThreadRun);
    void* volatile g_engine = nullptr;

    // Diagnostics: calls along the 3D model path, logged with the probe every 5 s.
    Call2Fn g_origCreatureRender = reinterpret_cast<Call2Fn>(Addr::cCreature_render);
    Call2Fn g_origObjectRender = reinterpret_cast<Call2Fn>(Addr::cObject3D_render);
    Call5Fn g_origDrawModel = reinterpret_cast<Call5Fn>(Addr::cObject3D_drawModel);
    volatile long g_creatureRenders = 0, g_objectRenders = 0, g_objectAttached = 0, g_modelDraws = 0;

    void __fastcall hookCreatureRender(void* self, void* edx, void* a, void* b)
    {
        _InterlockedIncrement(&g_creatureRenders);
        g_origCreatureRender(self, edx, a, b);
    }

    void __fastcall hookObjectRender(void* self, void* edx, void* a, void* b)
    {
        _InterlockedIncrement(&g_objectRenders);
        g_origObjectRender(self, edx, a, b);
        if (member<uint32_t>(self, 0x14) & 0x4000000)
        {
            _InterlockedIncrement(&g_objectAttached);
        }
    }

    void __fastcall hookDrawModel(void* self, void* edx, void* a, void* b, void* c, void* d, void* e)
    {
        _InterlockedIncrement(&g_modelDraws);
        g_origDrawModel(self, edx, a, b, c, d, e);
    }

    // Logs changes of the engine's fade/loading flags and the UI manager's mode (once per frame, on change).
    void logGameState()
    {
        static uint32_t lastEngine = ~0u, lastUi = ~0u;
        const uint32_t engineFlags = g_engine ? member<uint32_t>(g_engine, Engine::flags) & 0xF0000 : 0;
        void* ui = *reinterpret_cast<void**>(Addr::g_pUiManager);
        const uint32_t uiFlags = ui ? member<uint32_t>(ui, UiManager::flags) & 0x7F : 0;
        if (engineFlags != lastEngine || uiFlags != lastUi)
        {
            lastEngine = engineFlags;
            lastUi = uiFlags;
            LOG("State: engine {}{}{}{} | ui flags {:02x}{}", engineFlags & 0x10000 ? "loading " : "",
                engineFlags & 0x20000 ? "fade-out " : "", engineFlags & 0x40000 ? "fade-in " : "",
                engineFlags & 0x80000 ? "black " : "", uiFlags, uiFlags & 0x10 ? " cinematic" : "");
        }
    }

    void __fastcall hookRenderThreadRun(void* engine)
    {
        g_engine = engine;
        g_origRenderThreadRun(engine);
    }

    uint32_t __fastcall hookInit(void* self, void* edx, void* devices, void* deviceDesc, void* mode)
    {
        const uint32_t ok = g_origInit(self, edx, devices, deviceDesc, mode);
        auto& device = member<IDirect3DDevice7*>(self, DxDriver::device);
        LOG("dxDriver7::init -> {} ({}x{} {}bpp, {}, device {})", ok & 0xFF, member<uint16_t>(self, DxDriver::width),
            member<uint16_t>(self, DxDriver::height), member<int>(self, DxDriver::bpp),
            member<int>(self, DxDriver::windowed) == 1 ? "fullscreen" : "windowed", static_cast<void*>(device));
        if ((ok & 0xFF) && device && (g_config.d3dStats || UiCanvas::enabled()))
        {
            device = DeviceProxy::wrap(device);
        }
        return ok;
    }

    int __fastcall hookFlip(void* self, void* edx)
    {
        // Menus flip from the UI thread, the game world from the engine render thread.
        static DWORD lastThread = 0;
        const DWORD thread = GetCurrentThreadId();
        if (thread != lastThread)
        {
            lastThread = thread;
            LOG("Frames now presented by thread {}", thread);
            D3DStats::setRenderThread(thread);
            if (g_config.profiler)
            {
                Profiler::retarget(thread);
            }
        }
        int hr;
        {
            D3DStats::Scope s{D3DStats::TFlip};
            hr = g_origFlip(self, edx);
        }
        D3DStats::onFrame();
        logGameState();
        // Every 5 s, log where the next frame's 3D draws land (characters are 3D models).
        if (DeviceProxy* proxy = DeviceProxy::instance())
        {
            static DWORD nextProbe = GetTickCount() + 5000;
            static bool probing = false;
            if (probing)
            {
                proxy->setProbe(false);
                probing = false;
            }
            else if (static_cast<int>(GetTickCount() - nextProbe) >= 0)
            {
                if (void* textures = *reinterpret_cast<void**>(Addr::g_pTextureManager))
                {
                    LOG("Textures: {} / {} MB loaded", member<uint32_t>(textures, TextureManager::usedBytes) >> 20,
                        member<uint32_t>(textures, TextureManager::budgetBytes) >> 20);
                }
                LOG("Probe3D: last 5 s: creature renders {}, object renders {} (model attached {}), model draws {}",
                    _InterlockedExchange(&g_creatureRenders, 0), _InterlockedExchange(&g_objectRenders, 0),
                    _InterlockedExchange(&g_objectAttached, 0), _InterlockedExchange(&g_modelDraws, 0));
                proxy->setProbe(true);
                probing = true;
                nextProbe = GetTickCount() + 5000;
            }
        }
        return hr;
    }

    void* __fastcall hookLockBack(void* self, void* edx, void* desc)
    {
        D3DStats::count(D3DStats::CLockBack);
        D3DStats::Scope s{D3DStats::TLockBack};
        void* bits = g_origLockBack(self, edx, desc);
        // The savegame thumbnail copies desc.dwHeight rows into a 1024x768 buffer: hand it the centered 1024x768.
        if (bits && Resolution::active() && reinterpret_cast<uintptr_t>(_ReturnAddress()) == Addr::captureLockBackReturn)
        {
            auto* d = static_cast<DDSURFACEDESC2*>(desc);
            const int bytesPerPixel = static_cast<int>(d->ddpfPixelFormat.dwRGBBitCount / 8);
            d->lpSurface = static_cast<uint8_t*>(d->lpSurface) + Resolution::centerY() * d->lPitch +
                Resolution::centerX() * bytesPerPixel;
            d->dwWidth = 1024;
            d->dwHeight = 768;
            bits = d->lpSurface;
        }
        return bits;
    }

    void __fastcall hookWorldRender(void* self, void* edx, void* device)
    {
        D3DStats::Scope s{D3DStats::TWorld};
        // In game only Z is cleared: the ground covers 1024x768, but anything it leaves uncovered at larger
        // sizes (map edges, extreme zoom) would show old frames.
        if (Resolution::active() && device)
        {
            Resolution::refresh();
            static_cast<IDirect3DDevice7*>(device)->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
        }
        g_origWorldRender(self, edx, device);
    }

    void __fastcall hookUiRender(void* self, void* edx, void* device)
    {
        D3DStats::Scope s{D3DStats::TUi};
        UiCanvas::Scope ui;
        g_origUiRender(self, edx, device);
    }
}

void FrameHooks::install()
{
    Patch::hook(g_origInit, &hookInit, "dxDriver7::init");
    Patch::hook(g_origFlip, &hookFlip, "dxDriver7::flip");
    Patch::hook(g_origLockBack, &hookLockBack, "dxDriver7::lockBack");
    Patch::hook(g_origWorldRender, &hookWorldRender, "cWorldView0::render");
    Patch::hook(g_origUiRender, &hookUiRender, "cUI_Manager::render");
    Patch::hook(g_origRenderThreadRun, &hookRenderThreadRun, "cEngine::renderThreadRun");
    if (g_config.d3dStats)
    {
        Patch::hook(g_origCreatureRender, &hookCreatureRender, "cCreature::render");
        Patch::hook(g_origObjectRender, &hookObjectRender, "cObject3D::render");
        Patch::hook(g_origDrawModel, &hookDrawModel, "cObject3D::drawModel");
    }
}
