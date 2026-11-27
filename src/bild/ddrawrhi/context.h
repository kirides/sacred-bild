#pragma once
#include "ddraw9/d3d9_api.h"
#include "spin_lock.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <string>

// DirectDraw 7 / Direct3D 7 on NVRHI ([DDraw] Backend=nvrhi, docs/NvrhiRenderer.md stage 3): what the DirectDraw objects,
// surfaces and Direct3D 7 devices share. One NVRHI device (src/bild/rhi) and one command list that records the frame:
// texture uploads, copies and draws go into it in the order the game makes them, and presenting the frame executes it.
// Everything that records holds lock().
//
// Surface formats keep Direct3D 9's names (d9::D3DFORMAT, ddraw9/format.h does the CPU side); on the GPU every plain RGB
// format is BGRA8 (16-bit textures are expanded when uploaded), DXT1-5 are BC1-3.
namespace DDrawRhi
{
    // Logs a DirectDraw / Direct3D 7 feature this backend doesn't implement, once per feature.
    void unsupported(const char* what);

    // The GPU format of a surface format: BGRA8_UNORM for plain RGB, BC1-3 for DXT1-5, the depth formats; UNKNOWN if none.
    nvrhi::Format gpuFormat(d9::D3DFORMAT format);
    bool isDepth(d9::D3DFORMAT format);
    // Converts `width` x `height` pixels of `format` to BGRA8 (alpha 255 where the format has none).
    bool toBgra8(d9::D3DFORMAT format, const DDPIXELFORMAT& pf, const uint8_t* src, int srcPitch, uint32_t width,
        uint32_t height, uint8_t* dst, uint32_t dstPitch);

    namespace Context
    {
        // Creates the device from the settings ([Render] Api, the other API if that one fails) on first call; false if
        // neither works.
        bool available();
        const std::string& adapterName();

        RecursiveSpinLock& lock();
        nvrhi::IDevice* device();
        // The frame's command list, open. Lock held.
        nvrhi::ICommandList* commands();
        // Counts the command list's openings: volatile constant buffers must be written again in a new one.
        uint32_t epoch();
        // Executes what is recorded; with `wait`, also waits until the GPU is idle (read-backs). Lock held.
        void flush(bool wait);

        // Room in the upload ring: CPU-written memory the GPU reads as vertex and index data for this frame. Lock held.
        struct Upload
        {
            nvrhi::IBuffer* buffer;
            uint32_t offset;
            uint8_t* data;
        };
        bool upload(uint32_t bytes, Upload& out);

        // Values for vertex elements a format lacks, read with a stride of 0 (DDrawRhi::Device's input layouts):
        // normal (0,0,0) at 0, diffuse 0xFFFFFFFF at 12, specular 0 at 16, texture coordinates (0,0,0,0) at 20, bone
        // indices 0 at 36, weights (0,0,0,0) at 40.
        nvrhi::IBuffer* vertexDefaults();
        nvrhi::ITexture* whiteTexture();

        // Samplers by Direct3D 7 stage state: address U and V (D3DTADDRESS_*), MAG/MIN filter linear, mip filter.
        nvrhi::ISampler* sampler(DWORD addressU, DWORD addressV, bool magLinear, bool minLinear, DWORD mipFilter, DWORD border);

        // Framebuffers by their color and depth textures (depth may be null), cached until one of them is forgotten.
        nvrhi::IFramebuffer* framebuffer(nvrhi::ITexture* color, nvrhi::ITexture* depth);
        // A texture is going away: caches holding it drop it (framebuffers here, binding sets via the callback).
        void forget(nvrhi::ITexture* texture);
        using ForgetFn = void (*)(nvrhi::ITexture* texture);
        void onForget(ForgetFn fn);

        // `srcRect` of `src` into `dstRect` of the color target `dst`, stretched (linear or point). Lock held.
        void copy(nvrhi::ITexture* src, const RECT& srcRect, nvrhi::ITexture* dst, const RECT& dstRect, bool linear);
        // Fills `rect` of `color` (if writeColor) and `depth` (if writeDepth) with one color and one depth. Lock held.
        void fill(nvrhi::ITexture* color, nvrhi::ITexture* depth, const RECT& rect, const float rgba[4], bool writeColor,
            bool writeDepth, float z);

        // The game's window (DirectDraw's cooperative level window): the swap chain goes on it.
        void setWindow(HWND window);
        HWND window();
        // Shows `srcRect` (null: all) of the render target `frame` stretched over the window's client area, and starts
        // the next frame.
        HRESULT present(nvrhi::ITexture* frame, const RECT* srcRect);
    }
}
