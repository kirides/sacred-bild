#include "system_ddraw.h"
#include "log.h"
#include "patch.h"

#include <objbase.h>
#include <ddraw.h>
#include <d3d.h>
#include <algorithm>
#include <cstdint>

// Windows' Direct3D 7 runtime (d3dim700.dll) refuses render targets wider or taller than 2048 pixels:
// IDirect3D7::CreateDevice fails with DDERR_INVALIDOBJECT, IDirect3DDevice7::SetRenderTarget with DDERR_INVALIDPARAMS.
// Only those two calls check it, through GetSurfaceDesc of the surface's IDirectDrawSurface (CreateDevice) and
// IDirectDrawSurface7 (SetRenderTarget) interfaces; viewports, clears and clipped draws cover the whole surface
// afterwards. So they are retried with GetSurfaceDesc reporting at most 2048 x 2048, as DDrawCompat does.
namespace
{
    constexpr DWORD kMaxSize = 2048;

    // Vtable slots.
    constexpr int kCreateDevice = 4;        // IDirect3D7
    constexpr int kSetRenderTarget = 8;     // IDirect3DDevice7
    constexpr int kGetSurfaceDesc = 22;     // IDirectDrawSurface, IDirectDrawSurface7

    using CreateExFn = HRESULT(WINAPI*)(GUID*, LPVOID*, REFIID, IUnknown*);
    using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3D7*, REFCLSID, LPDIRECTDRAWSURFACE7, LPDIRECT3DDEVICE7*);
    using SetRenderTargetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice7*, LPDIRECTDRAWSURFACE7, DWORD);
    template <class Surface, class Desc>
    using GetSurfaceDescFn = HRESULT(STDMETHODCALLTYPE*)(Surface*, Desc*);

    CreateExFn g_origCreateEx = nullptr;
    CreateDeviceFn g_origCreateDevice = nullptr;
    SetRenderTargetFn g_origSetRenderTarget = nullptr;
    template <class Surface, class Desc>
    GetSurfaceDescFn<Surface, Desc> g_origGetSurfaceDesc = nullptr;
    volatile DWORD g_clampThread = 0;   // GetSurfaceDesc reports at most kMaxSize on this thread

    // Points vtable slot `index` of `object` at `hook`. A second vtable with a different implementation in that slot
    // is left alone: `original` serves one.
    template <class Fn>
    void hookSlot(void* object, int index, Fn hook, Fn& original)
    {
        void** slot = &(*static_cast<void***>(object))[index];
        void* target = reinterpret_cast<void*>(hook);
        if (*slot == target || (original && reinterpret_cast<void*>(original) != *slot))
        {
            return;
        }
        original = reinterpret_cast<Fn>(*slot);
        Patch::write(reinterpret_cast<uintptr_t>(slot), &target, sizeof(target));
    }

    template <class Surface, class Desc>
    HRESULT STDMETHODCALLTYPE getSurfaceDesc(Surface* self, Desc* desc)
    {
        const HRESULT hr = g_origGetSurfaceDesc<Surface, Desc>(self, desc);
        if (SUCCEEDED(hr) && g_clampThread == GetCurrentThreadId())
        {
            desc->dwWidth = std::min(desc->dwWidth, kMaxSize);
            desc->dwHeight = std::min(desc->dwHeight, kMaxSize);
        }
        return hr;
    }

    bool tooLarge(IDirectDrawSurface7* surface, DDSURFACEDESC2& desc)
    {
        desc = {};
        desc.dwSize = sizeof(desc);
        return surface && SUCCEEDED(surface->GetSurfaceDesc(&desc)) && (desc.dwWidth > kMaxSize || desc.dwHeight > kMaxSize);
    }

    template <class Surface, class Desc>
    void hookGetSurfaceDesc(IDirectDrawSurface7* surface, REFIID iid)
    {
        Surface* s = nullptr;
        if (SUCCEEDED(surface->QueryInterface(iid, reinterpret_cast<void**>(&s))))
        {
            hookSlot(s, kGetSurfaceDesc, &getSurfaceDesc<Surface, Desc>, g_origGetSurfaceDesc<Surface, Desc>);
            s->Release();
        }
    }

    // Repeats a call that failed on `surface`'s size, with its size clamped for the runtime.
    template <class Call>
    HRESULT retryClamped(IDirectDrawSurface7* surface, Call call)
    {
        hookGetSurfaceDesc<IDirectDrawSurface, DDSURFACEDESC>(surface, IID_IDirectDrawSurface);
        hookGetSurfaceDesc<IDirectDrawSurface7, DDSURFACEDESC2>(surface, IID_IDirectDrawSurface7);
        g_clampThread = GetCurrentThreadId();
        const HRESULT hr = call();
        g_clampThread = 0;
        return hr;
    }

    HRESULT STDMETHODCALLTYPE setRenderTarget(IDirect3DDevice7* self, LPDIRECTDRAWSURFACE7 target, DWORD flags)
    {
        HRESULT hr = g_origSetRenderTarget(self, target, flags);
        DDSURFACEDESC2 desc;
        if (hr == DDERR_INVALIDPARAMS && tooLarge(target, desc))
        {
            hr = retryClamped(target, [&] { return g_origSetRenderTarget(self, target, flags); });
            static bool logged = false;
            if (!logged)
            {
                logged = true;
                LOG("System ddraw: SetRenderTarget {}x{} over Direct3D 7's 2048 px limit, retried -> {:08x}",
                    desc.dwWidth, desc.dwHeight, static_cast<uint32_t>(hr));
            }
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE createDevice(IDirect3D7* self, REFCLSID type, LPDIRECTDRAWSURFACE7 target, LPDIRECT3DDEVICE7* out)
    {
        HRESULT hr = g_origCreateDevice(self, type, target, out);
        DDSURFACEDESC2 desc;
        if (hr == DDERR_INVALIDOBJECT && tooLarge(target, desc))
        {
            hr = retryClamped(target, [&] { return g_origCreateDevice(self, type, target, out); });
            LOG("System ddraw: CreateDevice on {}x{} over Direct3D 7's 2048 px limit, retried -> {:08x}",
                desc.dwWidth, desc.dwHeight, static_cast<uint32_t>(hr));
        }
        if (SUCCEEDED(hr) && out && *out)
        {
            hookSlot(*out, kSetRenderTarget, &setRenderTarget, g_origSetRenderTarget);
        }
        return hr;
    }

    HRESULT WINAPI directDrawCreateEx(GUID* guid, LPVOID* out, REFIID iid, IUnknown* outer)
    {
        const HRESULT hr = g_origCreateEx(guid, out, iid, outer);
        if (SUCCEEDED(hr) && out && *out && iid == IID_IDirectDraw7)
        {
            IDirect3D7* d3d = nullptr;
            if (SUCCEEDED(static_cast<IDirectDraw7*>(*out)->QueryInterface(IID_IDirect3D7, reinterpret_cast<void**>(&d3d))))
            {
                hookSlot(d3d, kCreateDevice, &createDevice, g_origCreateDevice);
                d3d->Release();
            }
        }
        return hr;
    }
}

FARPROC SystemDdraw::wrap(FARPROC createEx)
{
    if (!createEx)
    {
        return nullptr;
    }
    g_origCreateEx = reinterpret_cast<CreateExFn>(createEx);
    return reinterpret_cast<FARPROC>(&directDrawCreateEx);
}
