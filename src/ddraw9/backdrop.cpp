// Backdrops around the UI canvas (see backdrop.h): a chain of half-size render target textures blurs the canvas by
// box filtering, and its smallest one is drawn magnified with bilinear filtering.
#include "ddraw9/backdrop.h"
#include "ddraw9/gpu.h"
#include "log.h"

#include <algorithm>
#include <vector>

namespace DDraw9
{
    namespace
    {
        constexpr UINT kSmallest = 32;          // the blur ends at a texture about this wide
        constexpr DWORD kFillColor = 0xFF6A6A6A;    // the blurred screen at ~40 % brightness
        constexpr DWORD kDimColor = 0xA0000000;     // the world beside a window at ~37 % brightness

        struct Level
        {
            d9::IDirect3DTexture9* texture = nullptr;
            d9::IDirect3DSurface9* surface = nullptr;
            UINT width = 0, height = 0;
        };

        std::vector<Level> g_levels;
        d9::IDirect3DDevice9Ex* g_device = nullptr;
        UINT g_canvasWidth = 0, g_canvasHeight = 0;     // the levels' source size
        bool g_captured = false;
        bool g_failed = false;

        void release()
        {
            for (Level& l : g_levels)
            {
                if (l.surface)
                {
                    l.surface->Release();
                }
                if (l.texture)
                {
                    l.texture->Release();
                }
            }
            g_levels.clear();
            g_captured = false;
        }

        bool levels(d9::IDirect3DDevice9Ex* device, UINT width, UINT height)
        {
            if (device == g_device && width == g_canvasWidth && height == g_canvasHeight && !g_levels.empty())
            {
                return true;
            }
            release();
            g_device = device;
            g_canvasWidth = width;
            g_canvasHeight = height;
            UINT w = width, h = height;
            do
            {
                w = std::max(w / 2, 1u);
                h = std::max(h / 2, 1u);
                Level l;
                l.width = w;
                l.height = h;
                HRESULT hr = device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, d9::D3DFMT_X8R8G8B8, d9::D3DPOOL_DEFAULT,
                    &l.texture, nullptr);
                if (SUCCEEDED(hr))
                {
                    hr = l.texture->GetSurfaceLevel(0, &l.surface);
                }
                if (FAILED(hr))
                {
                    if (l.texture)
                    {
                        l.texture->Release();
                    }
                    LOG("Backdrop: creating a {}x{} render target texture failed ({:08x}), black bars instead", w, h,
                        static_cast<uint32_t>(hr));
                    release();
                    g_failed = true;
                    return false;
                }
                g_levels.push_back(l);
            } while (w > kSmallest);
            return true;
        }

        struct Vertex
        {
            float x, y, z, rhw;
            DWORD color;
            float u, v;
        };

        // The rects of the render target outside `canvas`.
        int bars(const RECT& canvas, LONG width, LONG height, RECT (&out)[4])
        {
            const RECT c = {std::clamp(canvas.left, 0L, width), std::clamp(canvas.top, 0L, height),
                std::clamp(canvas.right, 0L, width), std::clamp(canvas.bottom, 0L, height)};
            const RECT all[4] = {{0, 0, c.left, height}, {c.right, 0, width, height}, {c.left, 0, c.right, c.top},
                {c.left, c.bottom, c.right, height}};
            int n = 0;
            for (const RECT& r : all)
            {
                if (r.right > r.left && r.bottom > r.top)
                {
                    out[n++] = r;
                }
            }
            return n;
        }

        bool targetSize(d9::IDirect3DDevice9Ex* device, UINT& width, UINT& height)
        {
            d9::IDirect3DSurface9* target = nullptr;
            if (FAILED(device->GetRenderTarget(0, &target)) || !target)
            {
                return false;
            }
            d9::D3DSURFACE_DESC desc = {};
            const HRESULT hr = target->GetDesc(&desc);
            target->Release();
            width = desc.Width;
            height = desc.Height;
            return SUCCEEDED(hr) && width && height;
        }

