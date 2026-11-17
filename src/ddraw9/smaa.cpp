// SMAA 1x on the backend's render target (see smaa.h): three full-screen passes with the shaders of smaa.hlsl, the
// lookup textures of src/ddraw9/smaa (AreaTex.h, SearchTex.h) and the frame copied into a texture first.
#include "ddraw9/smaa.h"
#include "ddraw9/d3d9_api.h"
#include "ddraw9/device.h"
#include "ddraw9/gpu.h"
#include "log.h"

#include <cstring>
#include <mutex>

namespace DDraw9
{
    namespace
    {
#include "ddraw9/smaa_shaders.inc"
#include "ddraw9/smaa/AreaTex.h"
#include "ddraw9/smaa/SearchTex.h"

        constexpr int kPresets = 3;

        struct QuadVertex
        {
            float x, y, z;
            float u, v;
        };

        template <class T>
        void release(T*& p)
        {
            if (p)
            {
                p->Release();
                p = nullptr;
            }
        }

        struct State
        {
            d9::IDirect3DDevice9Ex* device = nullptr;   // the one everything below belongs to
            int ready = 0;                              // 0 not tried, 1 ready, -1 unavailable
            d9::IDirect3DVertexShader9* edgeVS = nullptr;
            d9::IDirect3DVertexShader9* blendVS = nullptr;
            d9::IDirect3DPixelShader9* blendPS = nullptr;
            d9::IDirect3DPixelShader9* edgePS[kPresets] = {};
            d9::IDirect3DVertexShader9* weightVS[kPresets] = {};
            d9::IDirect3DPixelShader9* weightPS[kPresets] = {};
            d9::IDirect3DTexture9* area = nullptr;
            d9::IDirect3DTexture9* search = nullptr;
            d9::IDirect3DStateBlock9* saved = nullptr;

            // Per render target size and format.
            UINT width = 0, height = 0;
            d9::D3DFORMAT format = d9::D3DFMT_UNKNOWN;
            d9::IDirect3DTexture9* color = nullptr;     // the frame so far
            d9::IDirect3DTexture9* edges = nullptr;
            d9::IDirect3DTexture9* weights = nullptr;
            bool srgbRead = false, srgbWrite = false;   // the blend pass in linear light where the device can

            void releaseTargets()
            {
                release(color);
                release(edges);
                release(weights);
                width = height = 0;
                format = d9::D3DFMT_UNKNOWN;
            }

            void releaseAll()
            {
                releaseTargets();
                release(edgeVS);
                release(blendVS);
                release(blendPS);
                for (int i = 0; i < kPresets; ++i)
                {
                    release(edgePS[i]);
                    release(weightVS[i]);
                    release(weightPS[i]);
                }
                release(area);
                release(search);
                release(saved);
                device = nullptr;
                ready = 0;
            }
        };
        State g;

        void logFailure(const char* what, HRESULT hr)
        {
            static int logged = 0;
            if (logged < 8)
            {
                ++logged;
                LOG("Direct3D 9: SMAA: {} failed ({:08x})", what, static_cast<uint32_t>(hr));
            }
        }

        // Fills a dynamic texture of one or two bytes per texel from rows of `pitch` bytes.
        HRESULT fill(d9::IDirect3DTexture9* texture, const unsigned char* bytes, UINT pitch, UINT rows)
        {
            d9::D3DLOCKED_RECT locked = {};
            const HRESULT hr = texture->LockRect(0, &locked, nullptr, D3DLOCK_DISCARD);
            if (FAILED(hr))
            {
                return hr;
            }
            auto* dst = static_cast<unsigned char*>(locked.pBits);
            for (UINT y = 0; y < rows; ++y)
            {
                std::memcpy(dst + size_t(y) * locked.Pitch, bytes + size_t(y) * pitch, pitch);
            }
            return texture->UnlockRect(0);
        }

