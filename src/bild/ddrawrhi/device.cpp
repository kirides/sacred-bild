#include "ddrawrhi/device.h"
#include "ddrawrhi/context.h"
#include "ddrawrhi/directdraw.h"
#include "ddrawrhi/surface.h"
#include "ddrawrhi/vertex_buffer.h"
#include "ddrawrhi/world.h"
#include "ddraw9/format.h"
#include "ddraw9/skin.h"
#include "rhi/rhi.h"
#include "render/fvf.h"
#include "log.h"

#include "ddrawrhi/shaders/ff_shaders.inc"

#include <nvrhi/utils.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace DDrawRhi
{
    namespace Format = DDraw9::Format;

    namespace
    {
        const void* g_deviceVtable = nullptr;

        // ff.hlsl's constant buffers, in HLSL's packing.
        struct LightConstants
        {
            float diffuse[4];
            float specular[4];
            float ambient[4];
            float position[4];
            float direction[4];
            float attenuation[4];
            float spot[4];
        };

        struct VertexConstants
        {
            float worldView[16];
            float projection[16];
            float normalMatrix[16];
            float texTransform[2][16];
            float target[4];
            float materialDiffuse[4];
            float materialAmbient[4];
            float materialSpecular[4];
            float materialEmissive[4];
            float ambient[4];
            float power;
            uint32_t flags;
            uint32_t materialSources;
            uint32_t lightCount;
            uint32_t texCoords[4];
            LightConstants lights[8];
            float groundTransform[4];
            float skinColor[4];
        };
        static_assert(sizeof(VertexConstants) == 1376);

        struct BoneConstants
        {
            float rows[DDraw9::Skin::kMaxBones * 3][4];
        };

        struct PixelConstants
        {
            uint32_t stage0[4];
            uint32_t stage0b[4];
            uint32_t stage1[4];
            uint32_t stage1b[4];
            float textureFactor[4];
            uint32_t alphaFunc;
            float alphaRef;
            uint32_t flags;
            uint32_t pad;
        };
        static_assert(sizeof(PixelConstants) == 96);

        enum : uint32_t
        {
            kFlagRhw = 0x001,
            kFlagLighting = 0x002,
            kFlagNormal = 0x004,
            kFlagDiffuse = 0x008,
            kFlagSpecular = 0x010,
            kFlagNormalize = 0x020,
            kFlagLocalViewer = 0x040,
            kFlagSpecularOn = 0x080,
            kFlagColorVertex = 0x100,
            kFlagGround = 0x200,
            kFlagSkin = 0x400,
            kFlagSkinNormalize = 0x800,
            kFlagSkinColor = 0x1000,
        };

        // Not a Direct3D 7 vertex format: the input layout and constants of the ground mode (ground.h).
        constexpr DWORD kGroundFvf = 0x7FFF0000;
        constexpr UINT kGroundStride = 28;
        constexpr uint32_t kQuadsPerDraw = 16384;     // 16-bit indices
        // ... and of the skinning mode (skin.h), without and with a per-vertex diffuse stream (in slot 2).
        constexpr DWORD kSkinFvf = 0x7FFE0000;
        constexpr DWORD kSkinDiffuseFvf = 0x7FFD0000;
        constexpr UINT kSkinStride = 52;
        bool skinKey(DWORD fvf) { return fvf == kSkinFvf || fvf == kSkinDiffuseFvf; }

        // Shared by all devices: the shaders, the binding layout, input layouts by vertex format, pipelines by state.
        struct Shared
        {
            nvrhi::ShaderHandle vs, ps;
            nvrhi::BindingLayoutHandle layout;
            nvrhi::BufferHandle quadIndices;    // 0 1 2 2 1 3 for every 4 vertices (the ground's quads)
            bool quadIndicesWritten = false;
            std::unordered_map<DWORD, nvrhi::InputLayoutHandle> inputLayouts;

            struct PipelineKey
            {
                DWORD fvf;
                uint32_t prim;
                uint32_t blend;         // enable | src << 1 | dst << 6
                uint32_t depth;         // enable | write << 1 | func << 2
                uint32_t raster;        // cull | fill << 2
                uint32_t stencil[3];    // enable | func << 1 | fail << 5 | zfail << 9 | pass << 13; ref; mask | writeMask << 16 (low bytes)
                nvrhi::Format color;
                nvrhi::Format depthFormat;
                bool operator==(const PipelineKey&) const = default;
            };
            struct PipelineKeyHash
            {
                size_t operator()(const PipelineKey& k) const
                {
                    size_t h = k.fvf;
                    for (uint32_t v : {k.prim, k.blend, k.depth, k.raster, k.stencil[0], k.stencil[1], k.stencil[2],
                             static_cast<uint32_t>(k.color), static_cast<uint32_t>(k.depthFormat)})
                    {
                        h = h * 0x9E3779B1u ^ v;
                    }
                    return h;
                }
            };
            std::unordered_map<PipelineKey, nvrhi::GraphicsPipelineHandle, PipelineKeyHash> pipelines;

            struct BindingKey
            {
                const void* device;
                nvrhi::ITexture* textures[2];
                nvrhi::ISampler* samplers[2];
                bool operator==(const BindingKey&) const = default;
            };
            struct BindingKeyHash
            {
                size_t operator()(const BindingKey& k) const
                {
                    size_t h = reinterpret_cast<size_t>(k.device);
                    for (const void* p : {static_cast<const void*>(k.textures[0]), static_cast<const void*>(k.textures[1]),
                             static_cast<const void*>(k.samplers[0]), static_cast<const void*>(k.samplers[1])})
                    {
                        h = h * 0x9E3779B1u ^ reinterpret_cast<size_t>(p);
                    }
                    return h;
                }
            };
            std::unordered_map<BindingKey, nvrhi::BindingSetHandle, BindingKeyHash> bindingSets;
        };

        Shared* g_shared = nullptr;

        // Context::forget: binding sets that hold a texture going away.
        void forgetTexture(nvrhi::ITexture* texture)
        {
            if (!g_shared)
            {
                return;
            }
            auto& sets = g_shared->bindingSets;
            for (auto it = sets.begin(); it != sets.end();)
            {
                it = it->first.textures[0] == texture || it->first.textures[1] == texture ? sets.erase(it) : std::next(it);
            }
        }

        bool initShared()
        {
            if (g_shared)
            {
                return true;
            }
            auto shared = std::make_unique<Shared>();
            shared->vs = Rhi::createShader(nvrhi::ShaderType::Vertex, "vsMain", kFixedVSDxil, kFixedVSSpirv);
            shared->ps = Rhi::createShader(nvrhi::ShaderType::Pixel, "psMain", kFixedPSDxil, kFixedPSSpirv);
            nvrhi::BindingLayoutDesc layout;
            layout.visibility = nvrhi::ShaderType::All;
            layout.bindings = {
                nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
                nvrhi::BindingLayoutItem::VolatileConstantBuffer(1),
                nvrhi::BindingLayoutItem::VolatileConstantBuffer(2),
                nvrhi::BindingLayoutItem::Texture_SRV(0),
                nvrhi::BindingLayoutItem::Texture_SRV(1),
                nvrhi::BindingLayoutItem::Sampler(0),
                nvrhi::BindingLayoutItem::Sampler(1),
            };
            shared->layout = Context::device()->createBindingLayout(layout);
            if (!shared->vs || !shared->ps || !shared->layout)
            {
                LOG("NVRHI backend: the fixed-function shaders could not be created");
                return false;
            }
            g_shared = shared.release();
            Context::onForget(&forgetTexture);
            return true;
        }

        nvrhi::IInputLayout* inputLayout(DWORD fvf)
        {
            auto it = g_shared->inputLayouts.find(fvf);
            if (it != g_shared->inputLayouts.end())
            {
                return it->second;
            }
            const UINT stride = fvf == kGroundFvf ? kGroundStride : skinKey(fvf) ? kSkinStride : Fvf::stride(fvf);
            UINT offset = 0;
            std::vector<nvrhi::VertexAttributeDesc> attrs;
            auto own = [&](const char* name, nvrhi::Format format, UINT at) {
                attrs.push_back(nvrhi::VertexAttributeDesc().setName(name).setFormat(format).setBufferIndex(0)
                    .setOffset(at).setElementStride(stride));
            };
            auto fallback = [&](const char* name, nvrhi::Format format, UINT at) {
                attrs.push_back(nvrhi::VertexAttributeDesc().setName(name).setFormat(format).setBufferIndex(1)
                    .setOffset(at).setElementStride(0));
            };
            auto finish = [&] {
                nvrhi::InputLayoutHandle layout = Context::device()->createInputLayout(attrs.data(), static_cast<uint32_t>(attrs.size()));
                g_shared->inputLayouts.emplace(fvf, layout);
                return layout;
            };
            if (skinKey(fvf))
            {
                own("POSITION", nvrhi::Format::RGB32_FLOAT, 0);
                own("NORMAL", nvrhi::Format::RGB32_FLOAT, 12);
                if (fvf == kSkinDiffuseFvf)
                {
                    attrs.push_back(nvrhi::VertexAttributeDesc().setName("DIFFUSE").setFormat(nvrhi::Format::R32_UINT)
                        .setBufferIndex(2).setOffset(0).setElementStride(4));
                }
                else
                {
                    fallback("DIFFUSE", nvrhi::Format::R32_UINT, 12);
                }
                fallback("SPECULAR", nvrhi::Format::R32_UINT, 16);
                own("TEXA", nvrhi::Format::RG32_FLOAT, 24);
                fallback("TEXB", nvrhi::Format::RGBA32_FLOAT, 20);
                fallback("TEXC", nvrhi::Format::RGBA32_FLOAT, 20);
                fallback("TEXD", nvrhi::Format::RGBA32_FLOAT, 20);
                own("BONES", nvrhi::Format::R32_UINT, 32);
                own("WEIGHTS", nvrhi::Format::RGBA32_FLOAT, 36);
                return finish();
            }
            if (fvf == kGroundFvf)
            {
                own("POSITION", nvrhi::Format::RG32_FLOAT, 0);
                fallback("NORMAL", nvrhi::Format::RGB32_FLOAT, 0);
                own("DIFFUSE", nvrhi::Format::R32_UINT, 8);
                fallback("SPECULAR", nvrhi::Format::R32_UINT, 16);
                own("TEXA", nvrhi::Format::RG32_FLOAT, 12);
                own("TEXB", nvrhi::Format::RG32_FLOAT, 20);
                fallback("TEXC", nvrhi::Format::RGBA32_FLOAT, 20);
                fallback("TEXD", nvrhi::Format::RGBA32_FLOAT, 20);
                fallback("BONES", nvrhi::Format::R32_UINT, 36);
                fallback("WEIGHTS", nvrhi::Format::RGBA32_FLOAT, 40);
                return finish();
            }
            switch (fvf & D3DFVF_POSITION_MASK)
            {
            case D3DFVF_XYZRHW: own("POSITION", nvrhi::Format::RGBA32_FLOAT, 0); offset = 16; break;
            case D3DFVF_XYZB1: own("POSITION", nvrhi::Format::RGB32_FLOAT, 0); offset = 16; break;
            case D3DFVF_XYZB2: own("POSITION", nvrhi::Format::RGB32_FLOAT, 0); offset = 20; break;
            case D3DFVF_XYZB3: own("POSITION", nvrhi::Format::RGB32_FLOAT, 0); offset = 24; break;
            case D3DFVF_XYZB4: own("POSITION", nvrhi::Format::RGB32_FLOAT, 0); offset = 28; break;
            case D3DFVF_XYZB5: own("POSITION", nvrhi::Format::RGB32_FLOAT, 0); offset = 32; break;
            default: own("POSITION", nvrhi::Format::RGB32_FLOAT, 0); offset = 12; break;
            }
            if (fvf & D3DFVF_NORMAL)
            {
                own("NORMAL", nvrhi::Format::RGB32_FLOAT, offset);
                offset += 12;
            }
            else
            {
                fallback("NORMAL", nvrhi::Format::RGB32_FLOAT, 0);
            }
            if (fvf & D3DFVF_RESERVED1)
            {
                offset += 4;
            }
            if (fvf & D3DFVF_DIFFUSE)
            {
                own("DIFFUSE", nvrhi::Format::R32_UINT, offset);
                offset += 4;
            }
            else
            {
                fallback("DIFFUSE", nvrhi::Format::R32_UINT, 12);
            }
            if (fvf & D3DFVF_SPECULAR)
            {
                own("SPECULAR", nvrhi::Format::R32_UINT, offset);
                offset += 4;
            }
            else
            {
                fallback("SPECULAR", nvrhi::Format::R32_UINT, 16);
            }
            static const char* kTex[4] = {"TEXA", "TEXB", "TEXC", "TEXD"};
            static const nvrhi::Format kTexFormats[5] = {nvrhi::Format::UNKNOWN, nvrhi::Format::R32_FLOAT,
                nvrhi::Format::RG32_FLOAT, nvrhi::Format::RGB32_FLOAT, nvrhi::Format::RGBA32_FLOAT};
            const UINT sets = Fvf::texCount(fvf);
            for (UINT set = 0; set < 4; ++set)
            {
                if (set < sets)
                {
                    own(kTex[set], kTexFormats[Fvf::texCoordSize(fvf, set)], Fvf::texCoordOffset(fvf, set));
                }
                else
                {
                    fallback(kTex[set], nvrhi::Format::RGBA32_FLOAT, 20);
                }
            }
            fallback("BONES", nvrhi::Format::R32_UINT, 36);
            fallback("WEIGHTS", nvrhi::Format::RGBA32_FLOAT, 40);
            return finish();
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

        // The inverse transpose of the upper 3x3 (normals in camera space), as a 4x4 with no translation.
        void normalMatrix(const D3DMATRIX& m, float out[16])
        {
            const float a = m._11, b = m._12, c = m._13, d = m._21, e = m._22, f = m._23, g = m._31, h = m._32, i = m._33;
            const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
            const float det = a * A + b * B + c * C;
            const float k = std::fabs(det) > 1e-20f ? 1.0f / det : 1.0f;
            // inverse = adjugate / det; transposed, so row r of the result is column r of the inverse.
            const float inv[9] = {A * k, -(b * i - c * h) * k, (b * f - c * e) * k,
                B * k, (a * i - c * g) * k, -(a * f - c * d) * k,
                C * k, -(a * h - b * g) * k, (a * e - b * d) * k};
            // out = transpose(inverse): out[r][c] = inv[c][r]
            std::memset(out, 0, 16 * sizeof(float));
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 3; ++col)
                    out[row * 4 + col] = inv[col * 3 + row];
            out[15] = 1.0f;
        }

        void colorValue(const D3DCOLORVALUE& c, float out[4])
        {
            out[0] = c.r;
            out[1] = c.g;
            out[2] = c.b;
            out[3] = c.a;
        }

        void colorDword(DWORD c, float out[4])
        {
            out[0] = ((c >> 16) & 255) / 255.0f;
            out[1] = ((c >> 8) & 255) / 255.0f;
            out[2] = (c & 255) / 255.0f;
            out[3] = (c >> 24) / 255.0f;
        }

        nvrhi::BlendFactor blendFactor(DWORD b, bool targetHasAlpha)
        {
            switch (b)
            {
            case D3DBLEND_ZERO: return nvrhi::BlendFactor::Zero;
            case D3DBLEND_ONE: return nvrhi::BlendFactor::One;
            case D3DBLEND_SRCCOLOR: return nvrhi::BlendFactor::SrcColor;
            case D3DBLEND_INVSRCCOLOR: return nvrhi::BlendFactor::InvSrcColor;
            case D3DBLEND_SRCALPHA: return nvrhi::BlendFactor::SrcAlpha;
            case D3DBLEND_INVSRCALPHA: return nvrhi::BlendFactor::InvSrcAlpha;
            // X8R8G8B8 targets read alpha 1 (the GPU's BGRA8 keeps whatever was written)
            case D3DBLEND_DESTALPHA: return targetHasAlpha ? nvrhi::BlendFactor::DstAlpha : nvrhi::BlendFactor::One;
            case D3DBLEND_INVDESTALPHA: return targetHasAlpha ? nvrhi::BlendFactor::InvDstAlpha : nvrhi::BlendFactor::Zero;
            case D3DBLEND_DESTCOLOR: return nvrhi::BlendFactor::DstColor;
            case D3DBLEND_INVDESTCOLOR: return nvrhi::BlendFactor::InvDstColor;
            case D3DBLEND_SRCALPHASAT: return nvrhi::BlendFactor::SrcAlphaSaturate;
            default: return nvrhi::BlendFactor::One;
            }
        }

        nvrhi::ComparisonFunc compare(DWORD f)
        {
            switch (f)
            {
            case D3DCMP_NEVER: return nvrhi::ComparisonFunc::Never;
            case D3DCMP_LESS: return nvrhi::ComparisonFunc::Less;
            case D3DCMP_EQUAL: return nvrhi::ComparisonFunc::Equal;
            case D3DCMP_LESSEQUAL: return nvrhi::ComparisonFunc::LessOrEqual;
            case D3DCMP_GREATER: return nvrhi::ComparisonFunc::Greater;
            case D3DCMP_NOTEQUAL: return nvrhi::ComparisonFunc::NotEqual;
            case D3DCMP_GREATEREQUAL: return nvrhi::ComparisonFunc::GreaterOrEqual;
            default: return nvrhi::ComparisonFunc::Always;
            }
        }

        nvrhi::StencilOp stencilOp(DWORD op)
        {
            switch (op)
            {
            case D3DSTENCILOP_ZERO: return nvrhi::StencilOp::Zero;
            case D3DSTENCILOP_REPLACE: return nvrhi::StencilOp::Replace;
            case D3DSTENCILOP_INCRSAT: return nvrhi::StencilOp::IncrementAndClamp;
            case D3DSTENCILOP_DECRSAT: return nvrhi::StencilOp::DecrementAndClamp;
            case D3DSTENCILOP_INVERT: return nvrhi::StencilOp::Invert;
            case D3DSTENCILOP_INCR: return nvrhi::StencilOp::IncrementAndWrap;
            case D3DSTENCILOP_DECR: return nvrhi::StencilOp::DecrementAndWrap;
            default: return nvrhi::StencilOp::Keep;
            }
        }

        // Render states the shader or the pipeline handles; the rest are logged once if set to something that matters.
        bool handledRenderState(DWORD state)
        {
            switch (state)
            {
            case D3DRENDERSTATE_ZENABLE: case D3DRENDERSTATE_FILLMODE: case D3DRENDERSTATE_SHADEMODE:
            case D3DRENDERSTATE_ZWRITEENABLE: case D3DRENDERSTATE_ALPHATESTENABLE: case D3DRENDERSTATE_LASTPIXEL:
            case D3DRENDERSTATE_SRCBLEND: case D3DRENDERSTATE_DESTBLEND: case D3DRENDERSTATE_CULLMODE:
            case D3DRENDERSTATE_ZFUNC: case D3DRENDERSTATE_ALPHAREF: case D3DRENDERSTATE_ALPHAFUNC:
            case D3DRENDERSTATE_DITHERENABLE: case D3DRENDERSTATE_ALPHABLENDENABLE: case D3DRENDERSTATE_SPECULARENABLE:
            case D3DRENDERSTATE_STENCILENABLE: case D3DRENDERSTATE_STENCILFAIL: case D3DRENDERSTATE_STENCILZFAIL:
            case D3DRENDERSTATE_STENCILPASS: case D3DRENDERSTATE_STENCILFUNC: case D3DRENDERSTATE_STENCILREF:
            case D3DRENDERSTATE_STENCILMASK: case D3DRENDERSTATE_STENCILWRITEMASK: case D3DRENDERSTATE_TEXTUREFACTOR:
            case D3DRENDERSTATE_CLIPPING: case D3DRENDERSTATE_LIGHTING: case D3DRENDERSTATE_AMBIENT:
            case D3DRENDERSTATE_COLORVERTEX: case D3DRENDERSTATE_LOCALVIEWER: case D3DRENDERSTATE_NORMALIZENORMALS:
            case D3DRENDERSTATE_DIFFUSEMATERIALSOURCE: case D3DRENDERSTATE_SPECULARMATERIALSOURCE:
            case D3DRENDERSTATE_AMBIENTMATERIALSOURCE: case D3DRENDERSTATE_EMISSIVEMATERIALSOURCE:
            // no visible effect, or always on
            case D3DRENDERSTATE_ANTIALIAS: case D3DRENDERSTATE_TEXTUREPERSPECTIVE: case D3DRENDERSTATE_LINEPATTERN:
            case D3DRENDERSTATE_ZVISIBLE: case D3DRENDERSTATE_STIPPLEDALPHA: case D3DRENDERSTATE_EDGEANTIALIAS:
            case D3DRENDERSTATE_EXTENTS: case D3DRENDERSTATE_COLORKEYBLENDENABLE: case D3DRENDERSTATE_FOGCOLOR:
            case D3DRENDERSTATE_FOGSTART: case D3DRENDERSTATE_FOGEND: case D3DRENDERSTATE_FOGDENSITY:
            case D3DRENDERSTATE_RANGEFOGENABLE: case D3DRENDERSTATE_ZBIAS:
                return true;
            default:
                return state >= D3DRENDERSTATE_WRAP0 && state <= D3DRENDERSTATE_WRAP7 && false;
            }
        }
    }

    // ------------------------------------------------------------------------------------------------------------
    // Device description

    D3DDEVICEDESC7 deviceDescription(REFCLSID type)
    {
        D3DDEVICEDESC7 d = {};
        d.dwDevCaps = D3DDEVCAPS_FLOATTLVERTEX | D3DDEVCAPS_EXECUTESYSTEMMEMORY | D3DDEVCAPS_TLVERTEXSYSTEMMEMORY |
            D3DDEVCAPS_TEXTUREVIDEOMEMORY | D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_CANRENDERAFTERFLIP |
            D3DDEVCAPS_DRAWPRIMITIVES2 | D3DDEVCAPS_DRAWPRIMITIVES2EX | D3DDEVCAPS_HWRASTERIZATION |
            D3DDEVCAPS_HWTRANSFORMANDLIGHT | D3DDEVCAPS_TEXTURENONLOCALVIDMEM | D3DDEVCAPS_CANBLTSYSTONONLOCAL;
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
        p.dwMiscCaps = D3DPMISCCAPS_MASKZ | D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW;
        p.dwRasterCaps = D3DPRASTERCAPS_DITHER | D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_SUBPIXEL | D3DPRASTERCAPS_FOGVERTEX |
            D3DPRASTERCAPS_FOGTABLE | D3DPRASTERCAPS_MIPMAPLODBIAS | D3DPRASTERCAPS_ZBIAS | D3DPRASTERCAPS_ANISOTROPY;
        p.dwZCmpCaps = p.dwAlphaCmpCaps = D3DPCMPCAPS_NEVER | D3DPCMPCAPS_LESS | D3DPCMPCAPS_EQUAL | D3DPCMPCAPS_LESSEQUAL |
            D3DPCMPCAPS_GREATER | D3DPCMPCAPS_NOTEQUAL | D3DPCMPCAPS_GREATEREQUAL | D3DPCMPCAPS_ALWAYS;
        p.dwSrcBlendCaps = p.dwDestBlendCaps = 0x1FFF;
        p.dwShadeCaps = D3DPSHADECAPS_COLORGOURAUDRGB | D3DPSHADECAPS_SPECULARGOURAUDRGB | D3DPSHADECAPS_ALPHAGOURAUDBLEND |
            D3DPSHADECAPS_FOGGOURAUD | D3DPSHADECAPS_COLORFLATRGB | D3DPSHADECAPS_SPECULARFLATRGB | D3DPSHADECAPS_ALPHAFLATBLEND |
            D3DPSHADECAPS_FOGFLAT;
        p.dwTextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_TRANSPARENCY |
            D3DPTEXTURECAPS_BORDER | D3DPTEXTURECAPS_TEXREPEATNOTSCALEDBYSIZE;
        p.dwTextureFilterCaps = D3DPTFILTERCAPS_NEAREST | D3DPTFILTERCAPS_LINEAR | D3DPTFILTERCAPS_MIPNEAREST |
            D3DPTFILTERCAPS_MIPLINEAR | D3DPTFILTERCAPS_LINEARMIPNEAREST | D3DPTFILTERCAPS_LINEARMIPLINEAR |
            D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MIPFPOINT | D3DPTFILTERCAPS_MIPFLINEAR |
            D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR;
        p.dwTextureBlendCaps = D3DPTBLENDCAPS_DECAL | D3DPTBLENDCAPS_MODULATE | D3DPTBLENDCAPS_DECALALPHA |
            D3DPTBLENDCAPS_MODULATEALPHA | D3DPTBLENDCAPS_DECALMASK | D3DPTBLENDCAPS_MODULATEMASK | D3DPTBLENDCAPS_COPY |
            D3DPTBLENDCAPS_ADD;
        p.dwTextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR | D3DPTADDRESSCAPS_CLAMP |
            D3DPTADDRESSCAPS_BORDER | D3DPTADDRESSCAPS_INDEPENDENTUV;
        d.dpcLineCaps = p;
        d.dpcTriCaps = p;
        d.dwDeviceRenderBitDepth = DDBD_16 | DDBD_32;
        d.dwDeviceZBufferBitDepth = DDBD_16 | DDBD_24 | DDBD_32;
        d.dwMinTextureWidth = 1;
        d.dwMinTextureHeight = 1;
        d.dwMaxTextureWidth = 16384;
        d.dwMaxTextureHeight = 16384;
        d.dwMaxTextureRepeat = 8192;
        d.dwMaxTextureAspectRatio = 16384;
        d.dwMaxAnisotropy = 16;
        d.dvGuardBandLeft = -16384.0f;
        d.dvGuardBandTop = -16384.0f;
        d.dvGuardBandRight = 16384.0f;
        d.dvGuardBandBottom = 16384.0f;
        d.dwStencilCaps = D3DSTENCILCAPS_KEEP | D3DSTENCILCAPS_ZERO | D3DSTENCILCAPS_REPLACE | D3DSTENCILCAPS_INCRSAT |
            D3DSTENCILCAPS_DECRSAT | D3DSTENCILCAPS_INVERT | D3DSTENCILCAPS_INCR | D3DSTENCILCAPS_DECR;
        d.dwFVFCaps = 8 | D3DFVFCAPS_DONOTSTRIPELEMENTS;
        d.dwTextureOpCaps = 0x00FFFFFF;
        d.wMaxTextureBlendStages = 2;
        d.wMaxSimultaneousTextures = 2;
        d.dwMaxActiveLights = 8;
        d.dvMaxVertexW = 1e10f;
        d.deviceGUID = type;
        d.wMaxUserClipPlanes = 0;
        d.wMaxVertexBlendMatrices = 0;
        d.dwVertexProcessingCaps = D3DVTXPCAPS_TEXGEN | D3DVTXPCAPS_MATERIALSOURCE7 | D3DVTXPCAPS_DIRECTIONALLIGHTS |
            D3DVTXPCAPS_POSITIONALLIGHTS | D3DVTXPCAPS_LOCALVIEWER;
        return d;
    }

    // ------------------------------------------------------------------------------------------------------------
    // Creation

    Device::Device(DirectDraw* ddraw) : m_lock(Context::lock()), m_ddraw(ddraw)
    {
        m_ddraw->AddRef();
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
        {
            std::scoped_lock lock(m_lock);
            if (g_shared)
            {
                auto& sets = g_shared->bindingSets;
                for (auto it = sets.begin(); it != sets.end();)
                {
                    it = it->first.device == this ? sets.erase(it) : std::next(it);
                }
            }
        }
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
        {
            std::scoped_lock lock(Context::lock());
            if (!Context::available() || !initShared())
            {
                return DDERR_GENERIC;
            }
        }
        auto* device = new Device(ddraw);
        nvrhi::IDevice* dev = Context::device();
        // Vulkan keeps a fixed number of versions per buffer for the frames in flight (Direct3D 12 suballocates freely):
        // enough for a frame's state changes several frames over (5.5 MB and 0.8 MB).
        device->m_vsConstants = dev->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(VertexConstants), "ff vertex constants", 4096));
        device->m_psConstants = dev->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(PixelConstants), "ff pixel constants", 8192));
        device->m_boneConstants = dev->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(BoneConstants), "ff bones", 2048));
        const HRESULT hr = device->m_vsConstants && device->m_psConstants && device->m_boneConstants ? device->bindTarget(target)
                                                                                                  : DDERR_GENERIC;
        if (FAILED(hr))
        {
            device->Release();
            return hr;
        }
        device->initState();
        Surface* depth = target->attachedDepth();
        LOG("NVRHI backend: Direct3D 7 device on a {}x{} {} render target, z-buffer {}", target->width(), target->height(),
            Format::name(target->format()), depth ? Format::name(depth->format()) : std::string("none"));
        *out = device;
        return D3D_OK;
    }

    HRESULT Device::bindTarget(Surface* target)
    {
        if (!target || target->kind() != Surface::Kind::RenderTarget || !target->gpu())
        {
            return DDERR_INVALIDPARAMS;
        }
        target->AddRef();
        if (m_target)
        {
            m_target->Release();
        }
        m_target = target;
        // As in Direct3D 9, a new target resets the viewport to all of it.
        m_viewport = {0, 0, target->width(), target->height(), 0.0f, 1.0f};
        m_vsDirty = true;
        return D3D_OK;
    }

    Surface* Device::depthSurface() const
    {
        return m_target ? m_target->attachedDepth() : nullptr;
    }

    void Device::initState()
    {
        DWORD* rs = m_rs;
        rs[D3DRENDERSTATE_TEXTUREPERSPECTIVE] = TRUE;
        rs[D3DRENDERSTATE_ZENABLE] = depthSurface() ? D3DZB_TRUE : D3DZB_FALSE;
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
        rs[D3DRENDERSTATE_STENCILFUNC] = D3DCMP_ALWAYS;
        rs[D3DRENDERSTATE_STENCILFAIL] = D3DSTENCILOP_KEEP;
        rs[D3DRENDERSTATE_STENCILZFAIL] = D3DSTENCILOP_KEEP;
        rs[D3DRENDERSTATE_STENCILPASS] = D3DSTENCILOP_KEEP;
        rs[D3DRENDERSTATE_STENCILMASK] = 0xFFFFFFFF;
        rs[D3DRENDERSTATE_STENCILWRITEMASK] = 0xFFFFFFFF;
        rs[D3DRENDERSTATE_TEXTUREFACTOR] = 0xFFFFFFFF;
        rs[D3DRENDERSTATE_CLIPPING] = TRUE;
        rs[D3DRENDERSTATE_LIGHTING] = TRUE;
        rs[D3DRENDERSTATE_COLORVERTEX] = TRUE;
        rs[D3DRENDERSTATE_LOCALVIEWER] = TRUE;
        rs[D3DRENDERSTATE_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1;
        rs[D3DRENDERSTATE_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
        rs[D3DRENDERSTATE_AMBIENTMATERIALSOURCE] = D3DMCS_COLOR2;
        rs[D3DRENDERSTATE_EMISSIVEMATERIALSOURCE] = D3DMCS_MATERIAL;
        rs[D3DRENDERSTATE_FOGEND] = 0x3F800000;     // 1.0f
        rs[D3DRENDERSTATE_FOGDENSITY] = 0x3F800000;
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
        }
        const D3DMATRIX id = identity();
        for (DWORD type = 0; type < kTransforms; ++type)
        {
            m_transforms[type] = id;
        }
        m_material = {};
        m_vsDirty = m_psDirty = true;
    }

    void Device::logRenderState(DWORD state, DWORD value)
    {
        if (handledRenderState(state))
        {
            return;
        }
        if ((state == D3DRENDERSTATE_FOGENABLE || state == D3DRENDERSTATE_FOGTABLEMODE || state == D3DRENDERSTATE_FOGVERTEXMODE)
            && value)
        {
            unsupported("fog");
        }
        else if (state == D3DRENDERSTATE_COLORKEYENABLE && value)
        {
            unsupported("D3DRENDERSTATE_COLORKEYENABLE (color keyed textures)");
        }
        else if (state == D3DRENDERSTATE_CLIPPLANEENABLE && value)
        {
            unsupported("user clip planes");
        }
        else if (state == D3DRENDERSTATE_VERTEXBLEND && value)
        {
            unsupported("vertex blending");
        }
        else if (state >= D3DRENDERSTATE_WRAP0 && state <= D3DRENDERSTATE_WRAP7 && value)
        {
            unsupported("texture coordinate wrapping (D3DRENDERSTATE_WRAP*)");
        }
        else if (state == D3DRENDERSTATE_FOGENABLE || state == D3DRENDERSTATE_FOGTABLEMODE ||
                 state == D3DRENDERSTATE_FOGVERTEXMODE || state == D3DRENDERSTATE_COLORKEYENABLE ||
                 state == D3DRENDERSTATE_CLIPPLANEENABLE || state == D3DRENDERSTATE_VERTEXBLEND ||
                 (state >= D3DRENDERSTATE_WRAP0 && state <= D3DRENDERSTATE_WRAP7))
        {
            // off: nothing to do
        }
        else
        {
            unsupported(Fmt::format("render state {}", state).c_str());
        }
    }

    void Device::logStageState(DWORD stage, DWORD type, DWORD value)
    {
        if (stage >= kShaderStages)
        {
            if (type == D3DTSS_COLOROP && value != D3DTOP_DISABLE)
            {
                unsupported("texture stages above 1");
            }
            return;
        }
        if (type == D3DTSS_TEXCOORDINDEX && (value & 0xFFFF0000))
        {
            unsupported("generated texture coordinates (D3DTSS_TCI_*)");
        }
        else if ((type == D3DTSS_COLOROP || type == D3DTSS_ALPHAOP) &&
                 (value == D3DTOP_PREMODULATE || value == D3DTOP_BUMPENVMAP || value == D3DTOP_BUMPENVMAPLUMINANCE))
        {
            unsupported(Fmt::format("texture operation {}", value).c_str());
        }
        else if ((type == D3DTSS_MAGFILTER || type == D3DTSS_MINFILTER) && value == D3DTFG_ANISOTROPIC)
        {
            unsupported("anisotropic filtering (linear instead)");
        }
    }

    // ------------------------------------------------------------------------------------------------------------
    // Drawing

    void Device::updateConstants(nvrhi::ICommandList* cmd, DWORD fvf)
    {
        if (m_paletteEpoch != m_epoch)
        {
            // Bound with every draw: written once per command list (the palette is written again by the next skinned draw).
            static const BoneConstants none = {};
            cmd->writeBuffer(m_boneConstants, &none, sizeof(none));
            m_paletteEpoch = m_epoch;
            m_paletteId = 0;
        }
        const bool ground = fvf == kGroundFvf;
        const bool skin = skinKey(fvf) && m_skin;
        const bool rhw = ground || (!skin && Fvf::pretransformed(fvf));
        if (fvf != m_constantsFvf)
        {
            m_constantsFvf = fvf;
            m_vsDirty = true;
        }
        if (m_vsDirty)
        {
            m_vsDirty = false;
            VertexConstants c = {};
            const D3DMATRIX& view = m_transforms[D3DTRANSFORMSTATE_VIEW];
            const D3DMATRIX worldView = multiply(m_transforms[D3DTRANSFORMSTATE_WORLD], view);
            std::memcpy(c.worldView, &worldView, sizeof(c.worldView));
            std::memcpy(c.projection, &m_transforms[D3DTRANSFORMSTATE_PROJECTION], sizeof(c.projection));
            normalMatrix(worldView, c.normalMatrix);
            for (DWORD s = 0; s < kShaderStages; ++s)
            {
                std::memcpy(c.texTransform[s], &m_transforms[D3DTRANSFORMSTATE_TEXTURE0 + s], sizeof(c.texTransform[s]));
                const DWORD flags = m_tss[s][D3DTSS_TEXTURETRANSFORMFLAGS];
                const DWORD count = flags & 0xFF;
                c.texCoords[s] = (m_tss[s][D3DTSS_TEXCOORDINDEX] & 0xFFFF) | (count != D3DTTFF_DISABLE ? count << 16 : 0) |
                    ((flags & D3DTTFF_PROJECTED) ? 1u << 24 : 0);
            }
            const float w = static_cast<float>(m_target->width()), h = static_cast<float>(m_target->height());
            c.target[0] = w;
            c.target[1] = h;
            c.target[2] = 1.0f / w;
            c.target[3] = 1.0f / h;
            colorValue(m_material.dcvDiffuse, c.materialDiffuse);
            colorValue(m_material.dcvAmbient, c.materialAmbient);
            colorValue(m_material.dcvSpecular, c.materialSpecular);
            colorValue(m_material.dcvEmissive, c.materialEmissive);
            colorDword(m_rs[D3DRENDERSTATE_AMBIENT], c.ambient);
            c.power = m_material.dvPower;
            uint32_t flags = 0;
            if (rhw) flags |= kFlagRhw;
            if (!rhw && m_rs[D3DRENDERSTATE_LIGHTING]) flags |= kFlagLighting;
            if (ground)
            {
                flags |= kFlagGround | kFlagDiffuse;
                std::memcpy(c.groundTransform, m_ground, sizeof(m_ground));
            }
            else if (skin)
            {
                flags |= kFlagSkin;
                if (m_skin->normals) flags |= kFlagNormal;
                if (m_skin->normalizeSkinned) flags |= kFlagSkinNormalize;
                if (m_skin->vertexColor) flags |= kFlagDiffuse;
                if (m_skin->vertexColor && !m_skin->diffuse)
                {
                    flags |= kFlagSkinColor;
                    colorDword(m_skin->color, c.skinColor);
                }
            }
            else
            {
                if (fvf & D3DFVF_NORMAL) flags |= kFlagNormal;
                if (fvf & D3DFVF_DIFFUSE) flags |= kFlagDiffuse;
                if (fvf & D3DFVF_SPECULAR) flags |= kFlagSpecular;
            }
            if (m_rs[D3DRENDERSTATE_NORMALIZENORMALS]) flags |= kFlagNormalize;
            if (m_rs[D3DRENDERSTATE_LOCALVIEWER]) flags |= kFlagLocalViewer;
            if (m_rs[D3DRENDERSTATE_SPECULARENABLE]) flags |= kFlagSpecularOn;
            if (m_rs[D3DRENDERSTATE_COLORVERTEX]) flags |= kFlagColorVertex;
            c.flags = flags;
            c.materialSources = (m_rs[D3DRENDERSTATE_DIFFUSEMATERIALSOURCE] & 3) | (m_rs[D3DRENDERSTATE_SPECULARMATERIALSOURCE] & 3) << 2 |
                (m_rs[D3DRENDERSTATE_AMBIENTMATERIALSOURCE] & 3) << 4 | (m_rs[D3DRENDERSTATE_EMISSIVEMATERIALSOURCE] & 3) << 6;
            if (flags & kFlagLighting)
            {
                for (DWORD i = 0; i < m_lights.size() && c.lightCount < 8; ++i)
                {
                    const Light& slot = m_lights[i];
                    if (!slot.set || !slot.enabled)
                    {
                        continue;
                    }
                    const D3DLIGHT7& l = slot.light;
                    LightConstants& out = c.lights[c.lightCount++];
                    colorValue(l.dcvDiffuse, out.diffuse);
                    colorValue(l.dcvSpecular, out.specular);
                    colorValue(l.dcvAmbient, out.ambient);
                    const D3DVECTOR& p = l.dvPosition;
                    out.position[0] = p.x * view._11 + p.y * view._21 + p.z * view._31 + view._41;
                    out.position[1] = p.x * view._12 + p.y * view._22 + p.z * view._32 + view._42;
                    out.position[2] = p.x * view._13 + p.y * view._23 + p.z * view._33 + view._43;
                    const float type = l.dltType == D3DLIGHT_PARALLELPOINT ? 3.0f : static_cast<float>(l.dltType);
                    out.position[3] = type;
                    const D3DVECTOR& d = l.dvDirection;
                    float dx = d.x * view._11 + d.y * view._21 + d.z * view._31;
                    float dy = d.x * view._12 + d.y * view._22 + d.z * view._32;
                    float dz = d.x * view._13 + d.y * view._23 + d.z * view._33;
                    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (len > 0.0f)
                    {
                        dx /= len;
                        dy /= len;
                        dz /= len;
                    }
                    out.direction[0] = dx;
                    out.direction[1] = dy;
                    out.direction[2] = dz;
                    out.attenuation[0] = l.dvRange > 0.0f ? l.dvRange : 3.4e38f;
                    out.attenuation[1] = l.dvAttenuation0;
                    out.attenuation[2] = l.dvAttenuation1;
                    out.attenuation[3] = l.dvAttenuation2;
                    out.spot[0] = l.dvFalloff;
                    out.spot[1] = std::cos(l.dvTheta * 0.5f);
                    out.spot[2] = std::cos(l.dvPhi * 0.5f);
                }
            }
            cmd->writeBuffer(m_vsConstants, &c, sizeof(c));
        }
        if (m_psDirty)
        {
            m_psDirty = false;
            PixelConstants c = {};
            const DWORD* s0 = m_tss[0];
            const DWORD* s1 = m_tss[1];
            c.stage0[0] = s0[D3DTSS_COLOROP];
            c.stage0[1] = s0[D3DTSS_COLORARG1];
            c.stage0[2] = s0[D3DTSS_COLORARG2];
            c.stage0[3] = s0[D3DTSS_ALPHAOP];
            c.stage0b[0] = s0[D3DTSS_ALPHAARG1];
            c.stage0b[1] = s0[D3DTSS_ALPHAARG2];
            c.stage0b[2] = m_textures[0] ? 1 : 0;
            c.stage1[0] = s0[D3DTSS_COLOROP] == D3DTOP_DISABLE ? D3DTOP_DISABLE : s1[D3DTSS_COLOROP];
            c.stage1[1] = s1[D3DTSS_COLORARG1];
            c.stage1[2] = s1[D3DTSS_COLORARG2];
            c.stage1[3] = s1[D3DTSS_ALPHAOP];
            c.stage1b[0] = s1[D3DTSS_ALPHAARG1];
            c.stage1b[1] = s1[D3DTSS_ALPHAARG2];
            c.stage1b[2] = m_textures[1] ? 1 : 0;
            colorDword(m_rs[D3DRENDERSTATE_TEXTUREFACTOR], c.textureFactor);
            c.alphaFunc = m_rs[D3DRENDERSTATE_ALPHATESTENABLE] ? m_rs[D3DRENDERSTATE_ALPHAFUNC] : 0;
            c.alphaRef = static_cast<float>(m_rs[D3DRENDERSTATE_ALPHAREF] & 0xFF);
            c.flags = m_rs[D3DRENDERSTATE_SPECULARENABLE] ? kFlagSpecularOn : 0;
            cmd->writeBuffer(m_psConstants, &c, sizeof(c));
        }
    }

    nvrhi::IGraphicsPipeline* Device::pipeline(DWORD fvf, nvrhi::PrimitiveType type, nvrhi::IFramebuffer* fb)
    {
        const nvrhi::FramebufferInfoEx& info = fb->getFramebufferInfo();
        Surface* depth = depthSurface();
        const bool zOn = depth && m_rs[D3DRENDERSTATE_ZENABLE] != D3DZB_FALSE;
        const bool stencilOn = depth && m_rs[D3DRENDERSTATE_STENCILENABLE] && Format::stencil(depth->format());
        Shared::PipelineKey key = {};
        key.fvf = fvf;
        key.prim = static_cast<uint32_t>(type);
        key.blend = m_rs[D3DRENDERSTATE_ALPHABLENDENABLE] ? 1u | (m_rs[D3DRENDERSTATE_SRCBLEND] & 31) << 1 |
            (m_rs[D3DRENDERSTATE_DESTBLEND] & 31) << 6 : 0u;
        key.depth = zOn ? 1u | (m_rs[D3DRENDERSTATE_ZWRITEENABLE] ? 2u : 0u) | (m_rs[D3DRENDERSTATE_ZFUNC] & 15) << 2 : 0u;
        key.raster = (m_rs[D3DRENDERSTATE_CULLMODE] & 3) | (m_rs[D3DRENDERSTATE_FILLMODE] == D3DFILL_WIREFRAME ? 4u : 0u);
        if (stencilOn)
        {
            key.stencil[0] = 1u | (m_rs[D3DRENDERSTATE_STENCILFUNC] & 15) << 1 | (m_rs[D3DRENDERSTATE_STENCILFAIL] & 15) << 5 |
                (m_rs[D3DRENDERSTATE_STENCILZFAIL] & 15) << 9 | (m_rs[D3DRENDERSTATE_STENCILPASS] & 15) << 13;
            key.stencil[1] = m_rs[D3DRENDERSTATE_STENCILREF] & 0xFF;
            key.stencil[2] = (m_rs[D3DRENDERSTATE_STENCILMASK] & 0xFF) | (m_rs[D3DRENDERSTATE_STENCILWRITEMASK] & 0xFF) << 16;
        }
        key.color = info.colorFormats.empty() ? nvrhi::Format::UNKNOWN : info.colorFormats[0];
        key.depthFormat = info.depthFormat;
        auto it = g_shared->pipelines.find(key);
        if (it != g_shared->pipelines.end())
        {
            return it->second;
        }

        nvrhi::GraphicsPipelineDesc desc;
        desc.VS = g_shared->vs;
        desc.PS = g_shared->ps;
        desc.inputLayout = inputLayout(fvf);
        desc.primType = type;
        desc.bindingLayouts = {g_shared->layout};
        nvrhi::BlendState::RenderTarget& blend = desc.renderState.blendState.targets[0];
        if (key.blend)
        {
            const bool targetAlpha = m_target->format() == d9::D3DFMT_A8R8G8B8;
            DWORD src = m_rs[D3DRENDERSTATE_SRCBLEND], dst = m_rs[D3DRENDERSTATE_DESTBLEND];
            if (src == D3DBLEND_BOTHSRCALPHA)
            {
                src = D3DBLEND_SRCALPHA;
                dst = D3DBLEND_INVSRCALPHA;
            }
            else if (src == D3DBLEND_BOTHINVSRCALPHA)
            {
                src = D3DBLEND_INVSRCALPHA;
                dst = D3DBLEND_SRCALPHA;
            }
            blend.setBlendEnable(true).setSrcBlend(blendFactor(src, targetAlpha)).setDestBlend(blendFactor(dst, targetAlpha))
                .setSrcBlendAlpha(blendFactor(src, targetAlpha)).setDestBlendAlpha(blendFactor(dst, targetAlpha));
        }
        nvrhi::DepthStencilState& ds = desc.renderState.depthStencilState;
        ds.setDepthTestEnable(zOn).setDepthWriteEnable(zOn && m_rs[D3DRENDERSTATE_ZWRITEENABLE])
            .setDepthFunc(compare(m_rs[D3DRENDERSTATE_ZFUNC])).setStencilEnable(stencilOn);
        if (stencilOn)
        {
            nvrhi::DepthStencilState::StencilOpDesc op;
            op.setFailOp(stencilOp(m_rs[D3DRENDERSTATE_STENCILFAIL])).setDepthFailOp(stencilOp(m_rs[D3DRENDERSTATE_STENCILZFAIL]))
                .setPassOp(stencilOp(m_rs[D3DRENDERSTATE_STENCILPASS])).setStencilFunc(compare(m_rs[D3DRENDERSTATE_STENCILFUNC]));
            ds.setFrontFaceStencil(op).setBackFaceStencil(op).setStencilRefValue(static_cast<uint8_t>(key.stencil[1]))
                .setStencilReadMask(static_cast<uint8_t>(m_rs[D3DRENDERSTATE_STENCILMASK]))
                .setStencilWriteMask(static_cast<uint8_t>(m_rs[D3DRENDERSTATE_STENCILWRITEMASK]));
        }
        nvrhi::RasterState& raster = desc.renderState.rasterState;
        switch (m_rs[D3DRENDERSTATE_CULLMODE])
        {
        case D3DCULL_CW: raster.setCullFront(); break;
        case D3DCULL_CCW: raster.setCullBack(); break;
        default: raster.setCullNone(); break;
        }
        raster.setFrontCounterClockwise(false).setDepthClipEnable(true);
        if (m_rs[D3DRENDERSTATE_FILLMODE] == D3DFILL_WIREFRAME)
        {
            raster.setFillWireframe();
        }
        nvrhi::GraphicsPipelineHandle pipeline = Context::device()->createGraphicsPipeline(desc, info);
        if (!pipeline)
        {
            LOG("NVRHI backend: pipeline for FVF {:x}, blend {:x}, depth {:x} could not be created", fvf, key.blend, key.depth);
        }
        g_shared->pipelines.emplace(key, pipeline);
        static size_t logged = 0;
        if (g_shared->pipelines.size() >= logged + 64)
        {
            logged = g_shared->pipelines.size();
            LOG("NVRHI backend: {} pipelines", logged);
        }
        return pipeline;
    }

    nvrhi::IBindingSet* Device::bindings()
    {
        Shared::BindingKey key = {};
        key.device = this;
        for (DWORD s = 0; s < kShaderStages; ++s)
        {
            nvrhi::ITexture* texture = m_textures[s] ? m_textures[s]->texture() : nullptr;
            key.textures[s] = texture ? texture : Context::whiteTexture();
            const DWORD* t = m_tss[s];
            key.samplers[s] = Context::sampler(t[D3DTSS_ADDRESSU], t[D3DTSS_ADDRESSV], t[D3DTSS_MAGFILTER] != D3DTFG_POINT,
                t[D3DTSS_MINFILTER] != D3DTFN_POINT, t[D3DTSS_MIPFILTER],
                t[D3DTSS_ADDRESSU] == D3DTADDRESS_BORDER || t[D3DTSS_ADDRESSV] == D3DTADDRESS_BORDER ? t[D3DTSS_BORDERCOLOR] : 0);
        }
        auto it = g_shared->bindingSets.find(key);
        if (it != g_shared->bindingSets.end())
        {
            return it->second;
        }
        nvrhi::BindingSetDesc desc;
        desc.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_vsConstants),
            nvrhi::BindingSetItem::ConstantBuffer(1, m_psConstants),
            nvrhi::BindingSetItem::ConstantBuffer(2, m_boneConstants),
            nvrhi::BindingSetItem::Texture_SRV(0, key.textures[0]),
            nvrhi::BindingSetItem::Texture_SRV(1, key.textures[1]),
            nvrhi::BindingSetItem::Sampler(0, key.samplers[0]),
            nvrhi::BindingSetItem::Sampler(1, key.samplers[1]),
        };
        nvrhi::BindingSetHandle set = Context::device()->createBindingSet(desc, g_shared->layout);
        g_shared->bindingSets.emplace(key, set);
        return set;
    }

    HRESULT Device::draw(D3DPRIMITIVETYPE type, const VertexSource& source, const WORD* indices, DWORD indexCount)
    {
        if (!m_target || !source.count || !source.stride)
        {
            return D3D_OK;
        }
        if ((source.fvf & D3DFVF_POSITION_MASK) >= D3DFVF_XYZB1 && (source.fvf & D3DFVF_POSITION_MASK) <= D3DFVF_XYZB5)
        {
            unsupported("vertex blending weights in the vertex format (ignored)");
        }
        nvrhi::ICommandList* cmd = Context::commands();
        if (Context::epoch() != m_epoch)
        {
            m_epoch = Context::epoch();
            m_vsDirty = m_psDirty = true;
        }

        // Triangle fans become lists (Direct3D 12 and Vulkan have no fans... Vulkan has, NVRHI not): indexed.
        nvrhi::PrimitiveType prim = nvrhi::PrimitiveType::TriangleList;
        switch (type)
        {
        case D3DPT_POINTLIST: prim = nvrhi::PrimitiveType::PointList; break;
        case D3DPT_LINELIST: prim = nvrhi::PrimitiveType::LineList; break;
        case D3DPT_LINESTRIP: prim = nvrhi::PrimitiveType::LineStrip; break;
        case D3DPT_TRIANGLESTRIP: prim = nvrhi::PrimitiveType::TriangleStrip; break;
        default: break;
        }
        if (type == D3DPT_TRIANGLEFAN)
        {
            const DWORD n = indices ? indexCount : source.count;
            m_indexScratch.clear();
            for (DWORD i = 1; i + 1 < n; ++i)
            {
                m_indexScratch.push_back(indices ? indices[0] : WORD(0));
                m_indexScratch.push_back(indices ? indices[i] : static_cast<WORD>(i));
                m_indexScratch.push_back(indices ? indices[i + 1] : static_cast<WORD>(i + 1));
            }
            indices = m_indexScratch.data();
            indexCount = static_cast<DWORD>(m_indexScratch.size());
        }

        Context::Upload vertices, idx = {};
        const uint32_t vertexBytes = source.count * source.stride;
        if (!Context::upload(vertexBytes, vertices))
        {
            unsupported("draws larger than the upload ring");
            return D3D_OK;
        }
        std::memcpy(vertices.data, source.data, vertexBytes);
        if (indices && !Context::upload(indexCount * sizeof(WORD), idx))
        {
            return D3D_OK;
        }
        if (indices)
        {
            std::memcpy(idx.data, indices, indexCount * sizeof(WORD));
        }

        nvrhi::GraphicsState state;
        if (!setup(cmd, source.fvf, prim, state))
        {
            return D3D_OK;
        }
        state.vertexBuffers = {
            nvrhi::VertexBufferBinding().setBuffer(vertices.buffer).setSlot(0).setOffset(vertices.offset),
            nvrhi::VertexBufferBinding().setBuffer(Context::vertexDefaults()).setSlot(1).setOffset(0),
        };
        if (indices)
        {
            state.indexBuffer = nvrhi::IndexBufferBinding().setBuffer(idx.buffer).setFormat(nvrhi::Format::R16_UINT).setOffset(idx.offset);
        }
        cmd->setGraphicsState(state);
        if (indices)
        {
            cmd->drawIndexed(nvrhi::DrawArguments().setVertexCount(indexCount));
        }
        else
        {
            cmd->draw(nvrhi::DrawArguments().setVertexCount(source.count));
        }
        return D3D_OK;
    }

    bool Device::setup(nvrhi::ICommandList* cmd, DWORD fvf, nvrhi::PrimitiveType prim, nvrhi::GraphicsState& state)
    {
        nvrhi::IBindingSet* set = bindings();      // uploads the textures' CPU changes first
        updateConstants(cmd, fvf);
        Surface* depth = depthSurface();
        nvrhi::IFramebuffer* fb = m_inWorld ? Context::framebuffer(m_worldColor, depth ? m_worldDepth.Get() : nullptr)
                                            : Context::framebuffer(m_target->gpu(), depth ? depth->gpu() : nullptr);
        nvrhi::IGraphicsPipeline* pso = pipeline(fvf, prim, fb);
        if (!pso || !set)
        {
            return false;
        }
        // In the world's own target everything is `s` times larger; pretransformed positions stay in render target
        // pixels (the shader's normalized coordinates don't depend on the size), viewports and scissors scale.
        const float s = m_inWorld ? m_worldScale : 1.0f;
        state.pipeline = pso;
        state.framebuffer = fb;
        state.bindings = {set};
        // Direct3D 7's viewport, clipped to the target: the scissor for every draw. Untransformed vertices go through it
        // with Direct3D 9's pixel centers (half a pixel down and right of today's); pretransformed ones are already in
        // target pixels (the shader moves them) and only clipped.
        const LONG tw = static_cast<LONG>(m_target->width()), th = static_cast<LONG>(m_target->height());
        const LONG x0 = std::clamp<LONG>(static_cast<LONG>(m_viewport.dwX), 0, tw);
        const LONG y0 = std::clamp<LONG>(static_cast<LONG>(m_viewport.dwY), 0, th);
        const LONG x1 = std::clamp<LONG>(static_cast<LONG>(m_viewport.dwX + m_viewport.dwWidth), 0, tw);
        const LONG y1 = std::clamp<LONG>(static_cast<LONG>(m_viewport.dwY + m_viewport.dwHeight), 0, th);
        if (x1 <= x0 || y1 <= y0)
        {
            return false;
        }
        nvrhi::Viewport vp;
        if (fvf == kGroundFvf || (!skinKey(fvf) && Fvf::pretransformed(fvf)))
        {
            vp = nvrhi::Viewport(0.0f, static_cast<float>(tw) * s, 0.0f, static_cast<float>(th) * s, 0.0f, 1.0f);
        }
        else
        {
            const float vx = (static_cast<float>(m_viewport.dwX) + 0.5f) * s, vy = (static_cast<float>(m_viewport.dwY) + 0.5f) * s;
            vp = nvrhi::Viewport(vx, vx + static_cast<float>(m_viewport.dwWidth) * s, vy, vy + static_cast<float>(m_viewport.dwHeight) * s,
                std::clamp(m_viewport.dvMinZ, 0.0f, 1.0f), std::clamp(m_viewport.dvMaxZ, 0.0f, 1.0f));
        }
        state.viewport.addViewport(vp);
        state.viewport.addScissorRect(nvrhi::Rect(static_cast<int>(x0 * s), static_cast<int>(x1 * s), static_cast<int>(y0 * s),
            static_cast<int>(y1 * s)));
        return true;
    }

    bool Device::drawGround(nvrhi::IBuffer* buffer, uint32_t firstQuad, uint32_t quads, float scale, float offsetX,
        float offsetY, float z)
    {
        std::scoped_lock lock(m_lock);
        if (!m_target || !buffer)
        {
            return false;
        }
        if (!quads)
        {
            return true;
        }
        // What the fixed-function pipeline would do with pretransformed vertices that the ground mode doesn't.
        if (m_rs[D3DRENDERSTATE_FOGENABLE])
        {
            return false;
        }
        for (DWORD stage = 0; stage < kShaderStages && m_tss[stage][D3DTSS_COLOROP] != D3DTOP_DISABLE; ++stage)
        {
            if (m_tss[stage][D3DTSS_TEXCOORDINDEX] > 1 || m_tss[stage][D3DTSS_TEXTURETRANSFORMFLAGS] != D3DTTFF_DISABLE)
            {
                return false;
            }
        }
        nvrhi::ICommandList* cmd = Context::commands();
        if (Context::epoch() != m_epoch)
        {
            m_epoch = Context::epoch();
            m_vsDirty = m_psDirty = true;
        }
        if (!g_shared->quadIndices)
        {
            g_shared->quadIndices = Context::device()->createBuffer(nvrhi::BufferDesc().setByteSize(kQuadsPerDraw * 6 * sizeof(WORD))
                .setIsIndexBuffer(true).setDebugName("quad indices").setInitialState(nvrhi::ResourceStates::IndexBuffer)
                .setKeepInitialState(true));
        }
        if (!g_shared->quadIndices)
        {
            return false;
        }
        if (!g_shared->quadIndicesWritten)
        {
            std::vector<WORD> indices(kQuadsPerDraw * 6);
            for (uint32_t q = 0; q < kQuadsPerDraw; ++q)
            {
                const WORD v = static_cast<WORD>(q * 4);
                const WORD quad[6] = {v, static_cast<WORD>(v + 1), static_cast<WORD>(v + 2), static_cast<WORD>(v + 2),
                    static_cast<WORD>(v + 1), static_cast<WORD>(v + 3)};
                std::copy(std::begin(quad), std::end(quad), indices.begin() + q * 6);
            }
            cmd->writeBuffer(g_shared->quadIndices, indices.data(), indices.size() * sizeof(WORD));
            g_shared->quadIndicesWritten = true;
        }
        const float ground[4] = {scale, offsetX, offsetY, z};
        if (std::memcmp(ground, m_ground, sizeof(ground)) != 0)
        {
            std::memcpy(m_ground, ground, sizeof(ground));
            m_vsDirty = true;
        }
        nvrhi::GraphicsState state;
        if (!setup(cmd, kGroundFvf, nvrhi::PrimitiveType::TriangleList, state))
        {
            return true;
        }
        state.indexBuffer = nvrhi::IndexBufferBinding().setBuffer(g_shared->quadIndices).setFormat(nvrhi::Format::R16_UINT);
        for (uint32_t done = 0; done < quads;)
        {
            const uint32_t n = std::min(quads - done, kQuadsPerDraw);
            state.vertexBuffers = {
                nvrhi::VertexBufferBinding().setBuffer(buffer).setSlot(0).setOffset(uint64_t(firstQuad + done) * 4 * kGroundStride),
                nvrhi::VertexBufferBinding().setBuffer(Context::vertexDefaults()).setSlot(1).setOffset(0),
            };
            cmd->setGraphicsState(state);
            cmd->drawIndexed(nvrhi::DrawArguments().setVertexCount(n * 6));
            done += n;
        }
        return true;
    }

    bool Device::drawSkinned(nvrhi::IBuffer* vertices, uint32_t vertexCount, nvrhi::IBuffer* indices, uint32_t firstIndex,
        uint32_t indexCount, const DDraw9::Skin::Draw& draw)
    {
        std::scoped_lock lock(m_lock);
        if (!m_target || !vertices || !indices || !draw.palette || draw.bones == 0 || draw.bones > DDraw9::Skin::kMaxBones ||
            draw.paletteId == 0 || indexCount < 3)
        {
            return false;
        }
        // What the fixed-function pipeline would do that the skinning mode doesn't: the caller skins on the CPU instead.
        const bool lighting = m_rs[D3DRENDERSTATE_LIGHTING] != 0;
        if (m_rs[D3DRENDERSTATE_FOGENABLE] || m_rs[D3DRENDERSTATE_CLIPPLANEENABLE] ||
            m_rs[D3DRENDERSTATE_VERTEXBLEND] != D3DVBLEND_DISABLE || (lighting && !draw.normals))
        {
            return false;
        }
        for (DWORD stage = 0; stage < kStages && m_tss[stage][D3DTSS_COLOROP] != D3DTOP_DISABLE; ++stage)
        {
            const DWORD flags = m_tss[stage][D3DTSS_TEXTURETRANSFORMFLAGS];
            if (stage >= kShaderStages || m_tss[stage][D3DTSS_TEXCOORDINDEX] > 3 ||
                (flags != D3DTTFF_DISABLE && flags != D3DTTFF_COUNT2))
            {
                return false;
            }
        }
        DWORD lights = 0;
        for (const Light& l : m_lights)
        {
            if (l.set && l.enabled && (++lights > 8 || (l.light.dltType != D3DLIGHT_POINT && l.light.dltType != D3DLIGHT_SPOT &&
                l.light.dltType != D3DLIGHT_DIRECTIONAL)))
            {
                return false;
            }
        }
        if (lighting)
        {
            // A shadow's world matrix flattens it onto the ground (no inverse); lit draws need one for their normals.
            const D3DMATRIX wv = multiply(m_transforms[D3DTRANSFORMSTATE_WORLD], m_transforms[D3DTRANSFORMSTATE_VIEW]);
            const float det = wv._11 * (wv._22 * wv._33 - wv._23 * wv._32) - wv._12 * (wv._21 * wv._33 - wv._23 * wv._31) +
                wv._13 * (wv._21 * wv._32 - wv._22 * wv._31);
            if (std::fabs(det) < 1e-12f)
            {
                return false;
            }
        }

        nvrhi::ICommandList* cmd = Context::commands();
        if (Context::epoch() != m_epoch)
        {
            m_epoch = Context::epoch();
            m_vsDirty = m_psDirty = true;
        }
        // Per-vertex diffuse colors into the upload ring (slot 2).
        const bool stream = draw.vertexColor && draw.diffuse;
        Context::Upload colors = {};
        if (stream)
        {
            if (!Context::upload(vertexCount * 4, colors))
            {
                return false;
            }
            auto* out = reinterpret_cast<DWORD*>(colors.data);
            const auto* in = reinterpret_cast<const uint8_t*>(draw.diffuse);
            for (uint32_t v = 0; v < vertexCount; ++v)
            {
                std::memcpy(out + v, in + size_t(v) * draw.diffuseStride, 4);
            }
        }
        // Bones: Granny's matrix rows with the translation in w. A character's shadow pass computes the same pose again
        // (another palette id, the same values): compared before writing.
        const size_t paletteFloats = size_t(draw.bones) * 12;
        if (draw.paletteId != m_paletteId && m_palette.size() == paletteFloats &&
            std::memcmp(m_palette.data(), draw.palette, paletteFloats * sizeof(float)) == 0)
        {
            m_paletteId = draw.paletteId;
        }
        if (draw.paletteId != m_paletteId || m_paletteEpoch != m_epoch)
        {
            m_palette.assign(draw.palette, draw.palette + paletteFloats);
            BoneConstants b = {};
            for (uint32_t i = 0; i < draw.bones; ++i)
            {
                const float* m = draw.palette + size_t(i) * 12;
                for (int r = 0; r < 3; ++r)
                {
                    b.rows[i * 3 + r][0] = m[r * 3];
                    b.rows[i * 3 + r][1] = m[r * 3 + 1];
                    b.rows[i * 3 + r][2] = m[r * 3 + 2];
                    b.rows[i * 3 + r][3] = m[9 + r];
                }
            }
            cmd->writeBuffer(m_boneConstants, &b, sizeof(b));
            m_paletteId = draw.paletteId;
            m_paletteEpoch = m_epoch;
        }
        const DWORD key = stream ? kSkinDiffuseFvf : kSkinFvf;
        m_skin = &draw;
        m_vsDirty = true;       // the draw's own flags and color
        nvrhi::GraphicsState state;
        const bool ready = setup(cmd, key, nvrhi::PrimitiveType::TriangleList, state);
        m_skin = nullptr;
        m_constantsFvf = ~0u;   // the next draw writes its own
        if (!ready)
        {
            return true;
        }
        state.vertexBuffers = {
            nvrhi::VertexBufferBinding().setBuffer(vertices).setSlot(0).setOffset(0),
            nvrhi::VertexBufferBinding().setBuffer(Context::vertexDefaults()).setSlot(1).setOffset(0),
        };
        if (stream)
        {
            state.vertexBuffers.push_back(nvrhi::VertexBufferBinding().setBuffer(colors.buffer).setSlot(2).setOffset(colors.offset));
        }
        state.indexBuffer = nvrhi::IndexBufferBinding().setBuffer(indices).setFormat(nvrhi::Format::R16_UINT)
            .setOffset(uint64_t(firstIndex) * sizeof(WORD));
        cmd->setGraphicsState(state);
        cmd->drawIndexed(nvrhi::DrawArguments().setVertexCount(indexCount));
        return true;
    }

    void Device::beginWorld(float scale)
    {
        std::scoped_lock lock(m_lock);
        if (!m_target || m_inWorld || scale <= 1.0f)
        {
            return;
        }
        const uint32_t w = static_cast<uint32_t>(std::lround(m_target->width() * scale));
        const uint32_t h = static_cast<uint32_t>(std::lround(m_target->height() * scale));
        if (!m_worldColor || m_worldColor->getDesc().width != w || m_worldColor->getDesc().height != h)
        {
            if (m_worldColor)
            {
                Context::forget(m_worldColor);
                Context::forget(m_worldDepth);
            }
            nvrhi::IDevice* dev = Context::device();
            m_worldColor = dev->createTexture(nvrhi::TextureDesc().setWidth(w).setHeight(h).setFormat(nvrhi::Format::BGRA8_UNORM)
                .setIsRenderTarget(true).setDebugName("world").setInitialState(nvrhi::ResourceStates::RenderTarget)
                .setKeepInitialState(true));
            m_worldDepth = dev->createTexture(nvrhi::TextureDesc().setWidth(w).setHeight(h).setFormat(nvrhi::Format::D24S8)
                .setIsRenderTarget(true).setDebugName("world depth").setInitialState(nvrhi::ResourceStates::DepthWrite)
                .setKeepInitialState(true));
            if (!m_worldColor || !m_worldDepth)
            {
                LOG("NVRHI backend: the world's {}x{} target could not be created", w, h);
                m_worldColor = m_worldDepth = nullptr;
                return;
            }
            LOG("NVRHI backend: the world view drawn at {}x{} ({:.2f} x the frame)", w, h, scale);
        }
        m_worldScale = scale;
        m_inWorld = true;
        // What the frame had before the world (the game cleared it) and an empty depth: the world starts from there.
        nvrhi::ICommandList* cmd = Context::commands();
        Context::copy(m_target->gpu(), {0, 0, static_cast<LONG>(m_target->width()), static_cast<LONG>(m_target->height())},
            m_worldColor, {0, 0, static_cast<LONG>(w), static_cast<LONG>(h)}, false);
        cmd->clearDepthStencilTexture(m_worldDepth, nvrhi::AllSubresources, true, 1.0f, true, 0);
    }

    nvrhi::ITexture* Device::renderTarget() const
    {
        return m_target ? m_target->gpu() : nullptr;
    }

    void Device::endWorld()
    {
        std::scoped_lock lock(m_lock);
        if (!m_inWorld)
        {
            return;
        }
        m_inWorld = false;
        const nvrhi::TextureDesc& d = m_worldColor->getDesc();
        Context::copy(m_worldColor, {0, 0, static_cast<LONG>(d.width), static_cast<LONG>(d.height)}, m_target->gpu(),
            {0, 0, static_cast<LONG>(m_target->width()), static_cast<LONG>(m_target->height())}, true);
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
        if (!m_target)
        {
            return D3D_OK;
        }
        Surface* depth = depthSurface();
        if (!depth)
        {
            flags &= ~(D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL);
        }
        else if (!Format::stencil(depth->format()))
        {
            flags &= ~D3DCLEAR_STENCIL;
        }
        if (!flags)
        {
            return D3D_OK;
        }
        // The viewport limits a clear, as in Direct3D 7 and 9.
        const LONG tw = static_cast<LONG>(m_target->width()), th = static_cast<LONG>(m_target->height());
        const RECT viewport = {std::clamp<LONG>(m_viewport.dwX, 0, tw), std::clamp<LONG>(m_viewport.dwY, 0, th),
            std::clamp<LONG>(m_viewport.dwX + m_viewport.dwWidth, 0, tw), std::clamp<LONG>(m_viewport.dwY + m_viewport.dwHeight, 0, th)};
        const bool whole = viewport.left == 0 && viewport.top == 0 && viewport.right == tw && viewport.bottom == th;
        nvrhi::ICommandList* cmd = Context::commands();
        float rgba[4];
        colorDword(color, rgba);
        if (flags & D3DCLEAR_STENCIL)
        {
            static bool logged = false;
            if (!logged && !whole)
            {
                logged = true;
                unsupported("clearing the stencil buffer in part of the target");
            }
        }
        nvrhi::ITexture* colorTarget = m_inWorld ? m_worldColor.Get() : m_target->gpu();
        nvrhi::ITexture* depthTarget = !depth ? nullptr : m_inWorld ? m_worldDepth.Get() : depth->gpu();
        if (whole && (!rects || !count))
        {
            if (flags & D3DCLEAR_TARGET)
            {
                cmd->clearTextureFloat(colorTarget, nvrhi::AllSubresources, nvrhi::Color(rgba[0], rgba[1], rgba[2], rgba[3]));
            }
            if (flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL))
            {
                cmd->clearDepthStencilTexture(depthTarget, nvrhi::AllSubresources, (flags & D3DCLEAR_ZBUFFER) != 0, z,
                    (flags & D3DCLEAR_STENCIL) != 0, static_cast<uint8_t>(stencil));
            }
            return D3D_OK;
        }
        const float s = m_inWorld ? m_worldScale : 1.0f;
        auto clearRect = [&](const RECT& r) {
            RECT clipped;
            if (IntersectRect(&clipped, &r, &viewport))
            {
                const RECT scaled = {static_cast<LONG>(clipped.left * s), static_cast<LONG>(clipped.top * s),
                    static_cast<LONG>(clipped.right * s), static_cast<LONG>(clipped.bottom * s)};
                Context::fill(colorTarget, depthTarget, scaled, rgba, (flags & D3DCLEAR_TARGET) != 0, (flags & D3DCLEAR_ZBUFFER) != 0, z);
            }
        };
        if (!rects || !count)
        {
            clearRect(viewport);
        }
        else
        {
            for (DWORD i = 0; i < count; ++i)
            {
                clearRect({rects[i].x1, rects[i].y1, rects[i].x2, rects[i].y2});
            }
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
        m_vsDirty = true;
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
        m_vsDirty = true;
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
        m_vsDirty = true;
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
        m_vsDirty = true;
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
        if (m_rs[i] != value)
        {
            m_rs[i] = value;
            m_vsDirty = m_psDirty = true;
            logRenderState(i, value);
        }
        return D3D_OK;
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
            Context::commands();
            surface->texture();
        }
        return D3D_OK;
    }

    HRESULT Device::DrawPrimitive(D3DPRIMITIVETYPE type, DWORD fvf, LPVOID verts, DWORD count, DWORD)
    {
        if (!verts)
        {
            return DDERR_INVALIDPARAMS;
        }
        if (!primitiveCount(type, count))
        {
            return D3D_OK;
        }
        std::scoped_lock lock(m_lock);
        return draw(type, {static_cast<const uint8_t*>(verts), fvf, Fvf::stride(fvf), count}, nullptr, 0);
    }

    HRESULT Device::DrawIndexedPrimitive(D3DPRIMITIVETYPE type, DWORD fvf, LPVOID verts, DWORD vertCount, LPWORD indices,
        DWORD indexCount, DWORD)
    {
        if (!verts || !indices)
        {
            return DDERR_INVALIDPARAMS;
        }
        if (!primitiveCount(type, indexCount) || !vertCount)
        {
            return D3D_OK;
        }
        std::scoped_lock lock(m_lock);
        return draw(type, {static_cast<const uint8_t*>(verts), fvf, Fvf::stride(fvf), vertCount}, indices, indexCount);
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
        if (!primitiveCount(type, count) || start >= buffer->vertices())
        {
            return D3D_OK;
        }
        count = std::min(count, buffer->vertices() - start);
        std::scoped_lock lock(m_lock);
        return draw(type, {buffer->data() + size_t(start) * buffer->stride(), buffer->fvf(), buffer->stride(), count}, nullptr, 0);
    }

    HRESULT Device::DrawIndexedPrimitiveVB(D3DPRIMITIVETYPE type, LPDIRECT3DVERTEXBUFFER7 vb, DWORD start, DWORD vertCount,
        LPWORD indices, DWORD indexCount, DWORD)
    {
        VertexBuffer* buffer = VertexBuffer::from(vb);
        if (!buffer || !indices)
        {
            return DDERR_INVALIDPARAMS;
        }
        if (!primitiveCount(type, indexCount) || !vertCount || start >= buffer->vertices())
        {
            return D3D_OK;
        }
        vertCount = std::min(vertCount, buffer->vertices() - start);
        std::scoped_lock lock(m_lock);
        return draw(type, {buffer->data() + size_t(start) * buffer->stride(), buffer->fvf(), buffer->stride(), vertCount},
            indices, indexCount);
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
            if ((surface == nullptr) != (m_textures[stage] == nullptr))
            {
                m_psDirty = true;
            }
            m_textures[stage] = surface;
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
        if (m_tss[stage][t] == value && t != D3DTSS_ADDRESS)
        {
            return D3D_OK;
        }
        m_tss[stage][t] = value;
        if (t == D3DTSS_ADDRESS)
        {
            m_tss[stage][D3DTSS_ADDRESSU] = m_tss[stage][D3DTSS_ADDRESSV] = value;
        }
        if (stage < kShaderStages)
        {
            m_vsDirty = m_psDirty = true;
        }
        logStageState(stage, t, value);
        return D3D_OK;
    }

    HRESULT Device::ValidateDevice(LPDWORD passes)
    {
        if (passes)
        {
            *passes = 1;
        }
        return D3D_OK;
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

namespace DDrawRhi::World
{
    void begin(IDirect3DDevice7* device, float scale)
    {
        if (Device* d = Device::from(device))
        {
            d->beginWorld(scale);
        }
    }

    void end(IDirect3DDevice7* device)
    {
        if (Device* d = Device::from(device))
        {
            d->endWorld();
        }
    }
}
