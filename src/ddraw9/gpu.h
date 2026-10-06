#pragma once
#include "ddraw9/d3d9_api.h"
#include "spin_lock.h"

// The Direct3D 9Ex device behind the DirectDraw/Direct3D 7 emulation. There is one per process: DirectDraw
// objects, surfaces and Direct3D 7 devices all share it.
namespace DDraw9::Gpu
{
    // Loads d3d9.dll and creates the Direct3D 9Ex object (once); false if that is not possible.
    bool available();
    d9::IDirect3D9Ex* d3d();
    UINT adapter();
    const d9::D3DCAPS9& caps();

    // The window frames are presented to (DirectDraw's cooperative level window), and whether Direct3D must
    // leave the FPU control word alone (DDSCL_FPUSETUP / DDSCL_FPUPRESERVE).
    void setWindow(HWND window, bool fpuPreserve);

    // The device, created on first use with a back buffer of width x height (0: the window's client size).
    d9::IDirect3DDevice9Ex* device(UINT width = 0, UINT height = 0);
    d9::IDirect3DDevice9Ex* existingDevice();
    // Makes the back buffer match the game's render target, until freezeBackBuffer: resizing resets the device
    // state, so it stops once a Direct3D 7 device relies on that.
    void matchBackBuffer(UINT width, UINT height);
    void freezeBackBuffer();

    // Shows `sourceRect` of the render target `source` in the window.
    HRESULT present(d9::IDirect3DSurface9* source, const RECT* sourceRect);

    // Drawn into the back buffer right before each present (after the game's frame is in it), on the presenting
    // thread: SacredBild's own UI (Overlay).
    using OverlayFn = void (*)(d9::IDirect3DDevice9Ex* device, d9::IDirect3DSurface9* backBuffer, HWND window);
    void setOverlay(OverlayFn draw);

    // Display mode of the adapter the window is on.
    d9::D3DDISPLAYMODE displayMode();

    bool textureFormat(d9::D3DFORMAT format);
    bool depthFormat(d9::D3DFORMAT format);
}

namespace DDraw9
{
    // Logs a DirectDraw / Direct3D 7 feature the backend doesn't implement, once per feature.
    void unsupported(const char* what);
}
