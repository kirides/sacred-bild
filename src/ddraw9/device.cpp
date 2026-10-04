#include "ddraw9/device.h"
#include "ddraw9/directdraw.h"
#include "ddraw9/format.h"
#include "ddraw9/gpu.h"
#include "ddraw9/surface.h"
#include "ddraw9/vertex_buffer.h"
#include "render/fvf.h"
#include "log.h"

#include <intrin.h>
#include <algorithm>
#include <cstring>
#include <format>
#include <mutex>

namespace DDraw9
{
    static_assert(sizeof(D3DLIGHT7) == sizeof(d9::D3DLIGHT9));
    static_assert(sizeof(D3DMATERIAL7) == sizeof(d9::D3DMATERIAL9));
    static_assert(sizeof(D3DVIEWPORT7) == sizeof(d9::D3DVIEWPORT9));

    namespace
    {
        const void* g_deviceVtable = nullptr;

        // Direct3D 7 render states that Direct3D 9 has under the same number with the same meaning.
        bool sharedRenderState(DWORD state)
        {
            switch (state)
            {
            case 7: case 8: case 9:                     // ZENABLE, FILLMODE, SHADEMODE
            case 14: case 15: case 16:                  // ZWRITEENABLE, ALPHATESTENABLE, LASTPIXEL
            case 19: case 20:                           // SRCBLEND, DESTBLEND
            case 22: case 23: case 24: case 25:         // CULLMODE, ZFUNC, ALPHAREF, ALPHAFUNC
            case 26: case 27: case 28: case 29:         // DITHERENABLE, ALPHABLENDENABLE, FOGENABLE, SPECULARENABLE
            case 34: case 35: case 36: case 37: case 38: // FOGCOLOR, FOGTABLEMODE, FOGSTART, FOGEND, FOGDENSITY
            case 48:                                    // RANGEFOGENABLE
            case 139: case 140: case 141: case 142: case 143:   // AMBIENT, FOGVERTEXMODE, COLORVERTEX, LOCALVIEWER, NORMALIZENORMALS
            case 145: case 146: case 147: case 148:     // DIFFUSE/SPECULAR/AMBIENT/EMISSIVEMATERIALSOURCE
            case 151: case 152:                         // VERTEXBLEND, CLIPPLANEENABLE
                return true;
            default:
                return (state >= 52 && state <= 60)     // STENCIL*, TEXTUREFACTOR
                    || (state >= 128 && state <= 137);  // WRAP0-7, CLIPPING, LIGHTING
            }
        }

        // Direct3D 7 render states without a Direct3D 9 counterpart that only change how things look, if at all.
        bool ignoredRenderState(DWORD state)
        {
            switch (state)
            {
            case D3DRENDERSTATE_ANTIALIAS:
            case D3DRENDERSTATE_TEXTUREPERSPECTIVE:     // always on
            case D3DRENDERSTATE_LINEPATTERN:
            case D3DRENDERSTATE_ZVISIBLE:
            case D3DRENDERSTATE_STIPPLEDALPHA:
            case D3DRENDERSTATE_EDGEANTIALIAS:
            case D3DRENDERSTATE_EXTENTS:
            case D3DRENDERSTATE_COLORKEYBLENDENABLE:
                return true;
            default:
                return false;
            }
        }

        DWORD primitiveCount(D3DPRIMITIVETYPE type, DWORD count)
        {
            switch (type)
            {
            case D3DPT_POINTLIST: return count;
            case D3DPT_LINELIST: return count / 2;
            case D3DPT_LINESTRIP: return count > 1 ? count - 1 : 0;
            case D3DPT_TRIANGLELIST: return count / 3;
            case D3DPT_TRIANGLESTRIP: case D3DPT_TRIANGLEFAN: return count > 2 ? count - 2 : 0;
            default: return 0;
            }
        }

        d9::D3DPRIMITIVETYPE primitive9(D3DPRIMITIVETYPE type)
        {
            return static_cast<d9::D3DPRIMITIVETYPE>(type);     // same values
        }

        // Direct3D 9 transform state of a Direct3D 7 one.
        bool transform9(DWORD type, d9::D3DTRANSFORMSTATETYPE& out)
        {
            switch (type)
            {
            case D3DTRANSFORMSTATE_WORLD: out = static_cast<d9::D3DTRANSFORMSTATETYPE>(256); return true;
            case D3DTRANSFORMSTATE_VIEW: out = d9::D3DTS_VIEW; return true;
            case D3DTRANSFORMSTATE_PROJECTION: out = d9::D3DTS_PROJECTION; return true;
            case D3DTRANSFORMSTATE_WORLD1: out = static_cast<d9::D3DTRANSFORMSTATETYPE>(257); return true;
            case D3DTRANSFORMSTATE_WORLD2: out = static_cast<d9::D3DTRANSFORMSTATETYPE>(258); return true;
            case D3DTRANSFORMSTATE_WORLD3: out = static_cast<d9::D3DTRANSFORMSTATETYPE>(259); return true;
            default:
                if (type >= D3DTRANSFORMSTATE_TEXTURE0 && type <= D3DTRANSFORMSTATE_TEXTURE7)
                {
                    out = static_cast<d9::D3DTRANSFORMSTATETYPE>(type);
                    return true;
                }
                return false;
            }
        }

        D3DMATRIX identity()
        {
            D3DMATRIX m = {};
            m._11 = m._22 = m._33 = m._44 = 1.0f;
            return m;
        }

        D3DMATRIX multiply(const D3DMATRIX& a, const D3DMATRIX& b)
        {
            D3DMATRIX r;
            const float(*A)[4] = reinterpret_cast<const float(*)[4]>(&a);
            const float(*B)[4] = reinterpret_cast<const float(*)[4]>(&b);
            float(*R)[4] = reinterpret_cast<float(*)[4]>(&r);
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    R[i][j] = A[i][0] * B[0][j] + A[i][1] * B[1][j] + A[i][2] * B[2][j] + A[i][3] * B[3][j];
            return r;
        }

        DWORD floatBits(float f)
        {
            DWORD d;
            std::memcpy(&d, &f, sizeof(d));
            return d;
        }

        void logFailure(const char* what, HRESULT hr)
        {
            static std::mutex mutex;
            static int logged = 0;
            std::scoped_lock lock(mutex);
            if (logged < 16)
            {
                ++logged;
                LOG("Direct3D 9: {} failed ({:08x})", what, static_cast<uint32_t>(hr));
            }
        }
    }

    // ------------------------------------------------------------------------------------------------------------
    // Device description

