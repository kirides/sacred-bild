#include "frame_hooks.h"
#include "controller/controller.h"
#include "diag/d3d_stats.h"
#include "device_proxy.h"
#include "display/focus.h"
#include "gpu_skin.h"
#include "ground_mesh.h"
#include "granny_async.h"
#include "granny_parallel.h"
#include "display/resolution.h"
#include "engine/sacred_addr.h"
#include "ui/ui_canvas.h"
#include "ui/ui_state.h"
#include "ui/ui_windows.h"
#include "settings/config/ddraw.h"
#include "settings/config/debug.h"
#include "settings/config/display.h"
#include "settings/config/render.h"
#include "ddraw9/smaa.h"
#include "ddrawrhi/device.h"
#include "ddrawrhi/smaa.h"
#include "ddrawrhi/world.h"
#include "log.h"
#include "patch.h"
#include "diag/profiler.h"
#include "sacred/engine.h"
#include "sacred/render.h"
#include "sacred/ui.h"

#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace
{
    using namespace Sacred;

    // Hooked without EDX: it is a plain fastcall (engine) in the exe.
    using ThreadRunFn = void(__fastcall*)(cEngine* engine);

    decltype(Addr::dxDriver7_init)::Ptr g_origInit = nullptr;
    decltype(Addr::dxDriver7_flip)::Ptr g_origFlip = nullptr;
    decltype(Addr::dxDriver7_lockBack)::Ptr g_origLockBack = nullptr;
    decltype(Addr::cWorldView0_render)::Ptr g_origWorldRender = nullptr;
    decltype(Addr::cUI_Manager_render)::Ptr g_origUiRender = nullptr;
    decltype(Addr::weatherRender)::Ptr g_origWeatherRender = nullptr;
    ThreadRunFn g_origRenderThreadRun = nullptr;
    cEngine* volatile g_engine = nullptr;
    dxDriver7* volatile g_dxDriver = nullptr;

    // Logs changes of the engine's fade/loading flags and the UI manager's mode (once per frame, on change).
    void logGameState()
    {
        static uint32_t lastEngine = ~0u, lastUi = ~0u;
        cEngine* engine = g_engine;
        const uint32_t engineFlags = engine ? engine->flags & 0xF0000 : 0;
        const cUI_Manager* ui = cUI_Manager::instance();
        const uint32_t uiFlags = ui ? ui->flags & 0x7F : 0;
        if (engineFlags != lastEngine || uiFlags != lastUi)
        {
            lastEngine = engineFlags;
            lastUi = uiFlags;
            LOG("State: engine {}{}{}{} | ui flags {:02x}{}", engineFlags & cEngine::loading ? "loading " : "",
                engineFlags & cEngine::fadeOut ? "fade-out " : "", engineFlags & cEngine::fadeIn ? "fade-in " : "",
                engineFlags & cEngine::black ? "black " : "", uiFlags,
                uiFlags & cUI_Manager::cinematic ? " cinematic" : "");
        }
    }

    // The engine is only valid while its render thread runs: leaving a game for the main menu ends the thread and the
    // game then frees the engine, while the menus go on presenting frames from the main thread (logGameState,
    // Controller and the others ask FrameHooks::engine() every frame). A pointer left behind read freed memory there.
    void __fastcall hookRenderThreadRun(cEngine* engine)
    {
        g_engine = engine;
        g_origRenderThreadRun(engine);
        g_engine = nullptr;
    }

    int64_t qpcNow()
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return now.QuadPart;
    }

    // The timer the frame waits sleep on. A high-resolution one (Windows 10 1803+) fires within a fraction of a
    // millisecond whatever the system timer resolution; without it, the system timer is set to its finest interval
    // (usually 0.5 ms) for a plain one.
    HANDLE createFrameTimer()
    {
        if (HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS))
        {
            return timer;
        }
        static const ULONG resolution = [] {
            using QueryFn = LONG(NTAPI*)(PULONG minimum, PULONG maximum, PULONG current);
            using SetFn = LONG(NTAPI*)(ULONG desired, BOOLEAN set, PULONG current);
            const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
            const auto query = reinterpret_cast<QueryFn>(GetProcAddress(ntdll, "NtQueryTimerResolution"));
            const auto set = reinterpret_cast<SetFn>(GetProcAddress(ntdll, "NtSetTimerResolution"));
            ULONG minimum = 0, maximum = 0, current = 0;
            return query && set && query(&minimum, &maximum, &current) == 0 && set(maximum, TRUE, &current) == 0
                ? current : 0ul;
        }();
        LOG("Frame limiter: no high-resolution timer, system timer {}",
            resolution ? std::to_string(resolution / 10) + " us" : std::string("unchanged"));
        return CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }

    // Waits for the next frame at `fps`, in place of the game's limiter, which spun on Sleep(0) for the whole wait and
    // kept a core busy. Sleeps on the timer in 100 ns units, without a final spin: waking a little off costs less
    // than a busy core. Each frame is due one period after the previous one was due, so the next wait makes up for
    // that and the rate averages out exactly; a frame more than a period late restarts the cadence from now instead
    // of catching up. Per thread: the menus and the game pace their own loops.
    void paceFrame(int fps)
    {
        static const int64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
        thread_local const HANDLE timer = createFrameTimer();
        thread_local int64_t due = 0;
        const int64_t period = freq / std::clamp(fps, 1, 1000);
        const int64_t now = qpcNow();
        if (due == 0 || now - due > period)
        {
            due = now;
        }
        due += period;
        const int64_t wait = due - now;
        LARGE_INTEGER relative;
        relative.QuadPart = -(wait * 10000000 / freq);
        if (timer && SetWaitableTimer(timer, &relative, 0, nullptr, nullptr, FALSE))
        {
            WaitForSingleObject(timer, INFINITE);
        }
        else
        {
            Sleep(static_cast<DWORD>(wait * 1000 / freq));
        }
    }

    // The game's frame limiter, called by the menus and the game once per frame, replaced by paceFrame: the in-game
    // limit (the game passes 60; menus keep theirs), and [Display] FpsLimitInactive in the background.
    using LimiterFn = void(__cdecl*)(double unused, uint32_t fps);
    LimiterFn g_origLimiter = nullptr;

    // Set by the limiter hook, cleared by each flip: tells hookFlip whether the frame was paced already.
    thread_local bool t_limiterCalled = false;

    // The limit for this frame: [Display] FpsLimitInactive in the background (the lower of it and the menu limit in
    // the menus), [Display] FpsLimitMenu in the menus, FpsLimit in game, `passed` (the menus' own) otherwise.
    int frameLimit(int passed, bool inGameRender)
    {
        const bool menu = g_engine == nullptr;
        int limit = inGameRender ? Config::display.fpsLimit : passed;
        if (menu && Config::display.fpsLimitMenu > 0)
        {
            limit = Config::display.fpsLimitMenu;
        }
        if (Config::display.fpsLimitInactive > 0 && !Focus::foreground() &&
            (limit <= 0 || Config::display.fpsLimitInactive < limit))
        {
            limit = Config::display.fpsLimitInactive;
        }
        return limit;
    }

    void __cdecl hookLimiter(double, uint32_t fps)
    {
        t_limiterCalled = true;
        const bool inGameRender = g_engine != nullptr &&
            reinterpret_cast<uintptr_t>(_ReturnAddress()) == Addr::renderLimiterReturn;
        const int limit = frameLimit(static_cast<uint16_t>(fps), inGameRender);   // the game reads the low word only
        if (limit > 0)
        {
            paceFrame(limit);
        }
    }

    uint32_t __fastcall hookInit(dxDriver7* self, void* edx, void* devices, void* deviceDesc, void* mode)
    {
        const uint32_t ok = g_origInit(self, edx, devices, deviceDesc, mode);
        g_dxDriver = self;
        IDirect3DDevice7*& device = self->device;
        LOG("dxDriver7::init -> {} ({}x{} {}bpp, {}, device {})", ok & 0xFF, self->width, self->height, self->bpp,
            self->windowed == 1 ? "fullscreen" : "windowed", static_cast<void*>(device));
        if ((ok & 0xFF) && device && (Config::debug.d3dStats || Config::render.batch || UiCanvas::enabled()))
        {
            device = DeviceProxy::wrap(device, self->ddraw);
        }
        return ok;
    }

    int __fastcall hookFlip(dxDriver7* self, void* edx)
    {
        // Menus flip from the UI thread, the game world from the engine render thread.
        static DWORD lastThread = 0;
        const DWORD thread = GetCurrentThreadId();
        if (thread != lastThread)
        {
            lastThread = thread;
            LOG("Frames now presented by thread {}", thread);
            D3DStats::setRenderThread(thread);
            if (Config::debug.profiler)
            {
                Profiler::retarget(thread);
            }
        }
        // The menus' own frames that bypassed the game's limiter (a loop that waits some other way) are paced here,
        // so the menu and background limits hold there too.
        if (g_engine == nullptr && !t_limiterCalled)
        {
            if (const int limit = frameLimit(0, false); limit > 0)
            {
                paceFrame(limit);
            }
        }
        t_limiterCalled = false;
        D3DStats::mark(D3DStats::TimerCount);
        Resolution::beforeFlip(self);
        int hr;
        {
            D3DStats::Scope s{D3DStats::TFlip};
            hr = g_origFlip(self, edx);
        }
        D3DStats::mark(D3DStats::TOwnAfterFlip);
        // Texture memory the texture manager loaded during this frame.
        if (cTextureManager* textures = cTextureManager::instance())
        {
            static uint32_t lastUsed = 0;
            const uint32_t used = textures->usedBytes;
            if (used > lastUsed)
            {
                D3DStats::count(D3DStats::CTextureKB, (used - lastUsed) >> 10);
            }
            lastUsed = used;
        }
        if (Config::debug.d3dStats)
        {
            D3DStats::onFrame();
        }
        GrannyAsync::onFrame();
        GrannyParallel::onFrame();
        GpuSkin::onFrame();
        Focus::onFrame();
        Controller::onFrame();
        logGameState();
        if (DeviceProxy* proxy = DeviceProxy::instance())
        {
            proxy->onPresent(thread);
        }
        // Every 5 s, the texture memory in use.
        static DWORD nextTextureLog = GetTickCount() + 5000;
        if (Config::debug.d3dStats && static_cast<int>(GetTickCount() - nextTextureLog) >= 0)
        {
            nextTextureLog = GetTickCount() + 5000;
            if (cTextureManager* textures = cTextureManager::instance())
            {
                LOG("Textures: {} / {} MB loaded | {}", textures->usedBytes >> 20, textures->budgetBytes >> 20,
                    D3DStats::memorySummary());
            }
        }
        D3DStats::mark(D3DStats::TGameBeforeWorld);
        return hr;
    }

    void* __fastcall hookLockBack(dxDriver7* self, void* edx, void* desc)
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

    // The weather manager keeps the screen size it saw when it was constructed (`+0x14` width, `+0x18` height, floats)
    // and spreads its rain drops over it: built before the display mode reached its final size (a window that was
    // resized afterwards, Wine/Lutris) the rain covered only part of the screen. Keep it at the real size.
    void __fastcall hookWeatherRender(void* self, void* edx, void* device, uint32_t a, uint32_t b, uint32_t c)
    {
        if (dxDriver7* driver = g_dxDriver)
        {
            auto* size = static_cast<float*>(self);
            size[0x14 / 4] = static_cast<float>(driver->width);
            size[0x18 / 4] = static_cast<float>(driver->height);
        }
        // [Debug] SACREDBILD_FORCE_WEATHER=flags,drops (environment, tests): the weather's flags (1 rain, 2 fog, 0x10
        // sparks) and rain drop count forced on.
        static const std::pair<uint32_t, int32_t> force = [] {
            char text[64] = {};
            uint32_t flags = 0;
            int32_t drops = 0;
            if (GetEnvironmentVariableA("SACREDBILD_FORCE_WEATHER", text, sizeof(text)) && sscanf_s(text, "%x,%d", &flags, &drops) >= 1)
            {
                LOG("Weather forced: flags {:#x}, {} drops", flags, drops);
            }
            return std::make_pair(flags, drops);
        }();
        if (force.first)
        {
            static_cast<uint32_t*>(self)[0x10 / 4] |= force.first;
            if (force.second > 0)
            {
                static_cast<int32_t*>(self)[0x1C / 4] = force.second;
            }
        }
        DeviceProxy* proxy = DeviceProxy::instance();
        const bool lines = proxy && device == proxy;
        if (lines)
        {
            proxy->beginLineBatch();
        }
        g_origWeatherRender(self, edx, device, a, b, c);
        if (lines)
        {
            proxy->endLineBatch();
        }
    }

    bool g_worldDrawn = false;      // the world view was drawn since the last UI render (SMAA)

    void __fastcall hookWorldRender(cWorldView* self, void* edx, IDirect3DDevice7* device)
    {
        D3DStats::mark(D3DStats::TimerCount);
        D3DStats::Scope s{D3DStats::TWorld};
        D3DStats::PassScope pass{D3DStats::PWorld};
        // In game only Z is cleared: the ground covers 1024x768, but anything it leaves uncovered at larger
        // sizes (map edges, extreme zoom) would show old frames.
        if (Resolution::active() && device)
        {
            Resolution::refresh();
            device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
        }
        DeviceProxy* proxy = DeviceProxy::instance();
        const int64_t proxyTime = D3DStats::total(D3DStats::TProxy);
        const bool ownWorld = Config::ddraw.nvrhi && Config::render.worldScale > 1.0f && proxy && device == proxy;
        if (ownWorld)
        {
            proxy->syncBatch();
            DDrawRhi::World::begin(proxy->real(), Config::render.worldScale);
        }
        if (proxy && device == proxy)
        {
            proxy->beginBatch();
        }
        // Bilinear sampling bleeds across the edges of the sprite pieces cut from their sheets (a grid of thin lines);
        // the game's FSAA_FILTER option pulls their texture coordinates in against that, so it counts as on then.
        if (Config::render.textureFilter == 2 || (Config::render.textureFilter == 1 && Config::ddraw.d3d9))
        {
            if (Addr::g_gameSettings.address)
            {
                (*Addr::g_gameSettings)[2] = 1;
            }
        }
        GroundMesh::beginFrame(self, device);
        DDrawRhi::Device* rhiDevice = proxy && device == proxy && Config::ddraw.nvrhi ? DDrawRhi::Device::from(proxy->real()) : nullptr;
        if (rhiDevice)
        {
            rhiDevice->setWorldView(true);
        }
        Profiler::worldDrawn();
        g_origWorldRender(self, edx, device);
        if (rhiDevice)
        {
            rhiDevice->setWorldView(false);
        }
        if (proxy && device == proxy)
        {
            proxy->endBatch();
            g_worldDrawn = true;
        }
        if (ownWorld)
        {
            proxy->syncBatch();
            DDrawRhi::World::end(proxy->real());
        }
        D3DStats::addTime(D3DStats::TWorldProxy, D3DStats::total(D3DStats::TProxy) - proxyTime);
        D3DStats::mark(D3DStats::TGameAfterWorld);
    }

    void __fastcall hookUiRender(cUI_Manager* self, void* edx, IDirect3DDevice7* device)
    {
        D3DStats::mark(D3DStats::TimerCount);
        D3DStats::Scope s{D3DStats::TUi};
        UiWindows::sync();
        UiCanvas::updateCursorFilter();
        const bool inWorld = g_worldDrawn;
        // The world view antialiased before the interface goes on top of it.
        if (g_worldDrawn)
        {
            g_worldDrawn = false;
            DeviceProxy* proxy = DeviceProxy::instance();
            if (Config::render.smaa > 0 && proxy && device == proxy)
            {
                if (Config::ddraw.nvrhi)
                {
                    DDrawRhi::Smaa::apply(proxy->real(), Config::render.smaa - 1);
                }
                else
                {
                    DDraw9::Smaa::apply(proxy->real(), static_cast<DDraw9::Smaa::Preset>(Config::render.smaa - 1));
                }
            }
        }
        // The manager draws a window alone (savegame, options, character, world map): the original screen showed
        // nothing else, so keep the world beside the canvas hidden.
        if (UiCanvas::enabled() && device && UiState::exclusiveWindow())
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
                device->Clear(count, used, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
            }
        }
        {
            UiCanvas::Scope ui;
            g_origUiRender(self, edx, device);
        }
        UiWindows::publishDrawn();
        // The interface draws models too (the hero's portrait, 0x6E3D10): the next advance starts after it.
        if (inWorld)
        {
            GrannyAsync::kickAhead();
        }
        D3DStats::mark(D3DStats::TGameAfterUi);
    }
}

