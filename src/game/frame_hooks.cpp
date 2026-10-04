#include "game/frame_hooks.h"
#include "game/d3d_stats.h"
#include "game/device_proxy.h"
#include "game/granny_async.h"
#include "game/resolution.h"
#include "game/sacred_addr.h"
#include "game/ui_canvas.h"
#include "config.h"
#include "log.h"
#include "patch.h"
#include "profiler.h"

#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

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

    InitFn g_origInit = nullptr;
    FlipFn g_origFlip = nullptr;
    LockBackFn g_origLockBack = nullptr;
    RenderFn g_origWorldRender = nullptr;
    RenderFn g_origUiRender = nullptr;
    ThreadRunFn g_origRenderThreadRun = nullptr;
    void* volatile g_engine = nullptr;
    void* volatile g_dxDriver = nullptr;

    // Diagnostics: calls along the 3D model path, logged with the probe every 5 s.
    Call2Fn g_origCreatureRender = nullptr;
    Call2Fn g_origObjectRender = nullptr;
    Call5Fn g_origDrawModel = nullptr;
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

    // The in-game frame limit (the game passes 60); menus keep theirs.
    using LimiterFn = void(__cdecl*)(double unused, uint32_t fps);
    LimiterFn g_origLimiter = nullptr;

    void __cdecl hookLimiter(double unused, uint32_t fps)
    {
        if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == Addr::renderLimiterReturn)
        {
            if (g_config.fpsLimit <= 0)
            {
                return;
            }
            fps = (fps & 0xFFFF0000u) | static_cast<uint16_t>(std::min(g_config.fpsLimit, 1000));
        }
        g_origLimiter(unused, fps);
    }

    uint32_t __fastcall hookInit(void* self, void* edx, void* devices, void* deviceDesc, void* mode)
    {
        const uint32_t ok = g_origInit(self, edx, devices, deviceDesc, mode);
        g_dxDriver = self;
        auto& device = member<IDirect3DDevice7*>(self, DxDriver::device);
        LOG("dxDriver7::init -> {} ({}x{} {}bpp, {}, device {})", ok & 0xFF, member<uint16_t>(self, DxDriver::width),
            member<uint16_t>(self, DxDriver::height), member<int>(self, DxDriver::bpp),
            member<int>(self, DxDriver::windowed) == 1 ? "fullscreen" : "windowed", static_cast<void*>(device));
        if ((ok & 0xFF) && device && (g_config.d3dStats || g_config.batch || UiCanvas::enabled()))
        {
            device = DeviceProxy::wrap(device, member<IDirectDraw7*>(self, DxDriver::ddraw));
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
        // Texture memory the texture manager loaded during this frame.
        if (void* textures = *reinterpret_cast<void**>(Addr::g_pTextureManager))
        {
            static uint32_t lastUsed = 0;
            const uint32_t used = member<uint32_t>(textures, TextureManager::usedBytes);
            if (used > lastUsed)
            {
                D3DStats::count(D3DStats::CTextureKB, (used - lastUsed) >> 10);
            }
            lastUsed = used;
        }
        D3DStats::onFrame();
        GrannyAsync::onFrame();
        logGameState();
        // Every 5 s, log where the next frame's 3D draws land (characters are 3D models).
        if (DeviceProxy* proxy = DeviceProxy::instance())
        {
            proxy->onPresent(thread);
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
                    LOG("Textures: {} / {} MB loaded | {}", member<uint32_t>(textures, TextureManager::usedBytes) >> 20,
                        member<uint32_t>(textures, TextureManager::budgetBytes) >> 20, D3DStats::memorySummary());
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
        if (DeviceProxy* proxy = DeviceProxy::instance())
        {
            proxy->syncBatch();     // the CPU must see everything drawn so far
        }
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
        DeviceProxy* proxy = DeviceProxy::instance();
        const int64_t proxyTime = D3DStats::total(D3DStats::TProxy);
        if (proxy && device == proxy)
        {
            proxy->beginBatch();
        }
        g_origWorldRender(self, edx, device);
        if (proxy && device == proxy)
        {
            proxy->endBatch();
        }
        D3DStats::addTime(D3DStats::TWorldProxy, D3DStats::total(D3DStats::TProxy) - proxyTime);
    }

    // True if one of the in-game windows covering the whole 1024x768 screen (save, options, ...) is open.
    bool fullScreenWindowOpen(void* uiManager)
    {
        for (uintptr_t slot = UiManager::firstGameWindow; slot <= UiManager::lastGameWindow; slot += 4)
        {
            void* window = member<void*>(uiManager, slot);
            if (!window || !(member<uint32_t>(window, UiControl::flags) & 1))
            {
                continue;
            }
            const int x = member<int>(window, UiControl::x), y = member<int>(window, UiControl::y);
            const int w = member<int16_t>(window, UiControl::width), h = member<int16_t>(window, UiControl::height);
            if (x <= 0 && y <= 0 && x + w >= 1023 && y + h >= 767)
            {
                return true;
            }
        }
        return false;
    }

    void __fastcall hookUiRender(void* self, void* edx, void* device)
    {
        D3DStats::Scope s{D3DStats::TUi};
        // The original screen showed nothing but a full-screen window; keep the world beside the canvas hidden.
        if (UiCanvas::enabled() && device && fullScreenWindowOpen(self))
        {
            const LONG l = std::lround(UiCanvas::left()), t = std::lround(UiCanvas::top());
            const LONG r = std::lround(UiCanvas::right()), b = std::lround(UiCanvas::bottom());
            const LONG w = Resolution::width(), h = Resolution::height();
            D3DRECT bars[4] = {{0, 0, l, h}, {r, 0, w, h}, {l, 0, r, t}, {l, b, r, h}};
            D3DRECT used[4];
            DWORD count = 0;
            for (const D3DRECT& bar : bars)
            {
                if (bar.x2 > bar.x1 && bar.y2 > bar.y1)
                {
                    used[count++] = bar;
                }
            }
            if (count)
            {
                static_cast<IDirect3DDevice7*>(device)->Clear(count, used, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
            }
        }
        UiCanvas::Scope ui;
        g_origUiRender(self, edx, device);
    }
}

bool FrameHooks::flip()
{
    void* dxDriver = g_dxDriver;
    if (!dxDriver)
    {
        return false;
    }
    hookFlip(dxDriver, nullptr);
    return true;
}

void FrameHooks::install()
{
    Patch::hook(g_origInit, Addr::dxDriver7_init, &hookInit, "dxDriver7::init");
    Patch::hook(g_origFlip, Addr::dxDriver7_flip, &hookFlip, "dxDriver7::flip");
    Patch::hook(g_origLockBack, Addr::dxDriver7_lockBack, &hookLockBack, "dxDriver7::lockBack");
    Patch::hook(g_origWorldRender, Addr::cWorldView0_render, &hookWorldRender, "cWorldView0::render");
    Patch::hook(g_origUiRender, Addr::cUI_Manager_render, &hookUiRender, "cUI_Manager::render");
    Patch::hook(g_origRenderThreadRun, Addr::cEngine_renderThreadRun, &hookRenderThreadRun, "cEngine::renderThreadRun");
    if (g_config.fpsLimit != 60)
    {
        Patch::hook(g_origLimiter, Addr::frameLimiter, &hookLimiter, "frameLimiter");
        LOG("Frame limit in game: {}", g_config.fpsLimit > 0 ? std::to_string(g_config.fpsLimit) : std::string("off"));
    }
    if (g_config.d3dStats)
    {
        Patch::hook(g_origCreatureRender, Addr::cCreature_render, &hookCreatureRender, "cCreature::render");
        Patch::hook(g_origObjectRender, Addr::cObject3D_render, &hookObjectRender, "cObject3D::render");
        Patch::hook(g_origDrawModel, Addr::cObject3D_drawModel, &hookDrawModel, "cObject3D::drawModel");
    }
}