        bool init(d9::IDirect3DDevice9Ex* dev)
        {
            if (g.device != dev)
            {
                g.releaseAll();
                g.device = dev;
            }
            if (g.ready)
            {
                return g.ready > 0;
            }
            g.ready = -1;
            if (Gpu::caps().PixelShaderVersion < D3DPS_VERSION(3, 0) || Gpu::caps().VertexShaderVersion < D3DVS_VERSION(3, 0))
            {
                LOG("Direct3D 9: SMAA: off, the device has no shader model 3");
                return false;
            }
            if (!Gpu::textureFormat(d9::D3DFMT_A8L8) || !Gpu::textureFormat(d9::D3DFMT_L8))
            {
                LOG("Direct3D 9: SMAA: off, no A8L8 / L8 textures for its lookup tables");
                return false;
            }
            const DWORD* const edgePS[kPresets] = {reinterpret_cast<const DWORD*>(kSmaaEdgePSLow),
                reinterpret_cast<const DWORD*>(kSmaaEdgePSMedium), reinterpret_cast<const DWORD*>(kSmaaEdgePSHigh)};
            const DWORD* const weightVS[kPresets] = {reinterpret_cast<const DWORD*>(kSmaaWeightVSLow),
                reinterpret_cast<const DWORD*>(kSmaaWeightVSMedium), reinterpret_cast<const DWORD*>(kSmaaWeightVSHigh)};
            const DWORD* const weightPS[kPresets] = {reinterpret_cast<const DWORD*>(kSmaaWeightPSLow),
                reinterpret_cast<const DWORD*>(kSmaaWeightPSMedium), reinterpret_cast<const DWORD*>(kSmaaWeightPSHigh)};
            HRESULT hr = dev->CreateVertexShader(reinterpret_cast<const DWORD*>(kSmaaEdgeVS), &g.edgeVS);
            if (SUCCEEDED(hr))
            {
                hr = dev->CreateVertexShader(reinterpret_cast<const DWORD*>(kSmaaBlendVS), &g.blendVS);
            }
            if (SUCCEEDED(hr))
            {
                hr = dev->CreatePixelShader(reinterpret_cast<const DWORD*>(kSmaaBlendPS), &g.blendPS);
            }
            for (int i = 0; i < kPresets && SUCCEEDED(hr); ++i)
            {
                hr = dev->CreatePixelShader(edgePS[i], &g.edgePS[i]);
                if (SUCCEEDED(hr))
                {
                    hr = dev->CreateVertexShader(weightVS[i], &g.weightVS[i]);
                }
                if (SUCCEEDED(hr))
                {
                    hr = dev->CreatePixelShader(weightPS[i], &g.weightPS[i]);
                }
            }
            if (FAILED(hr))
            {
                logFailure("creating the shaders", hr);
                return false;
            }
            // The area texture's two channels as A8L8 (sampled as .ra), the search texture as L8.
            hr = dev->CreateTexture(AREATEX_WIDTH, AREATEX_HEIGHT, 1, D3DUSAGE_DYNAMIC, d9::D3DFMT_A8L8, d9::D3DPOOL_DEFAULT,
                &g.area, nullptr);
            if (SUCCEEDED(hr))
            {
                hr = fill(g.area, areaTexBytes, AREATEX_PITCH, AREATEX_HEIGHT);
            }
            if (SUCCEEDED(hr))
            {
                hr = dev->CreateTexture(SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 1, D3DUSAGE_DYNAMIC, d9::D3DFMT_L8,
                    d9::D3DPOOL_DEFAULT, &g.search, nullptr);
            }
            if (SUCCEEDED(hr))
            {
                hr = fill(g.search, searchTexBytes, SEARCHTEX_PITCH, SEARCHTEX_HEIGHT);
            }
            if (SUCCEEDED(hr))
            {
                hr = dev->CreateStateBlock(d9::D3DSBT_ALL, &g.saved);
            }
            if (FAILED(hr))
            {
                logFailure("creating the lookup textures", hr);
                return false;
            }
            g.ready = 1;
            LOG("Direct3D 9: SMAA: ready");
            return true;
        }