    D3DDEVICEDESC7 deviceDescription(REFCLSID type)
    {
        Gpu::available();
        const d9::D3DCAPS9& c = Gpu::caps();
        D3DDEVICEDESC7 d = {};
        d.dwDevCaps = (c.DevCaps & 0x000FFFFF) | D3DDEVCAPS_FLOATTLVERTEX | D3DDEVCAPS_DRAWPRIMTLVERTEX |
            D3DDEVCAPS_TEXTUREVIDEOMEMORY | D3DDEVCAPS_TLVERTEXSYSTEMMEMORY | D3DDEVCAPS_EXECUTESYSTEMMEMORY |
            D3DDEVCAPS_DRAWPRIMITIVES2 | D3DDEVCAPS_DRAWPRIMITIVES2EX | D3DDEVCAPS_HWRASTERIZATION | D3DDEVCAPS_HWTRANSFORMANDLIGHT;
        if (type == IID_IDirect3DRGBDevice)
        {
            d.dwDevCaps &= ~(D3DDEVCAPS_HWRASTERIZATION | D3DDEVCAPS_HWTRANSFORMANDLIGHT);
        }
        else if (type == IID_IDirect3DHALDevice)
        {
            d.dwDevCaps &= ~D3DDEVCAPS_HWTRANSFORMANDLIGHT;
        }

        D3DPRIMCAPS p = {};
        p.dwSize = sizeof(p);
        p.dwMiscCaps = c.PrimitiveMiscCaps & (D3DPMISCCAPS_MASKZ | D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW);
        p.dwRasterCaps = (c.RasterCaps & 0x003FFFFF) | D3DPRASTERCAPS_SUBPIXEL |
            ((c.RasterCaps & 0x04000000) ? D3DPRASTERCAPS_ZBIAS : 0);   // D3DPRASTERCAPS_DEPTHBIAS
        p.dwZCmpCaps = c.ZCmpCaps;
        p.dwSrcBlendCaps = c.SrcBlendCaps & 0x1FFF;
        p.dwDestBlendCaps = c.DestBlendCaps & 0x1FFF;
        p.dwAlphaCmpCaps = c.AlphaCmpCaps;
        p.dwShadeCaps = c.ShadeCaps | D3DPSHADECAPS_COLORFLATRGB | D3DPSHADECAPS_COLORGOURAUDRGB |
            D3DPSHADECAPS_SPECULARFLATRGB | D3DPSHADECAPS_SPECULARGOURAUDRGB | D3DPSHADECAPS_ALPHAFLATBLEND |
            D3DPSHADECAPS_ALPHAGOURAUDBLEND | D3DPSHADECAPS_FOGFLAT | D3DPSHADECAPS_FOGGOURAUD;
        p.dwTextureCaps = c.TextureCaps & 0x0FFF;
        p.dwTextureFilterCaps = (c.TextureFilterCaps & (0x0700 | 0x30000 | 0x07000000)) | D3DPTFILTERCAPS_NEAREST |
            D3DPTFILTERCAPS_LINEAR | D3DPTFILTERCAPS_MIPNEAREST | D3DPTFILTERCAPS_MIPLINEAR |
            D3DPTFILTERCAPS_LINEARMIPNEAREST | D3DPTFILTERCAPS_LINEARMIPLINEAR;
        p.dwTextureBlendCaps = D3DPTBLENDCAPS_DECAL | D3DPTBLENDCAPS_MODULATE | D3DPTBLENDCAPS_DECALALPHA |
            D3DPTBLENDCAPS_MODULATEALPHA | D3DPTBLENDCAPS_DECALMASK | D3DPTBLENDCAPS_MODULATEMASK | D3DPTBLENDCAPS_COPY |
            D3DPTBLENDCAPS_ADD;
        p.dwTextureAddressCaps = c.TextureAddressCaps & 0x1F;
        d.dpcLineCaps = p;
        d.dpcTriCaps = p;
        d.dwDeviceRenderBitDepth = DDBD_16 | DDBD_32;
        d.dwDeviceZBufferBitDepth = DDBD_16 | DDBD_24 | DDBD_32;
        d.dwMinTextureWidth = 1;
        d.dwMinTextureHeight = 1;
        d.dwMaxTextureWidth = c.MaxTextureWidth;
        d.dwMaxTextureHeight = c.MaxTextureHeight;
        d.dwMaxTextureRepeat = c.MaxTextureRepeat;
        d.dwMaxTextureAspectRatio = c.MaxTextureAspectRatio;
        d.dwMaxAnisotropy = c.MaxAnisotropy;
        d.dvGuardBandLeft = c.GuardBandLeft;
        d.dvGuardBandTop = c.GuardBandTop;
        d.dvGuardBandRight = c.GuardBandRight;
        d.dvGuardBandBottom = c.GuardBandBottom;
        d.dvExtentsAdjust = c.ExtentsAdjust;
        d.dwStencilCaps = c.StencilCaps & 0xFF;
        d.dwFVFCaps = (std::min<DWORD>(c.FVFCaps & D3DFVFCAPS_TEXCOORDCOUNTMASK, 8)) | D3DFVFCAPS_DONOTSTRIPELEMENTS;
        d.dwTextureOpCaps = c.TextureOpCaps & 0x00FFFFFF;
        d.wMaxTextureBlendStages = static_cast<WORD>(std::min<DWORD>(c.MaxTextureBlendStages, 8));
        d.wMaxSimultaneousTextures = static_cast<WORD>(std::min<DWORD>(c.MaxSimultaneousTextures, 8));
        d.dwMaxActiveLights = c.MaxActiveLights;
        d.dvMaxVertexW = c.MaxVertexW;
        d.deviceGUID = type;
        d.wMaxUserClipPlanes = static_cast<WORD>(c.MaxUserClipPlanes);
        d.wMaxVertexBlendMatrices = static_cast<WORD>(c.MaxVertexBlendMatrices);
        d.dwVertexProcessingCaps = c.VertexProcessingCaps & 0x3F;
        return d;
    }

    // ------------------------------------------------------------------------------------------------------------
    // Creation

    Device::Device(DirectDraw* ddraw, d9::IDirect3DDevice9Ex* dev) : m_ddraw(ddraw), m_dev(dev)
    {
        m_ddraw->AddRef();
        m_dev->AddRef();
        if (!g_deviceVtable)
        {
            g_deviceVtable = *reinterpret_cast<void* const*>(static_cast<IDirect3DDevice7*>(this));
        }
    }

    Device* Device::from(const void* iface)
    {
        if (iface && g_deviceVtable && *static_cast<const void* const*>(iface) == g_deviceVtable)
        {
            return static_cast<Device*>(static_cast<IDirect3DDevice7*>(const_cast<void*>(iface)));
        }
        return nullptr;
    }

    Device::~Device()
    {
        for (Surface*& texture : m_textures)
        {
            if (texture)
            {
                texture->Release();
            }
        }
        if (m_target)
        {
            m_target->Release();
        }
        if (m_indexBuffer)
        {
            m_indexBuffer->Release();
        }
        releaseSkin();
        m_dev->Release();
        m_ddraw->Release();
    }

    Device::StateBlock::~StateBlock()
    {
        for (auto& [stage, texture] : textures)
        {
            if (texture)
            {
                texture->Release();
            }
        }
    }

    HRESULT Device::create(DirectDraw* ddraw, REFCLSID, Surface* target, Device** out)
    {
        *out = nullptr;
        if (target->kind() == Surface::Kind::Primary)
        {
            // A flip chain's primary: Direct3D draws into its back buffer.
            DDSCAPS2 caps = {DDSCAPS_BACKBUFFER};
            LPDIRECTDRAWSURFACE7 back = nullptr;
            if (FAILED(target->GetAttachedSurface(&caps, &back)))
            {
                return DDERR_INVALIDPARAMS;
            }
            target = Surface::from(back);
            back->Release();    // the primary keeps it alive; bindTarget takes the device's own reference
        }
        if (target->kind() != Surface::Kind::RenderTarget)
        {
            return DDERR_INVALIDPARAMS;
        }
        d9::IDirect3DDevice9Ex* dev = Gpu::existingDevice();
        if (!dev)
        {
            return DDERR_GENERIC;
        }
        Gpu::freezeBackBuffer();
        auto* device = new Device(ddraw, dev);
        const HRESULT hr = device->bindTarget(target);
        if (FAILED(hr))
        {
            device->Release();
            return hr;
        }
        device->initState();
        Surface* depth = target->attachedDepth();
        LOG("Direct3D 9: Direct3D 7 device on a {}x{} {} render target, z-buffer {}", target->width(), target->height(),
            Format::name(target->format()), depth ? Format::name(depth->format()) : std::string("none"));
        *out = device;
        return D3D_OK;
    }

    HRESULT Device::bindTarget(Surface* target)
    {
        if (!target || target->kind() != Surface::Kind::RenderTarget)
        {
            return DDERR_INVALIDPARAMS;
        }
        HRESULT hr = m_dev->SetRenderTarget(0, target->renderTarget());
        if (FAILED(hr))
        {
            logFailure("SetRenderTarget", hr);
            return DDERR_GENERIC;
        }
        Surface* depth = target->attachedDepth();
        m_dev->SetDepthStencilSurface(depth ? depth->depthStencil() : nullptr);
        m_depth = depth;
        target->AddRef();
        if (m_target)
        {
            m_target->Release();
        }
        m_target = target;
        // Direct3D 9 resets the viewport to the whole render target.
        m_viewport = {0, 0, target->width(), target->height(), 0.0f, 1.0f};
        m_viewport9 = {0, 0, target->width(), target->height(), 0.0f, 1.0f};
        return D3D_OK;
    }

