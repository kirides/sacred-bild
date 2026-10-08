// GPU skinning on the Direct3D 9 device (see skin.h): static per-mesh vertex buffers, the skinning shader (skin.hlsl)
// and its constants, taken from the Direct3D 7 state the game set for the fixed-function pipeline.
#include "ddraw9/device.h"
#include "ddraw9/gpu.h"
#include "log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace DDraw9
{
    namespace Skin
    {
        struct Mesh
        {
            d9::IDirect3DVertexBuffer9* buffer;
            uint32_t count;
        };

        // A piece's triangle list, in an index arena shared by all pieces (one SetIndices for all skinned draws).
        struct Indices
        {
            d9::IDirect3DIndexBuffer9* buffer;  // holds a reference
            uint32_t start;
            uint32_t count;
        };
    }

    namespace
    {
#include "ddraw9/skin_shaders.inc"

        static_assert(sizeof(Skin::Vertex) == 52);

        constexpr UINT kColorRegister = 16;
        constexpr UINT kLightRegister = 17;
        constexpr UINT kLightRegisters = 7;
        constexpr UINT kMaxLights = 8;
        constexpr UINT kBoneRegister = 73;
        constexpr UINT kDiffuseRing = 1u << 16;     // diffuse colors per pass through the ring
        constexpr UINT kIndexArena = 1u << 20;      // indices per arena (2 MB); all pieces of all meshes so far fit
        constexpr UINT kCameraRegisters = 10;       // world x view, projection, normal matrix: change per character

        using Float4 = float[4];

        void set(Float4& r, float x, float y, float z, float w)
        {
            r[0] = x;
            r[1] = y;
            r[2] = z;
            r[3] = w;
        }

        void set(Float4& r, const D3DCOLORVALUE& c)
        {
            set(r, c.r, c.g, c.b, c.a);
        }

        const float (*rows(const D3DMATRIX& m))[4]
        {
            return reinterpret_cast<const float(*)[4]>(&m);
        }

        D3DMATRIX multiply(const D3DMATRIX& a, const D3DMATRIX& b)
        {
            D3DMATRIX r;
            const float(*A)[4] = rows(a);
            const float(*B)[4] = rows(b);
            float(*R)[4] = reinterpret_cast<float(*)[4]>(&r);
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    R[i][j] = A[i][0] * B[0][j] + A[i][1] * B[1][j] + A[i][2] * B[2][j] + A[i][3] * B[3][j];
            return r;
        }

        // Inverse of the upper 3x3 of `m`; false if it is singular.
        bool invert3x3(const float (*m)[4], float out[3][3])
        {
            const float c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1];
            const float c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2];
            const float c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
            const float det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
            if (!(std::fabs(det) > 1e-20f))
            {
                return false;
            }
            const float inv = 1.0f / det;
            out[0][0] = c00 * inv;
            out[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * inv;
            out[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * inv;
            out[1][0] = c01 * inv;
            out[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * inv;
            out[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * inv;
            out[2][0] = c02 * inv;
            out[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * inv;
            out[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * inv;
            return true;
        }

        void logSkinFailure(const char* what, HRESULT hr)
        {
            static int logged = 0;
            if (logged < 8)
            {
                ++logged;
                LOG("Direct3D 9: GPU skinning: {} failed ({:08x})", what, static_cast<uint32_t>(hr));
            }
        }

        d9::D3DVERTEXELEMENT9 element(WORD stream, WORD offset, d9::D3DDECLTYPE type, d9::D3DDECLUSAGE usage)
        {
            return {stream, offset, static_cast<BYTE>(type), static_cast<BYTE>(d9::D3DDECLMETHOD_DEFAULT),
                static_cast<BYTE>(usage), 0};
        }
    }

    bool Device::initSkin()
    {
        if (m_skinState)
        {
            return m_skinState > 0;
        }
        m_skinState = -1;
        const d9::D3DCAPS9& caps = Gpu::caps();
        if (caps.VertexShaderVersion < D3DVS_VERSION(2, 0) || caps.MaxVertexShaderConst < 256)
        {
            LOG("Direct3D 9: GPU skinning off, the device has vertex shader version {:x} with {} constants (needs 2.0, 256)",
                caps.VertexShaderVersion & 0xFFFF, caps.MaxVertexShaderConst);
            return false;
        }
        const d9::D3DVERTEXELEMENT9 end = {0xFF, 0, static_cast<BYTE>(d9::D3DDECLTYPE_UNUSED), 0, 0, 0};
        const d9::D3DVERTEXELEMENT9 plain[] = {
            element(0, 0, d9::D3DDECLTYPE_FLOAT3, d9::D3DDECLUSAGE_POSITION),
            element(0, 12, d9::D3DDECLTYPE_FLOAT3, d9::D3DDECLUSAGE_NORMAL),
            element(0, 24, d9::D3DDECLTYPE_FLOAT2, d9::D3DDECLUSAGE_TEXCOORD),
            element(0, 32, d9::D3DDECLTYPE_D3DCOLOR, d9::D3DDECLUSAGE_BLENDINDICES),
            element(0, 36, d9::D3DDECLTYPE_FLOAT4, d9::D3DDECLUSAGE_BLENDWEIGHT),
            end,
        };
        const d9::D3DVERTEXELEMENT9 withDiffuse[] = {
            plain[0], plain[1], plain[2], plain[3], plain[4],
            element(1, 0, d9::D3DDECLTYPE_D3DCOLOR, d9::D3DDECLUSAGE_COLOR),
            end,
        };
        HRESULT hr = m_dev->CreateVertexDeclaration(plain, &m_skinDecls[0]);
        if (SUCCEEDED(hr))
        {
            hr = m_dev->CreateVertexDeclaration(withDiffuse, &m_skinDecls[1]);
        }
        if (SUCCEEDED(hr))
        {
            hr = m_dev->CreateVertexShader(reinterpret_cast<const DWORD*>(kSkinShader), &m_skinShaders[0]);
        }
        if (SUCCEEDED(hr))
        {
            hr = m_dev->CreateVertexShader(reinterpret_cast<const DWORD*>(kSkinShaderDiffuse), &m_skinShaders[1]);
        }
        if (FAILED(hr))
        {
            LOG("Direct3D 9: GPU skinning off, creating its shaders failed ({:08x})", static_cast<uint32_t>(hr));
            releaseSkin();
            m_skinState = -1;
            return false;
        }
        m_skinState = 1;
        LOG("Direct3D 9: GPU skinning ready (vs_2_0, up to {} bones and {} lights)", Skin::kMaxBones, kMaxLights);
        return true;
    }

    void Device::releaseSkin()
    {
        for (int i = 0; i < 2; ++i)
        {
            if (m_skinShaders[i])
            {
                m_skinShaders[i]->Release();
                m_skinShaders[i] = nullptr;
            }
            if (m_skinDecls[i])
            {
                m_skinDecls[i]->Release();
                m_skinDecls[i] = nullptr;
            }
        }
        if (m_skinDiffuse)
        {
            m_skinDiffuse->Release();
            m_skinDiffuse = nullptr;
        }
        if (m_skinArena)
        {
            m_skinArena->Release();
            m_skinArena = nullptr;
        }
        m_skinPalette.clear();
        m_skinPaletteId = 0;
        m_skinBound = false;
        m_skinConstantCount = 0;
        m_skinLightCount = -1;
    }

    bool Device::skinAvailable()
    {
        std::scoped_lock lock(m_lock);
        return initSkin();
    }

    Skin::Mesh* Device::createSkinMesh(const Skin::Vertex* vertices, uint32_t count)
    {
        std::scoped_lock lock(m_lock);
        if (!initSkin() || !vertices || !count)
        {
            return nullptr;
        }
        const UINT bytes = count * sizeof(Skin::Vertex);
        d9::IDirect3DVertexBuffer9* buffer = nullptr;
        HRESULT hr = m_dev->CreateVertexBuffer(bytes, D3DUSAGE_WRITEONLY, 0, d9::D3DPOOL_DEFAULT, &buffer, nullptr);
        if (FAILED(hr))
        {
            logSkinFailure("CreateVertexBuffer", hr);
            return nullptr;
        }
        void* data = nullptr;
        hr = buffer->Lock(0, 0, &data, 0);
        if (FAILED(hr) || !data)
        {
            logSkinFailure("Lock", hr);
            buffer->Release();
            return nullptr;
        }
        std::memcpy(data, vertices, bytes);
        buffer->Unlock();
        return new Skin::Mesh{buffer, count};
    }

    Skin::Indices* Device::createSkinIndices(const WORD* indices, uint32_t count)
    {
        std::scoped_lock lock(m_lock);
        if (!initSkin() || !indices || count < 3)
        {
            return nullptr;
        }
        // Appended to a dynamic arena without overwriting what earlier draws use; a full arena is left to the pieces
        // that reference it and a new one started.
        if (!m_skinArena || m_skinArenaUsed + count > m_skinArenaCapacity)
        {
            if (m_skinArena)
            {
                m_skinArena->Release();
                m_skinArena = nullptr;
            }
            const UINT capacity = std::max(kIndexArena, count);
            const HRESULT hr = m_dev->CreateIndexBuffer(capacity * sizeof(WORD), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                d9::D3DFMT_INDEX16, d9::D3DPOOL_DEFAULT, &m_skinArena, nullptr);
            if (FAILED(hr))
            {
                logSkinFailure("CreateIndexBuffer", hr);
                m_skinArena = nullptr;
                return nullptr;
            }
            m_skinArenaUsed = 0;
            m_skinArenaCapacity = capacity;
        }
        void* data = nullptr;
        const HRESULT hr = m_skinArena->Lock(m_skinArenaUsed * sizeof(WORD), count * sizeof(WORD), &data,
            m_skinArenaUsed ? D3DLOCK_NOOVERWRITE : D3DLOCK_DISCARD);
        if (FAILED(hr) || !data)
        {
            logSkinFailure("Lock (indices)", hr);
            return nullptr;
        }
        std::memcpy(data, indices, count * sizeof(WORD));
        m_skinArena->Unlock();
        m_skinArena->AddRef();
        auto* piece = new Skin::Indices{m_skinArena, m_skinArenaUsed, count};
        m_skinArenaUsed += count;
        return piece;
    }

    bool Device::skinConstants(const Skin::Draw& draw, bool diffuse)
    {
        // What the fixed-function pipeline would do that the shader doesn't: the caller skins on the CPU instead.
        const bool lighting = m_rs[D3DRENDERSTATE_LIGHTING] != 0;
        if (m_rs[D3DRENDERSTATE_FOGENABLE] || m_rs[D3DRENDERSTATE_CLIPPLANEENABLE] ||
            m_rs[D3DRENDERSTATE_VERTEXBLEND] != D3DVBLEND_DISABLE || (lighting && !draw.normals))
        {
            return false;
        }
        for (DWORD stage = 0; stage < kStages && m_tss[stage][D3DTSS_COLOROP] != D3DTOP_DISABLE; ++stage)
        {
            // The shader writes the coordinates to sets 0-3; generated coordinates have flags in the high bits.
            if (m_tss[stage][D3DTSS_TEXCOORDINDEX] > 3 || m_tss[stage][D3DTSS_TEXTURETRANSFORMFLAGS] != D3DTTFF_DISABLE)
            {
                return false;
            }
        }
        const D3DLIGHT7* lights[kMaxLights];
        UINT lightCount = 0;
        for (const Light& l : m_lights)
        {
            if (l.set && l.enabled)
            {
                if (lightCount == kMaxLights || (l.light.dltType != D3DLIGHT_POINT && l.light.dltType != D3DLIGHT_SPOT &&
                    l.light.dltType != D3DLIGHT_DIRECTIONAL))
                {
                    return false;
                }
                lights[lightCount++] = &l.light;
            }
        }

        // Camera space: world x view, its normal matrix (lit draws only: a shadow's world matrix flattens it onto the
        // ground and has no inverse), the projection (see skin.hlsl).
        Float4 c[kBoneRegister] = {};
        const D3DMATRIX worldView = multiply(m_transforms[D3DTRANSFORMSTATE_WORLD], m_transforms[D3DTRANSFORMSTATE_VIEW]);
        const float(*wv)[4] = rows(worldView);
        const float(*view)[4] = rows(m_transforms[D3DTRANSFORMSTATE_VIEW]);
        const float(*projection)[4] = rows(m_transforms[D3DTRANSFORMSTATE_PROJECTION]);
        float inverse[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        if (lighting && !invert3x3(wv, inverse))
        {
            return false;
        }
        for (int j = 0; j < 3; ++j)
        {
            set(c[j], wv[0][j], wv[1][j], wv[2][j], wv[3][j]);
            set(c[7 + j], inverse[j][0], inverse[j][1], inverse[j][2], 0.0f);
        }
        for (int j = 0; j < 4; ++j)
        {
            set(c[3 + j], projection[0][j], projection[1][j], projection[2][j], projection[3][j]);
        }

        // Material, global ambient, and which colors come from the vertex (D3DMCS_COLOR1: its diffuse color).
        set(c[10], m_material.dcvDiffuse);
        set(c[11], m_material.dcvAmbient);
        set(c[12], m_material.dcvSpecular);
        set(c[13], m_material.dcvEmissive);
        const DWORD ambient = m_rs[D3DRENDERSTATE_AMBIENT];
        set(c[14], ((ambient >> 16) & 0xFF) / 255.0f, ((ambient >> 8) & 0xFF) / 255.0f, (ambient & 0xFF) / 255.0f,
            m_material.dvPower);
        const DWORD color = diffuse ? 0xFFFFFFFF : draw.color;
        set(c[kColorRegister], ((color >> 16) & 0xFF) / 255.0f, ((color >> 8) & 0xFF) / 255.0f, (color & 0xFF) / 255.0f,
            (color >> 24) / 255.0f);
        const bool colorVertex = draw.vertexColor && m_rs[D3DRENDERSTATE_COLORVERTEX];
        auto fromVertex = [&](DWORD source) { return colorVertex && source == D3DMCS_COLOR1 ? 1.0f : 0.0f; };
        set(c[15], fromVertex(m_rs[D3DRENDERSTATE_DIFFUSEMATERIALSOURCE]), fromVertex(m_rs[D3DRENDERSTATE_AMBIENTMATERIALSOURCE]),
            fromVertex(m_rs[D3DRENDERSTATE_SPECULARMATERIALSOURCE]), fromVertex(m_rs[D3DRENDERSTATE_EMISSIVEMATERIALSOURCE]));

        // Lights in camera space, as computed for an earlier draw unless a light or the view changed.
        bool lightsChanged = static_cast<int>(lightCount) != m_skinLightsCached ||
            std::memcmp(&m_skinLightView, &m_transforms[D3DTRANSFORMSTATE_VIEW], sizeof(D3DMATRIX)) != 0;
        for (UINT i = 0; i < lightCount && !lightsChanged; ++i)
        {
            lightsChanged = std::memcmp(&m_skinLightKeys[i], lights[i], sizeof(D3DLIGHT7)) != 0;
        }
        if (lightsChanged)
        {
            m_skinLightsCached = static_cast<int>(lightCount);
            m_skinLightView = m_transforms[D3DTRANSFORMSTATE_VIEW];
        }
        for (UINT i = 0; i < lightCount && lightsChanged; ++i)
        {
            const D3DLIGHT7& l = *lights[i];
            m_skinLightKeys[i] = l;
            Float4* r = m_skinLightRegs + i * kLightRegisters;
            const D3DVECTOR& p = l.dvPosition;
            const D3DVECTOR& d = l.dvDirection;
            const float px = p.x * view[0][0] + p.y * view[1][0] + p.z * view[2][0] + view[3][0];
            const float py = p.x * view[0][1] + p.y * view[1][1] + p.z * view[2][1] + view[3][1];
            const float pz = p.x * view[0][2] + p.y * view[1][2] + p.z * view[2][2] + view[3][2];
            float dx = d.x * view[0][0] + d.y * view[1][0] + d.z * view[2][0];
            float dy = d.x * view[0][1] + d.y * view[1][1] + d.z * view[2][1];
            float dz = d.x * view[0][2] + d.y * view[1][2] + d.z * view[2][2];
            const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (length > 0.0f)
            {
                dx /= length;
                dy /= length;
                dz /= length;
            }
            const bool positional = l.dltType != D3DLIGHT_DIRECTIONAL;
            set(r[0], px, py, pz, l.dvRange);
            set(r[1], dx, dy, dz, l.dvFalloff);
            set(r[2], l.dcvDiffuse);
            set(r[3], l.dcvSpecular);
            set(r[4], l.dcvAmbient);
            set(r[5], l.dvAttenuation0, l.dvAttenuation1, l.dvAttenuation2, positional ? 1.0f : 0.0f);
            set(r[6], std::cos(l.dvTheta * 0.5f), std::cos(l.dvPhi * 0.5f), l.dltType == D3DLIGHT_SPOT ? 1.0f : 0.0f, 0.0f);
        }
        std::memcpy(c + kLightRegister, m_skinLightRegs, lightCount * kLightRegisters * sizeof(Float4));

        // Uploaded only where they changed, the camera (per character) apart from material and lights (rarely).
        const UINT count = kLightRegister + lightCount * kLightRegisters;
        if (!m_skinConstantCount || std::memcmp(c, m_skinConstants, kCameraRegisters * sizeof(Float4)) != 0)
        {
            m_dev->SetVertexShaderConstantF(0, &c[0][0], kCameraRegisters);
        }
        if (count != m_skinConstantCount || std::memcmp(c[kCameraRegisters], m_skinConstants[kCameraRegisters],
                (count - kCameraRegisters) * sizeof(Float4)) != 0)
        {
            m_dev->SetVertexShaderConstantF(kCameraRegisters, &c[kCameraRegisters][0], count - kCameraRegisters);
        }
        std::memcpy(m_skinConstants, c, count * sizeof(Float4));
        m_skinConstantCount = count;
        const BOOL flags[5] = {draw.normalizeSkinned, m_rs[D3DRENDERSTATE_NORMALIZENORMALS] != 0,
            m_rs[D3DRENDERSTATE_LOCALVIEWER] != 0, m_rs[D3DRENDERSTATE_SPECULARENABLE] != 0, lighting};
        if (std::memcmp(flags, m_skinFlags, sizeof(flags)) != 0)
        {
            m_dev->SetVertexShaderConstantB(0, flags, 5);
            std::memcpy(m_skinFlags, flags, sizeof(flags));
        }
        if (static_cast<int>(lightCount) != m_skinLightCount)
        {
            const int loop[4] = {static_cast<int>(lightCount), 0, 0, 0};
            m_dev->SetVertexShaderConstantI(0, loop, 1);
            m_skinLightCount = static_cast<int>(lightCount);
        }

        // Bones: Granny's matrix rows with the translation in w. A character's shadow pass computes the same pose
        // again (another palette id, the same values): compared before uploading.
        const size_t paletteFloats = size_t(draw.bones) * 12;
        if (draw.paletteId != m_skinPaletteId && m_skinPalette.size() == paletteFloats &&
            std::memcmp(m_skinPalette.data(), draw.palette, paletteFloats * sizeof(float)) == 0)
        {
            m_skinPaletteId = draw.paletteId;
        }
        if (draw.paletteId != m_skinPaletteId)
        {
            m_skinPalette.assign(draw.palette, draw.palette + paletteFloats);
            Float4 bones[Skin::kMaxBones * 3];
            for (uint32_t b = 0; b < draw.bones; ++b)
            {
                const float* m = draw.palette + size_t(b) * 12;
                for (int r = 0; r < 3; ++r)
                {
                    set(bones[b * 3 + r], m[r * 3], m[r * 3 + 1], m[r * 3 + 2], m[9 + r]);
                }
            }
            m_dev->SetVertexShaderConstantF(kBoneRegister, &bones[0][0], draw.bones * 3);
            m_skinPaletteId = draw.paletteId;
        }
        return true;
    }

    bool Device::drawSkinned(const Skin::Draw& draw)
    {
        std::scoped_lock lock(m_lock);
        if (!initSkin() || !draw.mesh || !draw.indices || !draw.palette || draw.bones == 0 ||
            draw.bones > Skin::kMaxBones || draw.paletteId == 0)
        {
            return false;
        }
        const bool diffuse = draw.diffuse != nullptr;
        const UINT vertices = draw.mesh->count;
        if (diffuse && vertices > kDiffuseRing)
        {
            return false;
        }
        if (!skinConstants(draw, diffuse))
        {
            return false;
        }

        // Per-vertex diffuse colors into the ring.
        UINT diffuseOffset = 0;
        if (diffuse)
        {
            if (!m_skinDiffuse)
            {
                const HRESULT hr = m_dev->CreateVertexBuffer(kDiffuseRing * 4, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0,
                    d9::D3DPOOL_DEFAULT, &m_skinDiffuse, nullptr);
                if (FAILED(hr))
                {
                    logSkinFailure("CreateVertexBuffer (diffuse)", hr);
                    m_skinDiffuse = nullptr;
                    return false;
                }
                m_skinDiffuseCursor = kDiffuseRing;     // the first lock discards
            }
            DWORD lockFlags = D3DLOCK_NOOVERWRITE;
            if (m_skinDiffuseCursor + vertices > kDiffuseRing)
            {
                m_skinDiffuseCursor = 0;
                lockFlags = D3DLOCK_DISCARD;
            }
            void* data = nullptr;
            if (FAILED(m_skinDiffuse->Lock(m_skinDiffuseCursor * 4, vertices * 4, &data, lockFlags)) || !data)
            {
                return false;
            }
            auto* out = static_cast<DWORD*>(data);
            const auto* in = reinterpret_cast<const uint8_t*>(draw.diffuse);
            for (UINT v = 0; v < vertices; ++v)
            {
                std::memcpy(out + v, in + size_t(v) * draw.diffuseStride, 4);
            }
            m_skinDiffuse->Unlock();
            diffuseOffset = m_skinDiffuseCursor * 4;
            m_skinDiffuseCursor += vertices;
        }

        prepareTarget();
        if (!m_skinBound || m_skinBoundVariant != int(diffuse))
        {
            m_dev->SetVertexDeclaration(m_skinDecls[diffuse]);
            m_dev->SetVertexShader(m_skinShaders[diffuse]);
            m_skinBound = true;
            m_skinBoundVariant = diffuse;
            m_fvf9 = 0;     // the declaration replaced the FVF: the next fixed-function draw sets its own
        }
        bindVertexBuffer(draw.mesh->buffer, sizeof(Skin::Vertex));
        if (diffuse)
        {
            m_dev->SetStreamSource(1, m_skinDiffuse, diffuseOffset, 4);
        }
        if (m_ib9Bound != draw.indices->buffer)
        {
            m_ib9Bound = draw.indices->buffer;
            m_dev->SetIndices(draw.indices->buffer);
        }
        const HRESULT hr = m_dev->DrawIndexedPrimitive(d9::D3DPT_TRIANGLELIST, 0, 0, vertices, draw.indices->start,
            draw.indices->count / 3);
        if (FAILED(hr))
        {
            logSkinFailure("DrawIndexedPrimitive", hr);
        }
        return true;
    }

    bool Skin::available(IDirect3DDevice7* device)
    {
        Device* d = Device::from(device);
        return d && d->skinAvailable();
    }

    Skin::Mesh* Skin::createMesh(IDirect3DDevice7* device, const Vertex* vertices, uint32_t count)
    {
        Device* d = Device::from(device);
        return d ? d->createSkinMesh(vertices, count) : nullptr;
    }

    void Skin::releaseMesh(Mesh* mesh)
    {
        if (mesh)
        {
            mesh->buffer->Release();
            delete mesh;
        }
    }

    Skin::Indices* Skin::createIndices(IDirect3DDevice7* device, const WORD* indices, uint32_t count)
    {
        Device* d = Device::from(device);
        return d ? d->createSkinIndices(indices, count) : nullptr;
    }

    void Skin::releaseIndices(Indices* indices)
    {
        if (indices)
        {
            indices->buffer->Release();
            delete indices;
        }
    }

    bool Skin::draw(IDirect3DDevice7* device, const Draw& draw)
    {
        Device* d = Device::from(device);
        return d && d->drawSkinned(draw);
    }
}