        // Draws a quad over the bars of the current render target: `texture` (null: untextured) with `color`, blended
        // by its alpha when `blend`.
        bool drawBars(const RECT& canvas, d9::IDirect3DTexture9* texture, const Vertex (&quad)[4], bool blend)
        {
            d9::IDirect3DDevice9Ex* device = Gpu::existingDevice();
            UINT width = 0, height = 0;
            if (!device || !targetSize(device, width, height))
            {
                return false;
            }
            RECT rects[4];
            const int count = bars(canvas, static_cast<LONG>(width), static_cast<LONG>(height), rects);
            if (!count)
            {
                return true;
            }
            d9::IDirect3DStateBlock9* saved = nullptr;
            if (FAILED(device->CreateStateBlock(d9::D3DSBT_ALL, &saved)))
            {
                return false;
            }
            const bool ownScene = SUCCEEDED(device->BeginScene());
            const d9::D3DVIEWPORT9 viewport = {0, 0, width, height, 0.0f, 1.0f};
            device->SetViewport(&viewport);
            device->SetPixelShader(nullptr);
            device->SetVertexShader(nullptr);
            device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
            device->SetTexture(0, texture);
            device->SetTexture(1, nullptr);
            device->SetRenderState(d9::D3DRS_ZENABLE, FALSE);
            device->SetRenderState(d9::D3DRS_ZWRITEENABLE, FALSE);
            device->SetRenderState(d9::D3DRS_CULLMODE, d9::D3DCULL_NONE);
            device->SetRenderState(d9::D3DRS_LIGHTING, FALSE);
            device->SetRenderState(d9::D3DRS_FOGENABLE, FALSE);
            device->SetRenderState(d9::D3DRS_ALPHATESTENABLE, FALSE);
            device->SetRenderState(d9::D3DRS_STENCILENABLE, FALSE);
            device->SetRenderState(d9::D3DRS_SRGBWRITEENABLE, FALSE);
            device->SetRenderState(d9::D3DRS_FILLMODE, d9::D3DFILL_SOLID);
            device->SetRenderState(d9::D3DRS_SHADEMODE, d9::D3DSHADE_GOURAUD);
            device->SetRenderState(d9::D3DRS_COLORWRITEENABLE, 0xF);
            device->SetRenderState(d9::D3DRS_ALPHABLENDENABLE, blend);
            device->SetRenderState(d9::D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
            device->SetRenderState(d9::D3DRS_BLENDOP, d9::D3DBLENDOP_ADD);
            device->SetRenderState(d9::D3DRS_SRCBLEND, d9::D3DBLEND_SRCALPHA);
            device->SetRenderState(d9::D3DRS_DESTBLEND, d9::D3DBLEND_INVSRCALPHA);
            device->SetRenderState(d9::D3DRS_SCISSORTESTENABLE, TRUE);
            device->SetTextureStageState(0, d9::D3DTSS_COLOROP, texture ? d9::D3DTOP_MODULATE : d9::D3DTOP_SELECTARG2);
            device->SetTextureStageState(0, d9::D3DTSS_COLORARG1, D3DTA_TEXTURE);
            device->SetTextureStageState(0, d9::D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            device->SetTextureStageState(0, d9::D3DTSS_ALPHAOP, d9::D3DTOP_SELECTARG2);
            device->SetTextureStageState(0, d9::D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
            device->SetTextureStageState(0, d9::D3DTSS_TEXCOORDINDEX, 0);
            device->SetTextureStageState(0, d9::D3DTSS_TEXTURETRANSFORMFLAGS, d9::D3DTTFF_DISABLE);
            device->SetTextureStageState(1, d9::D3DTSS_COLOROP, d9::D3DTOP_DISABLE);
            device->SetTextureStageState(1, d9::D3DTSS_ALPHAOP, d9::D3DTOP_DISABLE);
            device->SetSamplerState(0, d9::D3DSAMP_MINFILTER, d9::D3DTEXF_LINEAR);
            device->SetSamplerState(0, d9::D3DSAMP_MAGFILTER, d9::D3DTEXF_LINEAR);
            device->SetSamplerState(0, d9::D3DSAMP_MIPFILTER, d9::D3DTEXF_NONE);
            device->SetSamplerState(0, d9::D3DSAMP_ADDRESSU, d9::D3DTADDRESS_CLAMP);
            device->SetSamplerState(0, d9::D3DSAMP_ADDRESSV, d9::D3DTADDRESS_CLAMP);
            device->SetSamplerState(0, d9::D3DSAMP_SRGBTEXTURE, FALSE);
            for (int i = 0; i < count; ++i)
            {
                device->SetScissorRect(&rects[i]);
                device->DrawPrimitiveUP(d9::D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
            }
            if (ownScene)
            {
                device->EndScene();
            }
            saved->Apply();
            saved->Release();
            return true;
        }
    }

    void Backdrop::capture(const RECT& canvas)
    {
        d9::IDirect3DDevice9Ex* device = Gpu::existingDevice();
        const LONG width = canvas.right - canvas.left, height = canvas.bottom - canvas.top;
        if (!device || g_failed || width < 2 * static_cast<LONG>(kSmallest) || height < 2)
        {
            return;
        }
        if (!levels(device, static_cast<UINT>(width), static_cast<UINT>(height)))
        {
            return;
        }
        d9::IDirect3DSurface9* target = nullptr;
        if (FAILED(device->GetRenderTarget(0, &target)) || !target)
        {
            return;
        }
        // Each halving averages 2x2 texels with bilinear filtering: a box blur that widens with every level.
        HRESULT hr = device->StretchRect(target, &canvas, g_levels[0].surface, nullptr, d9::D3DTEXF_LINEAR);
        target->Release();
        for (size_t i = 1; i < g_levels.size() && SUCCEEDED(hr); ++i)
        {
            hr = device->StretchRect(g_levels[i - 1].surface, nullptr, g_levels[i].surface, nullptr, d9::D3DTEXF_LINEAR);
        }
        g_captured = SUCCEEDED(hr);
    }

    bool Backdrop::fill(const RECT& canvas)
    {
        d9::IDirect3DDevice9Ex* device = Gpu::existingDevice();
        UINT width = 0, height = 0;
        if (!device || !g_captured || device != g_device || g_levels.empty() || !targetSize(device, width, height) ||
            canvas.right <= canvas.left || canvas.bottom <= canvas.top)
        {
            return false;
        }
        // The capture scaled to cover the render target, centered on the canvas.
        const float cw = static_cast<float>(canvas.right - canvas.left), ch = static_cast<float>(canvas.bottom - canvas.top);
        const float scale = std::max(static_cast<float>(width) / cw, static_cast<float>(height) / ch);
        const float cx = (canvas.left + canvas.right) * 0.5f, cy = (canvas.top + canvas.bottom) * 0.5f;
        const float l = cx - cw * scale * 0.5f, r = cx + cw * scale * 0.5f;
        const float t = cy - ch * scale * 0.5f, b = cy + ch * scale * 0.5f;
        const Vertex quad[4] = {
            {l, t, 0.0f, 1.0f, kFillColor, 0.0f, 0.0f},
            {r, t, 0.0f, 1.0f, kFillColor, 1.0f, 0.0f},
            {l, b, 0.0f, 1.0f, kFillColor, 0.0f, 1.0f},
            {r, b, 0.0f, 1.0f, kFillColor, 1.0f, 1.0f},
        };
        return drawBars(canvas, g_levels.back().texture, quad, false);
    }

    bool Backdrop::dim(const RECT& canvas)
    {
        d9::IDirect3DDevice9Ex* device = Gpu::existingDevice();
        UINT width = 0, height = 0;
        if (!device || !targetSize(device, width, height))
        {
            return false;
        }
        const float w = static_cast<float>(width), h = static_cast<float>(height);
        const Vertex quad[4] = {
            {0.0f, 0.0f, 0.0f, 1.0f, kDimColor, 0.0f, 0.0f},
            {w, 0.0f, 0.0f, 1.0f, kDimColor, 0.0f, 0.0f},
            {0.0f, h, 0.0f, 1.0f, kDimColor, 0.0f, 0.0f},
            {w, h, 0.0f, 1.0f, kDimColor, 0.0f, 0.0f},
        };
        return drawBars(canvas, nullptr, quad, true);
    }

    void Backdrop::forget()
    {
        g_captured = false;
    }
}