    void Device::initState()
    {
        // Direct3D 7's defaults, applied to Direct3D 9 so both copies start out equal.
        DWORD* rs = m_rs;
        rs[D3DRENDERSTATE_TEXTUREPERSPECTIVE] = TRUE;
        rs[D3DRENDERSTATE_ZENABLE] = m_depth ? D3DZB_TRUE : D3DZB_FALSE;
        rs[D3DRENDERSTATE_FILLMODE] = D3DFILL_SOLID;
        rs[D3DRENDERSTATE_SHADEMODE] = D3DSHADE_GOURAUD;
        rs[D3DRENDERSTATE_ZWRITEENABLE] = TRUE;
        rs[D3DRENDERSTATE_ALPHATESTENABLE] = FALSE;
        rs[D3DRENDERSTATE_LASTPIXEL] = TRUE;
        rs[D3DRENDERSTATE_SRCBLEND] = D3DBLEND_ONE;
        rs[D3DRENDERSTATE_DESTBLEND] = D3DBLEND_ZERO;
        rs[D3DRENDERSTATE_CULLMODE] = D3DCULL_CCW;
        rs[D3DRENDERSTATE_ZFUNC] = D3DCMP_LESSEQUAL;
        rs[D3DRENDERSTATE_ALPHAREF] = 0;
        rs[D3DRENDERSTATE_ALPHAFUNC] = D3DCMP_ALWAYS;
        rs[D3DRENDERSTATE_DITHERENABLE] = FALSE;
        rs[D3DRENDERSTATE_ALPHABLENDENABLE] = FALSE;
        rs[D3DRENDERSTATE_FOGENABLE] = FALSE;
        rs[D3DRENDERSTATE_SPECULARENABLE] = FALSE;
        rs[D3DRENDERSTATE_FOGCOLOR] = 0;
        rs[D3DRENDERSTATE_FOGTABLEMODE] = D3DFOG_NONE;
        rs[D3DRENDERSTATE_FOGSTART] = floatBits(0.0f);
        rs[D3DRENDERSTATE_FOGEND] = floatBits(1.0f);
        rs[D3DRENDERSTATE_FOGDENSITY] = floatBits(1.0f);
        rs[D3DRENDERSTATE_RANGEFOGENABLE] = FALSE;
        rs[D3DRENDERSTATE_STENCILENABLE] = FALSE;
        rs[D3DRENDERSTATE_STENCILFAIL] = D3DSTENCILOP_KEEP;
        rs[D3DRENDERSTATE_STENCILZFAIL] = D3DSTENCILOP_KEEP;
        rs[D3DRENDERSTATE_STENCILPASS] = D3DSTENCILOP_KEEP;
        rs[D3DRENDERSTATE_STENCILFUNC] = D3DCMP_ALWAYS;
        rs[D3DRENDERSTATE_STENCILREF] = 0;
        rs[D3DRENDERSTATE_STENCILMASK] = 0xFFFFFFFF;
        rs[D3DRENDERSTATE_STENCILWRITEMASK] = 0xFFFFFFFF;
        rs[D3DRENDERSTATE_TEXTUREFACTOR] = 0xFFFFFFFF;
        rs[D3DRENDERSTATE_CLIPPING] = TRUE;
        rs[D3DRENDERSTATE_LIGHTING] = TRUE;
        rs[D3DRENDERSTATE_AMBIENT] = 0;
        rs[D3DRENDERSTATE_FOGVERTEXMODE] = D3DFOG_NONE;
        rs[D3DRENDERSTATE_COLORVERTEX] = TRUE;
        rs[D3DRENDERSTATE_LOCALVIEWER] = TRUE;
        rs[D3DRENDERSTATE_NORMALIZENORMALS] = FALSE;
        rs[D3DRENDERSTATE_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1;
        rs[D3DRENDERSTATE_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
        rs[D3DRENDERSTATE_AMBIENTMATERIALSOURCE] = D3DMCS_COLOR2;
        rs[D3DRENDERSTATE_EMISSIVEMATERIALSOURCE] = D3DMCS_MATERIAL;
        rs[D3DRENDERSTATE_VERTEXBLEND] = D3DVBLEND_DISABLE;
        rs[D3DRENDERSTATE_CLIPPLANEENABLE] = 0;
        for (DWORD state = 0; state < kRenderStates; ++state)
        {
            if (sharedRenderState(state))
            {
                m_rs9Known[state] = false;
                applyRenderState(state, rs[state]);
            }
        }

        for (DWORD stage = 0; stage < kStages; ++stage)
        {
            DWORD* tss = m_tss[stage];
            tss[D3DTSS_COLOROP] = stage == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE;
            tss[D3DTSS_COLORARG1] = D3DTA_TEXTURE;
            tss[D3DTSS_COLORARG2] = D3DTA_CURRENT;
            tss[D3DTSS_ALPHAOP] = stage == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
            tss[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
            tss[D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
            tss[D3DTSS_TEXCOORDINDEX] = stage;
            tss[D3DTSS_ADDRESS] = tss[D3DTSS_ADDRESSU] = tss[D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
            tss[D3DTSS_MAGFILTER] = D3DTFG_POINT;
            tss[D3DTSS_MINFILTER] = D3DTFN_POINT;
            tss[D3DTSS_MIPFILTER] = D3DTFP_NONE;
            tss[D3DTSS_MAXANISOTROPY] = 1;
            tss[D3DTSS_TEXTURETRANSFORMFLAGS] = D3DTTFF_DISABLE;
            for (DWORD type = 1; type <= D3DTSS_TEXTURETRANSFORMFLAGS; ++type)
            {
                if (type != D3DTSS_ADDRESS)
                {
                    applyStageState(stage, type, tss[type]);
                }
            }
            m_dev->SetTexture(stage, nullptr);
        }

        const D3DMATRIX id = identity();
        for (DWORD type = 0; type < kTransforms; ++type)
        {
            m_transforms[type] = id;
            applyTransform(type, id);
        }
        const d9::D3DVIEWPORT9 vp = m_viewport9;
        m_dev->SetViewport(&vp);
        m_material = {};
        m_dev->SetMaterial(reinterpret_cast<const d9::D3DMATERIAL9*>(&m_material));
    }

    // ------------------------------------------------------------------------------------------------------------
    // State translation

    void Device::setRenderState9(DWORD state, DWORD value)
    {
        if (m_rs9Known[state] && m_rs9[state] == value)
        {
            return;
        }
        m_rs9[state] = value;
        m_rs9Known[state] = true;
        m_dev->SetRenderState(static_cast<d9::D3DRENDERSTATETYPE>(state), value);
    }

    void Device::setStageState9(DWORD stage, DWORD type, DWORD value)
    {
        if (m_tss9Known[stage][type] && m_tss9[stage][type] == value)
        {
            return;
        }
        m_tss9[stage][type] = value;
        m_tss9Known[stage][type] = true;
        m_dev->SetTextureStageState(stage, static_cast<d9::D3DTEXTURESTAGESTATETYPE>(type), value);
    }

    void Device::setSamplerState9(DWORD stage, DWORD type, DWORD value)
    {
        if (m_samp9Known[stage][type] && m_samp9[stage][type] == value)
        {
            return;
        }
        m_samp9[stage][type] = value;
        m_samp9Known[stage][type] = true;
        m_dev->SetSamplerState(stage, static_cast<d9::D3DSAMPLERSTATETYPE>(type), value);
    }

    HRESULT Device::applyRenderState(DWORD state, DWORD value)
    {
        if (sharedRenderState(state))
        {
            if (state == D3DRENDERSTATE_ZENABLE && value == D3DZB_USEW && !(Gpu::caps().RasterCaps & D3DPRASTERCAPS_WBUFFER))
            {
                value = D3DZB_TRUE;
            }
            else if (state == D3DRENDERSTATE_SHADEMODE && value == D3DSHADE_PHONG)
            {
                value = D3DSHADE_GOURAUD;
            }
            setRenderState9(state, value);
            return D3D_OK;
        }
        if (state == D3DRENDERSTATE_ZBIAS)
        {
            // 0..16, larger is closer; Direct3D 9 adds a depth offset instead.
            setRenderState9(195, floatBits(-static_cast<float>(value) * 0.00001f));     // D3DRS_DEPTHBIAS
            return D3D_OK;
        }
        if (ignoredRenderState(state))
        {
            return D3D_OK;
        }
        if (state == D3DRENDERSTATE_COLORKEYENABLE)
        {
            if (value)
            {
                unsupported("D3DRENDERSTATE_COLORKEYENABLE (color keyed textures)");
            }
            return D3D_OK;
        }
        unsupported(std::format("render state {}", state).c_str());
        return D3D_OK;
    }

    HRESULT Device::applyStageState(DWORD stage, DWORD type, DWORD value)
    {
        switch (type)
        {
        case D3DTSS_ADDRESS:
            setSamplerState9(stage, d9::D3DSAMP_ADDRESSU, value);
            setSamplerState9(stage, d9::D3DSAMP_ADDRESSV, value);
            break;
        case D3DTSS_ADDRESSU: setSamplerState9(stage, d9::D3DSAMP_ADDRESSU, value); break;
        case D3DTSS_ADDRESSV: setSamplerState9(stage, d9::D3DSAMP_ADDRESSV, value); break;
        case D3DTSS_BORDERCOLOR: setSamplerState9(stage, d9::D3DSAMP_BORDERCOLOR, value); break;
        case D3DTSS_MAGFILTER:
            // D3DTFG_POINT, LINEAR, FLATCUBIC, GAUSSIANCUBIC, ANISOTROPIC
            setSamplerState9(stage, d9::D3DSAMP_MAGFILTER, value == D3DTFG_POINT ? d9::D3DTEXF_POINT
                : value == D3DTFG_ANISOTROPIC ? d9::D3DTEXF_ANISOTROPIC : d9::D3DTEXF_LINEAR);
            break;
        case D3DTSS_MINFILTER:
            // D3DTFN_POINT, LINEAR, ANISOTROPIC
            setSamplerState9(stage, d9::D3DSAMP_MINFILTER, value == D3DTFN_POINT ? d9::D3DTEXF_POINT
                : value == D3DTFN_ANISOTROPIC ? d9::D3DTEXF_ANISOTROPIC : d9::D3DTEXF_LINEAR);
            break;
        case D3DTSS_MIPFILTER:
            // D3DTFP_NONE, POINT, LINEAR
            setSamplerState9(stage, d9::D3DSAMP_MIPFILTER, value == D3DTFP_POINT ? d9::D3DTEXF_POINT
                : value == D3DTFP_LINEAR ? d9::D3DTEXF_LINEAR : d9::D3DTEXF_NONE);
            break;
        case D3DTSS_MIPMAPLODBIAS: setSamplerState9(stage, d9::D3DSAMP_MIPMAPLODBIAS, value); break;
        case D3DTSS_MAXMIPLEVEL: setSamplerState9(stage, d9::D3DSAMP_MAXMIPLEVEL, value); break;
        case D3DTSS_MAXANISOTROPY: setSamplerState9(stage, d9::D3DSAMP_MAXANISOTROPY, std::max<DWORD>(value, 1)); break;
        default:
            // COLOROP .. TEXCOORDINDEX (1-11), BUMPENVLSCALE, BUMPENVLOFFSET, TEXTURETRANSFORMFLAGS (22-24)
            if ((type >= D3DTSS_COLOROP && type <= D3DTSS_TEXCOORDINDEX) ||
                (type >= D3DTSS_BUMPENVLSCALE && type <= D3DTSS_TEXTURETRANSFORMFLAGS))
            {
                setStageState9(stage, type, value);
            }
            else
            {
                unsupported(std::format("texture stage state {}", type).c_str());
            }
            break;
        }
        return D3D_OK;
    }

    HRESULT Device::applyTransform(DWORD type, const D3DMATRIX& m)
    {
        d9::D3DTRANSFORMSTATETYPE type9;
        if (!transform9(type, type9))
        {
            return D3D_OK;
        }
        m_dev->SetTransform(type9, &m);
        return D3D_OK;
    }

    void Device::syncDepth()
    {
        // A z-buffer attached after the device was created.
        if (m_target && m_target->attachedDepth() != m_depth)
        {
            m_depth = m_target->attachedDepth();
            m_dev->SetDepthStencilSurface(m_depth ? m_depth->depthStencil() : nullptr);
        }
    }

    void Device::applyFixedFunction()
    {
        for (uint32_t dirty = m_transformsDirty; dirty; dirty &= dirty - 1)
        {
            unsigned long type = 0;
            _BitScanForward(&type, dirty);
            applyTransform(type, m_transforms[type]);
        }
        m_transformsDirty = 0;
        if (m_materialDirty)
        {
            m_materialDirty = false;
            m_dev->SetMaterial(reinterpret_cast<const d9::D3DMATERIAL9*>(&m_material));
        }
        if (m_lightsDirty)
        {
            m_lightsDirty = false;
            for (DWORD index = 0; index < m_lights.size(); ++index)
            {
                Light& slot = m_lights[index];
                if (slot.lightDirty)
                {
                    slot.lightDirty = false;
                    const D3DLIGHT7& light = slot.light;
                    const HRESULT hr = m_dev->SetLight(index, reinterpret_cast<const d9::D3DLIGHT9*>(&light));
                    if (FAILED(hr))
                    {
                        logFailure(std::format("SetLight (type {}, range {}, attenuation {} {} {})",
                            static_cast<int>(light.dltType), light.dvRange, light.dvAttenuation0, light.dvAttenuation1,
                            light.dvAttenuation2).c_str(), hr);
                    }
                }
                if (slot.enableDirty)
                {
                    slot.enableDirty = false;
                    m_dev->LightEnable(index, slot.enabled);
                }
            }
        }
    }

    void Device::prepare(DWORD fvf)
    {
        prepareTarget();
        applyFixedFunction();
        if (m_skinBound)
        {
            // Back from the skinning shader to the fixed-function pipeline.
            m_skinBound = false;
            m_dev->SetVertexShader(nullptr);
            m_dev->SetStreamSource(1, nullptr, 0, 0);
            m_fvf9 = 0;
        }
        if (fvf != m_fvf9)
        {
            m_fvf9 = fvf;
            m_dev->SetFVF(fvf);
        }
    }

    void Device::prepareTarget()
    {
        syncDepth();
        for (DWORD stage = 0; stage <= m_maxStage; ++stage)
        {
            Surface* texture = m_textures[stage];
            if (texture && texture->uploadPending())
            {
                d9::IDirect3DBaseTexture9* texture9 = texture->texture();
                if (texture9 != m_textures9[stage])
                {
                    m_textures9[stage] = texture9;
                    m_dev->SetTexture(stage, texture9);
                }
            }
        }
    }

    void Device::bindVertexBuffer(d9::IDirect3DVertexBuffer9* vb, UINT stride)
    {
        if (vb != m_vb9 || stride != m_vbStride9)
        {
            m_vb9 = vb;
            m_vbStride9 = stride;
            m_dev->SetStreamSource(0, vb, 0, stride);
        }
    }

    int Device::uploadIndices(const WORD* indices, DWORD count)
    {
        if (!m_indexBuffer || count > m_indexCapacity)
        {
            if (m_indexBuffer)
            {
                m_indexBuffer->Release();
                m_indexBuffer = nullptr;
                m_ib9Bound = nullptr;
            }
            UINT capacity = 1u << 17;
            while (capacity < count)
            {
                capacity *= 2;
            }
            const HRESULT hr = m_dev->CreateIndexBuffer(capacity * sizeof(WORD), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                d9::D3DFMT_INDEX16, d9::D3DPOOL_DEFAULT, &m_indexBuffer, nullptr);
            if (FAILED(hr))
            {
                logFailure("CreateIndexBuffer", hr);
                m_indexBuffer = nullptr;
                return -1;
            }
            m_indexCapacity = capacity;
            m_indexCursor = capacity;   // the first lock discards
        }
        DWORD flags = D3DLOCK_NOOVERWRITE;
        if (m_indexCursor + count > m_indexCapacity)
        {
            m_indexCursor = 0;
            flags = D3DLOCK_DISCARD;
        }
        void* data = nullptr;
        if (FAILED(m_indexBuffer->Lock(m_indexCursor * sizeof(WORD), count * sizeof(WORD), &data, flags)))
        {
            return -1;
        }
        std::memcpy(data, indices, count * sizeof(WORD));
        m_indexBuffer->Unlock();
        const int first = static_cast<int>(m_indexCursor);
        m_indexCursor += count;
        return first;
    }

    bool Device::gather(DWORD fvf, const D3DDRAWPRIMITIVESTRIDEDDATA& data, DWORD count)
    {
        struct Component
        {
            const D3DDP_PTRSTRIDE* source;
            UINT bytes;
        };
        Component components[4 + 8];
        UINT n = 0;
        UINT positionBytes = 12;
        switch (fvf & D3DFVF_POSITION_MASK)
        {
        case D3DFVF_XYZRHW: positionBytes = 16; break;
        case D3DFVF_XYZB1: positionBytes = 16; break;
        case D3DFVF_XYZB2: positionBytes = 20; break;
        case D3DFVF_XYZB3: positionBytes = 24; break;
        case D3DFVF_XYZB4: positionBytes = 28; break;
        case D3DFVF_XYZB5: positionBytes = 32; break;
        default: break;
        }
        static const D3DDP_PTRSTRIDE none = {};
        components[n++] = {&data.position, positionBytes};
        if (fvf & D3DFVF_NORMAL) components[n++] = {&data.normal, 12};
        if (fvf & D3DFVF_RESERVED1) components[n++] = {&none, 4};
        if (fvf & D3DFVF_DIFFUSE) components[n++] = {&data.diffuse, 4};
        if (fvf & D3DFVF_SPECULAR) components[n++] = {&data.specular, 4};
        const UINT sets = std::min<UINT>(Fvf::texCount(fvf), 8);
        for (UINT set = 0; set < sets; ++set)
        {
            components[n++] = {&data.textureCoords[set], Fvf::texCoordSize(fvf, set) * 4};
        }
        const UINT stride = Fvf::stride(fvf);
        m_scratch.resize(size_t(stride) * count);
        uint8_t* out = m_scratch.data();
        UINT offset = 0;
        for (UINT c = 0; c < n; ++c)
        {
            const D3DDP_PTRSTRIDE& src = *components[c].source;
            const UINT bytes = components[c].bytes;
            for (DWORD i = 0; i < count; ++i)
            {
                uint8_t* dst = out + size_t(i) * stride + offset;
                if (src.lpvData)
                {
                    std::memcpy(dst, static_cast<const uint8_t*>(src.lpvData) + size_t(i) * src.dwStride, bytes);
                }
                else
                {
                    std::memset(dst, 0, bytes);
                }
            }
            offset += bytes;
        }
        return offset == stride;
    }

    // ------------------------------------------------------------------------------------------------------------
    // IUnknown

    HRESULT Device::QueryInterface(REFIID riid, LPVOID* out)
    {
        if (!out)
        {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IDirect3DDevice7)
        {
            *out = static_cast<IDirect3DDevice7*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        unsupported("IDirect3DDevice7::QueryInterface for an interface other than IDirect3DDevice7");
        return E_NOINTERFACE;
    }

    ULONG Device::AddRef()
    {
        return static_cast<ULONG>(InterlockedIncrement(&m_refs));
    }

    ULONG Device::Release()
    {
        const LONG refs = InterlockedDecrement(&m_refs);
        if (refs == 0)
        {
            delete this;
        }
        return static_cast<ULONG>(refs);
    }

    // ------------------------------------------------------------------------------------------------------------
    // IDirect3DDevice7

    HRESULT Device::GetCaps(LPD3DDEVICEDESC7 desc)
    {
        if (!desc)
        {
            return DDERR_INVALIDPARAMS;
        }
        *desc = deviceDescription(IID_IDirect3DTnLHalDevice);
        return D3D_OK;
    }

    HRESULT Device::EnumTextureFormats(LPD3DENUMPIXELFORMATSCALLBACK callback, LPVOID context)
    {
        if (!callback)
        {
            return DDERR_INVALIDPARAMS;
        }
        // The game's callbacks keep the last match for a bit depth: 565 after 555 for 16-bit textures without alpha.
        for (d9::D3DFORMAT format : {d9::D3DFMT_A8R8G8B8, d9::D3DFMT_X8R8G8B8, d9::D3DFMT_X1R5G5B5, d9::D3DFMT_A1R5G5B5,
                 d9::D3DFMT_A4R4G4B4, d9::D3DFMT_R5G6B5})
        {
            if (!Gpu::textureFormat(format))
            {
                continue;
            }
            DDPIXELFORMAT pf = Format::toPixelFormat(format);
            if (callback(&pf, context) == D3DENUMRET_CANCEL)
            {
                break;
            }
        }
        return D3D_OK;
    }

    HRESULT Device::BeginScene()
    {
        std::scoped_lock lock(m_lock);
        if (m_inScene)
        {
            return D3DERR_SCENE_IN_SCENE;
        }
        // Never fails for Direct3D 9's reasons: the game's dxDriver7_beginScene retries until it succeeds.
        const HRESULT hr = m_dev->BeginScene();
        if (FAILED(hr))
        {
            logFailure("BeginScene", hr);
        }
        m_inScene = true;
        return D3D_OK;
    }

    HRESULT Device::EndScene()
    {
        std::scoped_lock lock(m_lock);
        if (!m_inScene)
        {
            return D3DERR_SCENE_NOT_IN_SCENE;
        }
        m_inScene = false;
        const HRESULT hr = m_dev->EndScene();
        if (FAILED(hr))
        {
            logFailure("EndScene", hr);
            return D3DERR_SCENE_END_FAILED;
        }
        return D3D_OK;
    }

    HRESULT Device::GetDirect3D(LPDIRECT3D7* out)
    {
        if (!out)
        {
            return DDERR_INVALIDPARAMS;
        }
        *out = static_cast<IDirect3D7*>(m_ddraw);
        m_ddraw->AddRef();
        return D3D_OK;
    }

    HRESULT Device::SetRenderTarget(LPDIRECTDRAWSURFACE7 surface, DWORD)
    {
        std::scoped_lock lock(m_lock);
        return bindTarget(Surface::from(surface));
    }

    HRESULT Device::GetRenderTarget(LPDIRECTDRAWSURFACE7* out)
    {
        if (!out)
        {
            return DDERR_INVALIDPARAMS;
        }
        std::scoped_lock lock(m_lock);
        *out = m_target;
        if (m_target)
        {
            m_target->AddRef();
        }
        return D3D_OK;
    }

    HRESULT Device::Clear(DWORD count, LPD3DRECT rects, DWORD flags, D3DCOLOR color, D3DVALUE z, DWORD stencil)
    {
        std::scoped_lock lock(m_lock);
        syncDepth();
        if (!m_depth)
        {
            flags &= ~(D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL);
        }
        else if (!Format::stencil(m_depth->format()))
        {
            flags &= ~D3DCLEAR_STENCIL;
        }
        if (!flags)
        {
            return D3D_OK;
        }
        if (!rects)
        {
            count = 0;
        }
        const HRESULT hr = m_dev->Clear(count, rects, flags, color, z, stencil);
        if (FAILED(hr))
        {
            logFailure("Clear", hr);
        }
        return D3D_OK;
    }

    HRESULT Device::SetTransform(D3DTRANSFORMSTATETYPE type, LPD3DMATRIX m)
    {
        const DWORD i = static_cast<DWORD>(type);
        if (!m || i >= kTransforms)
        {
            return DDERR_INVALIDPARAMS;
        }
        std::scoped_lock lock(m_lock);
        if (m_recording)
        {
            m_recording->transforms.emplace_back(i, *m);
            return D3D_OK;
        }
        m_transforms[i] = *m;
        m_transformsDirty |= 1u << i;
        return D3D_OK;
    }

    HRESULT Device::GetTransform(D3DTRANSFORMSTATETYPE type, LPD3DMATRIX m)
    {
        const DWORD i = static_cast<DWORD>(type);
        if (!m || i >= kTransforms)
        {
            return DDERR_INVALIDPARAMS;
        }
        std::scoped_lock lock(m_lock);
        *m = m_transforms[i];
        return D3D_OK;
    }

    HRESULT Device::MultiplyTransform(D3DTRANSFORMSTATETYPE type, LPD3DMATRIX m)
    {
        const DWORD i = static_cast<DWORD>(type);
        if (!m || i >= kTransforms)
        {
            return DDERR_INVALIDPARAMS;
        }
        std::scoped_lock lock(m_lock);
        D3DMATRIX product = multiply(*m, m_transforms[i]);
        return SetTransform(type, &product);
    }

    HRESULT Device::SetViewport(LPD3DVIEWPORT7 vp)
    {
        if (!vp)
        {
            return DDERR_INVALIDPARAMS;
        }
        std::scoped_lock lock(m_lock);
        if (m_recording)
        {
            m_recording->viewport = *vp;
            return D3D_OK;
        }
        m_viewport = *vp;
        // Direct3D 9 rejects viewports reaching outside the render target.
        d9::D3DVIEWPORT9 v = {vp->dwX, vp->dwY, vp->dwWidth, vp->dwHeight, vp->dvMinZ, vp->dvMaxZ};
        if (m_target)
        {
            const DWORD w = m_target->width(), h = m_target->height();
            v.X = std::min<DWORD>(v.X, w - 1);
            v.Y = std::min<DWORD>(v.Y, h - 1);
            v.Width = std::max<DWORD>(1, std::min<DWORD>(v.Width, w - v.X));
            v.Height = std::max<DWORD>(1, std::min<DWORD>(v.Height, h - v.Y));
        }
        if (std::memcmp(&v, &m_viewport9, sizeof(v)) != 0)
        {
            m_viewport9 = v;
            const HRESULT hr = m_dev->SetViewport(&v);
            if (FAILED(hr))
            {
                logFailure("SetViewport", hr);
            }
        }
        return D3D_OK;
    }

    HRESULT Device::GetViewport(LPD3DVIEWPORT7 vp)
    {
        if (!vp)
        {
            return DDERR_INVALIDPARAMS;
        }
        std::scoped_lock lock(m_lock);
        *vp = m_viewport;
        return D3D_OK;
    }

    HRESULT Device::SetMaterial(LPD3DMATERIAL7 material)
    {
        if (!material)
        {
            return DDERR_INVALIDPARAMS;
        }
        std::scoped_lock lock(m_lock);
        if (m_recording)
        {
            m_recording->material = *material;
            return D3D_OK;
        }
        m_material = *material;
        m_materialDirty = true;
        return D3D_OK;
    }

    HRESULT Device::GetMaterial(LPD3DMATERIAL7 material)
    {
        if (!material)
        {
            return DDERR_INVALIDPARAMS;
        }
        std::scoped_lock lock(m_lock);
        *material = m_material;
        return D3D_OK;
    }

    HRESULT Device::SetLight(DWORD index, LPD3DLIGHT7 light)
    {
        if (!light || index >= 4096)
        {
            return DDERR_INVALIDPARAMS;
        }
        std::scoped_lock lock(m_lock);
        if (m_recording)
        {
            m_recording->lights.emplace_back(index, *light);
            return D3D_OK;
        }
        if (index >= m_lights.size())
        {
            m_lights.resize(index + 1);
        }
        m_lights[index].light = *light;
        m_lights[index].set = true;
        m_lights[index].lightDirty = true;
        m_lightsDirty = true;
        return D3D_OK;
    }

    HRESULT Device::GetLight(DWORD index, LPD3DLIGHT7 light)
    {
        if (!light)
        {
            return DDERR_INVALIDPARAMS;
        }
        std::scoped_lock lock(m_lock);
        if (index >= m_lights.size() || !m_lights[index].set)
        {
            return DDERR_INVALIDPARAMS;
        }
        *light = m_lights[index].light;
        return D3D_OK;
    }

    HRESULT Device::LightEnable(DWORD index, BOOL enable)
    {
        if (index >= 4096)
        {
            return DDERR_INVALIDPARAMS;
        }
        std::scoped_lock lock(m_lock);
        if (m_recording)
        {
            m_recording->lightEnables.emplace_back(index, enable);
            return D3D_OK;
        }
        if (index >= m_lights.size())
        {
            m_lights.resize(index + 1);
        }
        Light& slot = m_lights[index];
        if (!slot.set)
        {
            // Enabling a light that was never set creates the default one: white, directional, along +Z.
            slot.light = {};
            slot.light.dltType = D3DLIGHT_DIRECTIONAL;
            slot.light.dcvDiffuse = {1.0f, 1.0f, 1.0f, 0.0f};
            slot.light.dvDirection = {0.0f, 0.0f, 1.0f};
            slot.set = true;
        }
        slot.enabled = enable;
        slot.enableDirty = true;
        m_lightsDirty = true;
        return D3D_OK;
    }

    HRESULT Device::GetLightEnable(DWORD index, BOOL* enable)
    {
        if (!enable)
        {
            return DDERR_INVALIDPARAMS;
        }
        std::scoped_lock lock(m_lock);
        if (index >= m_lights.size() || !m_lights[index].set)
        {
            return DDERR_INVALIDPARAMS;
        }
        *enable = m_lights[index].enabled;
        return D3D_OK;
    }

    HRESULT Device::SetRenderState(D3DRENDERSTATETYPE state, DWORD value)
    {
        const DWORD i = static_cast<DWORD>(state);
        if (i >= kRenderStates)
        {
            return DDERR_INVALIDPARAMS;
        }
        std::scoped_lock lock(m_lock);
        if (m_recording)
        {
            m_recording->renderStates.emplace_back(i, value);
            return D3D_OK;
        }
        m_rs[i] = value;
        return applyRenderState(i, value);
    }

    HRESULT Device::GetRenderState(D3DRENDERSTATETYPE state, LPDWORD value)
    {
        const DWORD i = static_cast<DWORD>(state);
        if (!value || i >= kRenderStates)
        {
            return DDERR_INVALIDPARAMS;
        }
        std::scoped_lock lock(m_lock);
        *value = m_rs[i];
        return D3D_OK;
    }

    HRESULT Device::PreLoad(LPDIRECTDRAWSURFACE7 texture)
    {
        std::scoped_lock lock(m_lock);
        if (Surface* surface = Surface::from(texture))
        {
            surface->texture();
        }
        return D3D_OK;
    }

    HRESULT Device::DrawPrimitive(D3DPRIMITIVETYPE type, DWORD fvf, LPVOID verts, DWORD count, DWORD)
    {
        const DWORD prims = primitiveCount(type, count);
        if (!verts)
        {
            return DDERR_INVALIDPARAMS;
        }
        if (!prims)
        {
            return D3D_OK;
        }
        std::scoped_lock lock(m_lock);
        prepare(fvf);
        const HRESULT hr = m_dev->DrawPrimitiveUP(primitive9(type), prims, verts, Fvf::stride(fvf));
        m_vb9 = nullptr;    // user pointer draws unbind stream 0
        if (FAILED(hr))
        {
            logFailure("DrawPrimitiveUP", hr);
        }
        return D3D_OK;
    }

    HRESULT Device::DrawIndexedPrimitive(D3DPRIMITIVETYPE type, DWORD fvf, LPVOID verts, DWORD vertCount, LPWORD indices,
        DWORD indexCount, DWORD)
    {
        const DWORD prims = primitiveCount(type, indexCount);
        if (!verts || !indices)
        {
            return DDERR_INVALIDPARAMS;
        }
        if (!prims || !vertCount)
        {
            return D3D_OK;
        }
        std::scoped_lock lock(m_lock);
        prepare(fvf);
        const HRESULT hr = m_dev->DrawIndexedPrimitiveUP(primitive9(type), 0, vertCount, prims, indices, d9::D3DFMT_INDEX16,
            verts, Fvf::stride(fvf));
        m_vb9 = nullptr;
        m_ib9Bound = nullptr;
        if (FAILED(hr))
        {
            logFailure("DrawIndexedPrimitiveUP", hr);
        }
        return D3D_OK;
    }

    HRESULT Device::SetClipStatus(LPD3DCLIPSTATUS status)
    {
        if (!status)
        {
            return DDERR_INVALIDPARAMS;
        }
        std::scoped_lock lock(m_lock);
        m_clipStatus = *status;
        return D3D_OK;
    }

    HRESULT Device::GetClipStatus(LPD3DCLIPSTATUS status)
    {
        if (!status)
        {
            return DDERR_INVALIDPARAMS;
        }
        std::scoped_lock lock(m_lock);
        *status = m_clipStatus;
        return D3D_OK;
    }

    HRESULT Device::DrawPrimitiveStrided(D3DPRIMITIVETYPE type, DWORD fvf, LPD3DDRAWPRIMITIVESTRIDEDDATA data, DWORD count,
        DWORD flags)
    {
        if (!data)
        {
            return DDERR_INVALIDPARAMS;
        }
        if (!primitiveCount(type, count))
        {
            return D3D_OK;
        }
        std::scoped_lock lock(m_lock);
        if (!gather(fvf, *data, count))
        {
            unsupported("strided draws of this vertex format");
            return DDERR_INVALIDPARAMS;
        }
        return DrawPrimitive(type, fvf, m_scratch.data(), count, flags);
    }

    HRESULT Device::DrawIndexedPrimitiveStrided(D3DPRIMITIVETYPE type, DWORD fvf, LPD3DDRAWPRIMITIVESTRIDEDDATA data,
        DWORD vertCount, LPWORD indices, DWORD indexCount, DWORD flags)
    {
        if (!data || !indices)
        {
            return DDERR_INVALIDPARAMS;
        }
        if (!primitiveCount(type, indexCount) || !vertCount)
        {
            return D3D_OK;
        }
        std::scoped_lock lock(m_lock);
        if (!gather(fvf, *data, vertCount))
        {
            unsupported("strided draws of this vertex format");
            return DDERR_INVALIDPARAMS;
        }
        return DrawIndexedPrimitive(type, fvf, m_scratch.data(), vertCount, indices, indexCount, flags);
    }

    HRESULT Device::DrawPrimitiveVB(D3DPRIMITIVETYPE type, LPDIRECT3DVERTEXBUFFER7 vb, DWORD start, DWORD count, DWORD)
    {
        VertexBuffer* buffer = VertexBuffer::from(vb);
        if (!buffer)
        {
            return DDERR_INVALIDPARAMS;
        }
        const DWORD prims = primitiveCount(type, count);
        if (!prims)
        {
            return D3D_OK;
        }
        std::scoped_lock lock(m_lock);
        prepare(buffer->fvf());
        bindVertexBuffer(buffer->buffer(), buffer->stride());
        const HRESULT hr = m_dev->DrawPrimitive(primitive9(type), start, prims);
        if (FAILED(hr))
        {
            logFailure("DrawPrimitive", hr);
        }
        return D3D_OK;
    }

    HRESULT Device::DrawIndexedPrimitiveVB(D3DPRIMITIVETYPE type, LPDIRECT3DVERTEXBUFFER7 vb, DWORD start, DWORD vertCount,
        LPWORD indices, DWORD indexCount, DWORD)
    {
        VertexBuffer* buffer = VertexBuffer::from(vb);
        if (!buffer || !indices)
        {
            return DDERR_INVALIDPARAMS;
        }
        const DWORD prims = primitiveCount(type, indexCount);
        if (!prims || !vertCount)
        {
            return D3D_OK;
        }
        std::scoped_lock lock(m_lock);
        const int first = uploadIndices(indices, indexCount);
        if (first < 0)
        {
            return DDERR_GENERIC;
        }
        prepare(buffer->fvf());
        bindVertexBuffer(buffer->buffer(), buffer->stride());
        if (m_ib9Bound != m_indexBuffer)
        {
            m_ib9Bound = m_indexBuffer;
            m_dev->SetIndices(m_indexBuffer);
        }
        const HRESULT hr = m_dev->DrawIndexedPrimitive(primitive9(type), static_cast<INT>(start), 0, vertCount,
            static_cast<UINT>(first), prims);
        if (FAILED(hr))
        {
            logFailure("DrawIndexedPrimitive", hr);
        }
        return D3D_OK;
    }

    HRESULT Device::ComputeSphereVisibility(LPD3DVECTOR, LPD3DVALUE, DWORD count, DWORD, LPDWORD result)
    {
        unsupported("IDirect3DDevice7::ComputeSphereVisibility (every sphere is reported visible)");
        if (result)
        {
            std::fill_n(result, count, 0);
        }
        return D3D_OK;
    }

    HRESULT Device::GetTexture(DWORD stage, LPDIRECTDRAWSURFACE7* out)
    {
        if (!out || stage >= kStages)
        {
            return DDERR_INVALIDPARAMS;
        }
        std::scoped_lock lock(m_lock);
        *out = m_textures[stage];
        if (m_textures[stage])
        {
            m_textures[stage]->AddRef();
        }
        return D3D_OK;
    }

    HRESULT Device::SetTexture(DWORD stage, LPDIRECTDRAWSURFACE7 texture)
    {
        if (stage >= kStages)
        {
            return DDERR_INVALIDPARAMS;
        }
        Surface* surface = Surface::from(texture);
        if (texture && !surface)
        {
            unsupported("SetTexture with a surface of another DirectDraw implementation");
            return DDERR_INVALIDPARAMS;
        }
        std::scoped_lock lock(m_lock);
        if (m_recording)
        {
            if (surface)
            {
                surface->AddRef();
            }
            m_recording->textures.emplace_back(stage, surface);
            return D3D_OK;
        }
        if (surface != m_textures[stage])
        {
            if (surface)
            {
                surface->AddRef();
            }
            if (m_textures[stage])
            {
                m_textures[stage]->Release();
            }
            m_textures[stage] = surface;
            if (surface && stage > m_maxStage)
            {
                m_maxStage = stage;
            }
        }
        d9::IDirect3DBaseTexture9* texture9 = surface ? surface->texture() : nullptr;
        if (texture9 != m_textures9[stage])
        {
            m_textures9[stage] = texture9;
            m_dev->SetTexture(stage, texture9);
        }
        return D3D_OK;
    }

    HRESULT Device::GetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, LPDWORD value)
    {
        DWORD t = static_cast<DWORD>(type);
        if (!value || stage >= kStages || t >= kStageStates)
        {
            return DDERR_INVALIDPARAMS;
        }
        if (t == D3DTSS_ADDRESS)
        {
            t = D3DTSS_ADDRESSU;
        }
        std::scoped_lock lock(m_lock);
        *value = m_tss[stage][t];
        return D3D_OK;
    }

    HRESULT Device::SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
    {
        const DWORD t = static_cast<DWORD>(type);
        if (stage >= kStages || t >= kStageStates)
        {
            return DDERR_INVALIDPARAMS;
        }
        std::scoped_lock lock(m_lock);
        if (m_recording)
        {
            m_recording->stageStates.push_back({stage, t, value});
            return D3D_OK;
        }
        m_tss[stage][t] = value;
        if (t == D3DTSS_ADDRESS)
        {
            m_tss[stage][D3DTSS_ADDRESSU] = m_tss[stage][D3DTSS_ADDRESSV] = value;
        }
        return applyStageState(stage, t, value);
    }

    HRESULT Device::ValidateDevice(LPDWORD passes)
    {
        std::scoped_lock lock(m_lock);
        prepare(m_fvf9);
        DWORD n = 0;
        const HRESULT hr = m_dev->ValidateDevice(&n);
        if (passes)
        {
            *passes = n;
        }
        return hr;
    }

    // ------------------------------------------------------------------------------------------------------------
    // State blocks

    Device::StateBlock* Device::stateBlock(DWORD handle)
    {
        return handle && handle <= m_stateBlocks.size() ? m_stateBlocks[handle - 1].get() : nullptr;
    }

    void Device::capture(StateBlock& block, D3DSTATEBLOCKTYPE type) const
    {
        const bool all = type == D3DSBT_ALL;
        const bool pixel = all || type == D3DSBT_PIXELSTATE;
        const bool vertex = all || type == D3DSBT_VERTEXSTATE;
        for (DWORD state = 1; state < kRenderStates; ++state)
        {
            block.renderStates.emplace_back(state, m_rs[state]);
        }
        if (pixel)
        {
            for (DWORD stage = 0; stage < kStages; ++stage)
            {
                for (DWORD t = 1; t <= D3DTSS_TEXTURETRANSFORMFLAGS; ++t)
                {
                    if (t != D3DTSS_ADDRESS)
                    {
                        block.stageStates.push_back({stage, t, m_tss[stage][t]});
                    }
                }
            }
        }
        if (all)
        {
            for (DWORD stage = 0; stage < kStages; ++stage)
            {
                if (m_textures[stage])
                {
                    m_textures[stage]->AddRef();
                }
                block.textures.emplace_back(stage, m_textures[stage]);
            }
            for (DWORD t = 0; t < kTransforms; ++t)
            {
                block.transforms.emplace_back(t, m_transforms[t]);
            }
            block.viewport = m_viewport;
            for (DWORD plane = 0; plane < kClipPlanes; ++plane)
            {
                block.clipPlanes.push_back({plane, {m_clipPlanes[plane][0], m_clipPlanes[plane][1], m_clipPlanes[plane][2], m_clipPlanes[plane][3]}});
            }
        }
        if (vertex)
        {
            block.material = m_material;
            for (DWORD i = 0; i < m_lights.size(); ++i)
            {
                if (m_lights[i].set)
                {
                    block.lights.emplace_back(i, m_lights[i].light);
                    block.lightEnables.emplace_back(i, m_lights[i].enabled);
                }
            }
        }
    }

    void Device::recapture(StateBlock& block) const
    {
        for (auto& [state, value] : block.renderStates) value = m_rs[state];
        for (auto& s : block.stageStates) s[2] = m_tss[s[0]][s[1]];
        for (auto& [stage, texture] : block.textures)
        {
            if (m_textures[stage]) m_textures[stage]->AddRef();
            if (texture) texture->Release();
            texture = m_textures[stage];
        }
        for (auto& [type, m] : block.transforms) m = m_transforms[type];
        if (block.viewport) block.viewport = m_viewport;
        if (block.material) block.material = m_material;
        for (auto& [index, light] : block.lights)
        {
            if (index < m_lights.size()) light = m_lights[index].light;
        }
        for (auto& [index, enabled] : block.lightEnables)
        {
            if (index < m_lights.size()) enabled = m_lights[index].enabled;
        }
        for (auto& [index, plane] : block.clipPlanes)
        {
            std::copy(std::begin(m_clipPlanes[index]), std::end(m_clipPlanes[index]), plane.begin());
        }
    }

    void Device::apply(const StateBlock& block)
    {
        for (const auto& [state, value] : block.renderStates) SetRenderState(static_cast<D3DRENDERSTATETYPE>(state), value);
        for (const auto& s : block.stageStates) SetTextureStageState(s[0], static_cast<D3DTEXTURESTAGESTATETYPE>(s[1]), s[2]);
        for (const auto& [stage, texture] : block.textures) SetTexture(stage, texture);
        for (auto [type, m] : block.transforms) SetTransform(static_cast<D3DTRANSFORMSTATETYPE>(type), &m);
        if (block.viewport)
        {
            D3DVIEWPORT7 vp = *block.viewport;
            SetViewport(&vp);
        }
        if (block.material)
        {
            D3DMATERIAL7 material = *block.material;
            SetMaterial(&material);
        }
        for (auto [index, light] : block.lights) SetLight(index, &light);
        for (const auto& [index, enabled] : block.lightEnables) LightEnable(index, enabled);
        for (auto [index, plane] : block.clipPlanes) SetClipPlane(index, plane.data());
    }

    HRESULT Device::BeginStateBlock()
    {
        std::scoped_lock lock(m_lock);
        if (m_recording)
        {
            return D3DERR_INBEGINSTATEBLOCK;
        }
        m_recording = std::make_unique<StateBlock>();
        return D3D_OK;
    }

    HRESULT Device::EndStateBlock(LPDWORD handle)
    {
        if (!handle)
        {
            return DDERR_INVALIDPARAMS;
        }
        std::scoped_lock lock(m_lock);
        if (!m_recording)
        {
            return D3DERR_NOTINBEGINSTATEBLOCK;
        }
        m_stateBlocks.push_back(std::move(m_recording));
        *handle = static_cast<DWORD>(m_stateBlocks.size());
        return D3D_OK;
    }

    HRESULT Device::ApplyStateBlock(DWORD handle)
    {
        std::scoped_lock lock(m_lock);
        StateBlock* block = stateBlock(handle);
        if (!block || m_recording)
        {
            return D3DERR_INVALIDSTATEBLOCK;
        }
        apply(*block);
        return D3D_OK;
    }

    HRESULT Device::CaptureStateBlock(DWORD handle)
    {
        std::scoped_lock lock(m_lock);
        StateBlock* block = stateBlock(handle);
        if (!block || m_recording)
        {
            return D3DERR_INVALIDSTATEBLOCK;
        }
        recapture(*block);
        return D3D_OK;
    }

    HRESULT Device::DeleteStateBlock(DWORD handle)
    {
        std::scoped_lock lock(m_lock);
        if (!stateBlock(handle) || m_recording)
        {
            return D3DERR_INVALIDSTATEBLOCK;
        }
        m_stateBlocks[handle - 1].reset();
        return D3D_OK;
    }

    HRESULT Device::CreateStateBlock(D3DSTATEBLOCKTYPE type, LPDWORD handle)
    {
        if (!handle || (type != D3DSBT_ALL && type != D3DSBT_PIXELSTATE && type != D3DSBT_VERTEXSTATE))
        {
            return DDERR_INVALIDPARAMS;
        }
        std::scoped_lock lock(m_lock);
        if (m_recording)
        {
            return D3DERR_INBEGINSTATEBLOCK;
        }
        auto block = std::make_unique<StateBlock>();
        capture(*block, type);
        m_stateBlocks.push_back(std::move(block));
        *handle = static_cast<DWORD>(m_stateBlocks.size());
        return D3D_OK;
    }

    // ------------------------------------------------------------------------------------------------------------
    // The rest

    HRESULT Device::Load(LPDIRECTDRAWSURFACE7 dst, LPPOINT dstPoint, LPDIRECTDRAWSURFACE7 src, LPRECT srcRect, DWORD)
    {
        Surface* to = Surface::from(dst);
        Surface* from = Surface::from(src);
        if (!to || !from)
        {
            return DDERR_INVALIDPARAMS;
        }
        RECT area = srcRect ? *srcRect : RECT{0, 0, static_cast<LONG>(from->width()), static_cast<LONG>(from->height())};
        const POINT origin = dstPoint ? *dstPoint : POINT{0, 0};
        RECT target = {origin.x, origin.y, origin.x + area.right - area.left, origin.y + area.bottom - area.top};
        return to->Blt(&target, from, &area, DDBLT_WAIT, nullptr);
    }

    HRESULT Device::SetClipPlane(DWORD index, D3DVALUE* plane)
    {
        if (!plane || index >= kClipPlanes)
        {
            return DDERR_INVALIDPARAMS;
        }
        std::scoped_lock lock(m_lock);
        if (m_recording)
        {
            m_recording->clipPlanes.push_back({index, {plane[0], plane[1], plane[2], plane[3]}});
            return D3D_OK;
        }
        std::copy(plane, plane + 4, m_clipPlanes[index]);
        m_dev->SetClipPlane(index, plane);
        return D3D_OK;
    }

    HRESULT Device::GetClipPlane(DWORD index, D3DVALUE* plane)
    {
        if (!plane || index >= kClipPlanes)
        {
            return DDERR_INVALIDPARAMS;
        }
        std::scoped_lock lock(m_lock);
        std::copy(std::begin(m_clipPlanes[index]), std::end(m_clipPlanes[index]), plane);
        return D3D_OK;
    }

    HRESULT Device::GetInfo(DWORD, LPVOID, DWORD)
    {
        return S_FALSE;
    }
}