bool FrameHooks::flip()
{
    dxDriver7* dxDriver = g_dxDriver;
    if (!dxDriver)
    {
        return false;
    }
    hookFlip(dxDriver, nullptr);
    return true;
}

dxDriver7* FrameHooks::dxDriver()
{
    return g_dxDriver;
}

cEngine* FrameHooks::engine()
{
    return g_engine;
}

void FrameHooks::install()
{
    Patch::hook(g_origInit, Addr::dxDriver7_init, &hookInit, "dxDriver7::init");
    Patch::hook(g_origFlip, Addr::dxDriver7_flip, &hookFlip, "dxDriver7::flip");
    Patch::hook(g_origLockBack, Addr::dxDriver7_lockBack, &hookLockBack, "dxDriver7::lockBack");
    Patch::hook(g_origWorldRender, Addr::cWorldView0_render, &hookWorldRender, "cWorldView0::render");
    Patch::hook(g_origUiRender, Addr::cUI_Manager_render, &hookUiRender, "cUI_Manager::render");
    Patch::hook(g_origWeatherRender, Addr::weatherRender, &hookWeatherRender, "weather::render");
    Patch::hook(g_origRenderThreadRun, Addr::cEngine_renderThreadRun, &hookRenderThreadRun, "cEngine::renderThreadRun");
    Patch::hook(g_origLimiter, Addr::frameLimiter, &hookLimiter, "frameLimiter");
    // The menu loop sleeps 20 ms every frame before drawing (jz over `push 14h; call Sleep` -> jmp), which at the
    // default timer resolution is ~31 ms: the menus ran at ~30 fps whatever [Display] FpsLimitMenu said. paceFrame
    // paces them now.
    if (Patch::verify(Addr::menuLoopSleepJump, {0x74, 0x04, 0x6A, 0x14}) &&
        Patch::value<uint8_t>(Addr::menuLoopSleepJump, 0xEB))
    {
        LOG("Menu loop: 20 ms sleep removed");
    }
    LOG("Frame limit in game: {}, in the background: {}",
        Config::display.fpsLimit > 0 ? std::to_string(Config::display.fpsLimit) : std::string("off"),
        Config::display.fpsLimitInactive > 0 ? std::to_string(Config::display.fpsLimitInactive) : std::string("off"));
}