        bool targets(d9::IDirect3DDevice9Ex* dev, const d9::D3DSURFACE_DESC& desc)
        {
            if (g.color && g.width == desc.Width && g.height == desc.Height && g.format == desc.Format)
            {
                return true;
            }
            g.releaseTargets();
            HRESULT hr = dev->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, desc.Format,
                d9::D3DPOOL_DEFAULT, &g.color, nullptr);
            if (SUCCEEDED(hr))
            {
                hr = dev->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, d9::D3DFMT_A8R8G8B8,
                    d9::D3DPOOL_DEFAULT, &g.edges, nullptr);
            }
            if (SUCCEEDED(hr))
            {
                hr = dev->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, d9::D3DFMT_A8R8G8B8,
                    d9::D3DPOOL_DEFAULT, &g.weights, nullptr);
            }
            if (FAILED(hr))
            {
                logFailure("creating the render targets", hr);
                g.releaseTargets();
                return false;
            }
            g.width = desc.Width;
            g.height = desc.Height;
            g.format = desc.Format;
            d9::IDirect3D9Ex* d3d = Gpu::d3d();
            g.srgbRead = d3d && SUCCEEDED(d3d->CheckDeviceFormat(Gpu::adapter(), d9::D3DDEVTYPE_HAL, d9::D3DFMT_X8R8G8B8,
                D3DUSAGE_QUERY_SRGBREAD, d9::D3DRTYPE_TEXTURE, desc.Format));
            g.srgbWrite = g.srgbRead && SUCCEEDED(d3d->CheckDeviceFormat(Gpu::adapter(), d9::D3DDEVTYPE_HAL,
                d9::D3DFMT_X8R8G8B8, D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_SRGBWRITE, d9::D3DRTYPE_TEXTURE, desc.Format));
            LOG("Direct3D 9: SMAA: {}x{} targets, blending {}", desc.Width, desc.Height,
                g.srgbWrite ? "in linear light (sRGB read/write)" : "in gamma space");
            return true;
        }

        void sampler(d9::IDirect3DDevice9Ex* dev, DWORD stage, d9::IDirect3DBaseTexture9* texture, bool linear, bool srgb)
        {
            dev->SetTexture(stage, texture);
            dev->SetSamplerState(stage, d9::D3DSAMP_ADDRESSU, d9::D3DTADDRESS_CLAMP);
            dev->SetSamplerState(stage, d9::D3DSAMP_ADDRESSV, d9::D3DTADDRESS_CLAMP);
            dev->SetSamplerState(stage, d9::D3DSAMP_MAGFILTER, linear ? d9::D3DTEXF_LINEAR : d9::D3DTEXF_POINT);
            dev->SetSamplerState(stage, d9::D3DSAMP_MINFILTER, linear ? d9::D3DTEXF_LINEAR : d9::D3DTEXF_POINT);
            dev->SetSamplerState(stage, d9::D3DSAMP_MIPFILTER, d9::D3DTEXF_NONE);
            dev->SetSamplerState(stage, d9::D3DSAMP_MAXMIPLEVEL, 0);
            dev->SetSamplerState(stage, d9::D3DSAMP_SRGBTEXTURE, srgb ? TRUE : FALSE);
        }

        void target(d9::IDirect3DDevice9Ex* dev, d9::IDirect3DTexture9* texture)
        {
            d9::IDirect3DSurface9* surface = nullptr;
            if (SUCCEEDED(texture->GetSurfaceLevel(0, &surface)))
            {
                dev->SetRenderTarget(0, surface);
                surface->Release();
            }
        }

        // One full-screen pass; the quad's corners are moved half a pixel up-left (Direct3D 9 pixel centers).
        void pass(d9::IDirect3DDevice9Ex* dev, d9::IDirect3DVertexShader9* vs, d9::IDirect3DPixelShader9* ps)
        {
            const float px = 1.0f / static_cast<float>(g.width), py = 1.0f / static_cast<float>(g.height);
            const QuadVertex quad[4] = {
                {-1.0f - px, 1.0f + py, 0.5f, 0.0f, 0.0f},
                {1.0f - px, 1.0f + py, 0.5f, 1.0f, 0.0f},
                {-1.0f - px, -1.0f + py, 0.5f, 0.0f, 1.0f},
                {1.0f - px, -1.0f + py, 0.5f, 1.0f, 1.0f},
            };
            dev->SetVertexShader(vs);
            dev->SetPixelShader(ps);
            dev->DrawPrimitiveUP(d9::D3DPT_TRIANGLESTRIP, 2, quad, sizeof(QuadVertex));
        }
    }

    bool Smaa::apply(IDirect3DDevice7* device, Preset preset)
    {
        if (!Device::from(device))
        {
            return false;
        }
        d9::IDirect3DDevice9Ex* dev = Gpu::existingDevice();
        if (!dev)
        {
            return false;
        }
        std::scoped_lock lock(Gpu::stateLock());
        if (!init(dev))
        {
            return false;
        }
        d9::IDirect3DSurface9* frame = nullptr;
        if (FAILED(dev->GetRenderTarget(0, &frame)) || !frame)
        {
            return false;
        }
        d9::D3DSURFACE_DESC desc = {};
        frame->GetDesc(&desc);
        d9::IDirect3DSurface9* colorSurface = nullptr;
        if (!targets(dev, desc) || FAILED(g.color->GetSurfaceLevel(0, &colorSurface)))
        {
            frame->Release();
            return false;
        }
        HRESULT hr = dev->StretchRect(frame, nullptr, colorSurface, nullptr, d9::D3DTEXF_NONE);
        colorSurface->Release();
        if (FAILED(hr))
        {
            logFailure("StretchRect", hr);
            frame->Release();
            return false;
        }

        // Everything the passes change comes back from the state block; the render target and depth buffer it
        // doesn't hold are put back by hand.
        d9::IDirect3DSurface9* depth = nullptr;
        dev->GetDepthStencilSurface(&depth);
        d9::D3DVIEWPORT9 viewport = {};
        dev->GetViewport(&viewport);
        g.saved->Capture();
        const bool scene = SUCCEEDED(dev->BeginScene());     // fails inside the game's own scene: draw in that one

        dev->SetDepthStencilSurface(nullptr);
        dev->SetRenderState(d9::D3DRS_ZENABLE, FALSE);
        dev->SetRenderState(d9::D3DRS_ZWRITEENABLE, FALSE);
        dev->SetRenderState(d9::D3DRS_STENCILENABLE, FALSE);
        dev->SetRenderState(d9::D3DRS_ALPHABLENDENABLE, FALSE);
        dev->SetRenderState(d9::D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetRenderState(d9::D3DRS_CULLMODE, d9::D3DCULL_NONE);
        dev->SetRenderState(d9::D3DRS_FILLMODE, d9::D3DFILL_SOLID);
        dev->SetRenderState(d9::D3DRS_FOGENABLE, FALSE);
        dev->SetRenderState(d9::D3DRS_SCISSORTESTENABLE, FALSE);
        dev->SetRenderState(d9::D3DRS_CLIPPLANEENABLE, 0);
        dev->SetRenderState(d9::D3DRS_COLORWRITEENABLE, 0xF);
        dev->SetRenderState(d9::D3DRS_SRGBWRITEENABLE, FALSE);
        dev->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
        const float metrics[4] = {1.0f / static_cast<float>(g.width), 1.0f / static_cast<float>(g.height),
            static_cast<float>(g.width), static_cast<float>(g.height)};
        dev->SetVertexShaderConstantF(0, metrics, 1);
        dev->SetPixelShaderConstantF(0, metrics, 1);
        const int p = static_cast<int>(preset);

        // 1. Edges by luma, from the frame's colors as they are.
        target(dev, g.edges);
        const d9::D3DVIEWPORT9 full = {0, 0, g.width, g.height, 0.0f, 1.0f};
        dev->SetViewport(&full);
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
        sampler(dev, 0, g.color, true, false);
        pass(dev, g.edgeVS, g.edgePS[p]);

        // 2. Blending weights.
        target(dev, g.weights);
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
        sampler(dev, 0, g.edges, true, false);
        sampler(dev, 1, g.area, true, false);
        sampler(dev, 2, g.search, false, false);
        pass(dev, g.weightVS[p], g.weightPS[p]);

        // 3. Neighborhood blending back into the frame.
        dev->SetRenderTarget(0, frame);
        dev->SetViewport(&full);
        sampler(dev, 0, g.color, true, g.srgbWrite);
        sampler(dev, 1, g.weights, true, false);
        sampler(dev, 2, nullptr, false, false);
        dev->SetRenderState(d9::D3DRS_SRGBWRITEENABLE, g.srgbWrite ? TRUE : FALSE);
        pass(dev, g.blendVS, g.blendPS);

        if (scene)
        {
            dev->EndScene();
        }
        g.saved->Apply();
        dev->SetRenderTarget(0, frame);
        dev->SetDepthStencilSurface(depth);
        dev->SetViewport(&viewport);
        // The Direct3D 7 device sets what it believes is set again (shaders, bindings, sampler states).
        Gpu::overlayDrew();
        frame->Release();
        if (depth)
        {
            depth->Release();
        }
        return true;
    }
}
