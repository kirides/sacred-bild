// The ground from static vertex buffers on the Direct3D 9 device (see ground.h): the ground shader (ground.hlsl), its
// vertex declaration and one index buffer of quads shared by all draws.
#include "ddraw9/device.h"
#include "ddraw9/gpu.h"
#include "log.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

namespace DDraw9
{
    namespace Ground
    {
        struct Mesh
        {
            d9::IDirect3DVertexBuffer9* buffer;
            uint32_t capacity;      // quads
        };
    }

    namespace
    {
#include "ddraw9/ground_shaders.inc"

        static_assert(sizeof(Ground::Vertex) == 28);

        constexpr UINT kTransformRegister = 250;
        constexpr UINT kMaxQuadsPerDraw = 16384;    // 16-bit indices from the draw's base vertex

        void logGroundFailure(const char* what, HRESULT hr)
        {
            static int logged = 0;
            if (logged < 8)
            {
                ++logged;
                LOG("Direct3D 9: ground: {} failed ({:08x})", what, static_cast<uint32_t>(hr));
            }
        }

        d9::D3DVERTEXELEMENT9 element(WORD offset, d9::D3DDECLTYPE type, d9::D3DDECLUSAGE usage, BYTE index)
        {
            return {0, offset, static_cast<BYTE>(type), static_cast<BYTE>(d9::D3DDECLMETHOD_DEFAULT),
                static_cast<BYTE>(usage), index};
        }
    }

    bool Device::initGround()
    {
        if (m_groundState)
        {
            return m_groundState > 0;
        }
        m_groundState = -1;
        const d9::D3DCAPS9& caps = Gpu::caps();
        if (caps.VertexShaderVersion < D3DVS_VERSION(2, 0) || caps.MaxVertexShaderConst < 256)
        {
            LOG("Direct3D 9: ground buffers off, the device has vertex shader version {:x} with {} constants (needs 2.0, 256)",
                caps.VertexShaderVersion & 0xFFFF, caps.MaxVertexShaderConst);
            return false;
        }
        const d9::D3DVERTEXELEMENT9 elements[] = {
            element(0, d9::D3DDECLTYPE_FLOAT2, d9::D3DDECLUSAGE_POSITION, 0),
            element(8, d9::D3DDECLTYPE_D3DCOLOR, d9::D3DDECLUSAGE_COLOR, 0),
            element(12, d9::D3DDECLTYPE_FLOAT2, d9::D3DDECLUSAGE_TEXCOORD, 0),
            element(20, d9::D3DDECLTYPE_FLOAT2, d9::D3DDECLUSAGE_TEXCOORD, 1),
            {0xFF, 0, static_cast<BYTE>(d9::D3DDECLTYPE_UNUSED), 0, 0, 0},
        };
        HRESULT hr = m_dev->CreateVertexDeclaration(elements, &m_groundDecl);
        if (SUCCEEDED(hr))
        {
            hr = m_dev->CreateVertexShader(reinterpret_cast<const DWORD*>(kGroundShader), &m_groundShader);
        }
        if (SUCCEEDED(hr))
        {
            hr = m_dev->CreateIndexBuffer(kMaxQuadsPerDraw * 6 * sizeof(WORD), D3DUSAGE_WRITEONLY, d9::D3DFMT_INDEX16,
                d9::D3DPOOL_DEFAULT, &m_quadIndices, nullptr);
        }
        void* data = nullptr;
        if (SUCCEEDED(hr))
        {
            hr = m_quadIndices->Lock(0, 0, &data, 0);
        }
        if (FAILED(hr) || !data)
        {
            LOG("Direct3D 9: ground buffers off, creating the shader or index buffer failed ({:08x})",
                static_cast<uint32_t>(hr));
            releaseGround();
            m_groundState = -1;
            return false;
        }
        auto* indices = static_cast<WORD*>(data);
        for (UINT q = 0; q < kMaxQuadsPerDraw; ++q)
        {
            const WORD v = static_cast<WORD>(q * 4);
            const WORD quad[6] = {v, static_cast<WORD>(v + 1), static_cast<WORD>(v + 2), static_cast<WORD>(v + 2),
                static_cast<WORD>(v + 1), static_cast<WORD>(v + 3)};
            std::copy(std::begin(quad), std::end(quad), indices + q * 6);
        }
        m_quadIndices->Unlock();
        m_groundState = 1;
        LOG("Direct3D 9: ground buffers ready (vs_2_0)");
        return true;
    }

    void Device::releaseGround()
    {
        if (m_groundShader)
        {
            m_groundShader->Release();
            m_groundShader = nullptr;
        }
        if (m_groundDecl)
        {
            m_groundDecl->Release();
            m_groundDecl = nullptr;
        }
        if (m_quadIndices)
        {
            if (m_ib9Bound == m_quadIndices)
            {
                m_ib9Bound = nullptr;
            }
            m_quadIndices->Release();
            m_quadIndices = nullptr;
        }
        m_groundConstantsSet = false;
    }

    bool Device::groundAvailable()
    {
        std::scoped_lock lock(m_lock);
        return initGround();
    }

    Ground::Mesh* Device::createGroundMesh(uint32_t capacity)
    {
        std::scoped_lock lock(m_lock);
        if (!initGround() || !capacity)
        {
            return nullptr;
        }
        // Dynamic: tiles are appended as they come into view (D3DLOCK_NOOVERWRITE, never over quads already drawn).
        d9::IDirect3DVertexBuffer9* buffer = nullptr;
        const HRESULT hr = m_dev->CreateVertexBuffer(capacity * 4 * sizeof(Ground::Vertex),
            D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, d9::D3DPOOL_DEFAULT, &buffer, nullptr);
        if (FAILED(hr))
        {
            logGroundFailure("CreateVertexBuffer", hr);
            return nullptr;
        }
        return new Ground::Mesh{buffer, capacity};
    }

    bool Device::writeGround(Ground::Mesh* mesh, uint32_t first, const Ground::Vertex* vertices, uint32_t count)
    {
        std::scoped_lock lock(m_lock);
        if (!mesh || first + count > mesh->capacity)
        {
            return false;
        }
        if (!count)
        {
            return true;
        }
        const UINT stride = 4 * sizeof(Ground::Vertex);
        void* data = nullptr;
        const HRESULT hr = mesh->buffer->Lock(first * stride, count * stride, &data, D3DLOCK_NOOVERWRITE);
        if (FAILED(hr) || !data)
        {
            logGroundFailure("Lock", hr);
            return false;
        }
        std::memcpy(data, vertices, size_t(count) * stride);
        mesh->buffer->Unlock();
        return true;
    }

    bool Device::drawGround(const Ground::Draw& draw)
    {
        std::scoped_lock lock(m_lock);
        if (!initGround() || !draw.mesh || draw.firstQuad + draw.quads > draw.mesh->capacity)
        {
            return false;
        }
        if (!draw.quads)
        {
            return true;
        }
        // What the fixed-function pipeline would do with pretransformed vertices that the shader doesn't.
        if (m_rs[D3DRENDERSTATE_FOGENABLE])
        {
            return false;
        }
        for (DWORD stage = 0; stage < kStages && m_tss[stage][D3DTSS_COLOROP] != D3DTOP_DISABLE; ++stage)
        {
            if (m_tss[stage][D3DTSS_TEXCOORDINDEX] > 1 || m_tss[stage][D3DTSS_TEXTURETRANSFORMFLAGS] != D3DTTFF_DISABLE)
            {
                return false;
            }
        }

        // Pretransformed vertices are in render target pixels; the shader's output goes through the viewport.
        const d9::D3DVIEWPORT9& vp = m_viewport9;
        if (!vp.Width || !vp.Height)
        {
            return true;
        }
        const float sx = 2.0f / static_cast<float>(vp.Width);
        const float sy = -2.0f / static_cast<float>(vp.Height);
        const float depthRange = vp.MaxZ - vp.MinZ;
        const float constants[2][4] = {
            {draw.scale * sx, draw.scale * sy, (draw.offsetX - static_cast<float>(vp.X)) * sx - 1.0f,
                (draw.offsetY - static_cast<float>(vp.Y)) * sy + 1.0f},
            {depthRange > 0.0f ? (draw.z - vp.MinZ) / depthRange : draw.z, 0.0f, 0.0f, 0.0f},
        };
        if (!m_groundConstantsSet || std::memcmp(constants, m_groundConstants, sizeof(constants)) != 0)
        {
            m_dev->SetVertexShaderConstantF(kTransformRegister, &constants[0][0], 2);
            std::memcpy(m_groundConstants, constants, sizeof(constants));
            m_groundConstantsSet = true;
        }

        prepareTarget();
        // The skinning shader's binding state stands for "a shader is bound": the next fixed-function draw unbinds it,
        // and the next skinned draw binds its own again.
        if (!m_skinBound || m_skinBoundVariant != -2)
        {
            m_dev->SetVertexDeclaration(m_groundDecl);
            m_dev->SetVertexShader(m_groundShader);
            m_skinBound = true;
            m_skinBoundVariant = -2;
            m_fvf9 = 0;
        }
        bindVertexBuffer(draw.mesh->buffer, sizeof(Ground::Vertex));
        if (m_ib9Bound != m_quadIndices)
        {
            m_ib9Bound = m_quadIndices;
            m_dev->SetIndices(m_quadIndices);
        }
        for (uint32_t done = 0; done < draw.quads;)
        {
            const uint32_t n = std::min(draw.quads - done, kMaxQuadsPerDraw);
            const HRESULT hr = m_dev->DrawIndexedPrimitive(d9::D3DPT_TRIANGLELIST, static_cast<INT>((draw.firstQuad + done) * 4),
                0, n * 4, 0, n * 2);
            if (FAILED(hr))
            {
                logGroundFailure("DrawIndexedPrimitive", hr);
                break;
            }
            done += n;
        }
        return true;
    }

    bool Ground::available(IDirect3DDevice7* device)
    {
        Device* d = Device::from(device);
        return d && d->groundAvailable();
    }

    Ground::Mesh* Ground::createMesh(IDirect3DDevice7* device, uint32_t capacity)
    {
        Device* d = Device::from(device);
        return d ? d->createGroundMesh(capacity) : nullptr;
    }

    void Ground::releaseMesh(Mesh* mesh)
    {
        if (mesh)
        {
            mesh->buffer->Release();
            delete mesh;
        }
    }

    uint32_t Ground::capacity(const Mesh* mesh)
    {
        return mesh ? mesh->capacity : 0;
    }

    bool Ground::write(IDirect3DDevice7* device, Mesh* mesh, uint32_t first, const Vertex* vertices, uint32_t count)
    {
        Device* d = Device::from(device);
        return d && d->writeGround(mesh, first, vertices, count);
    }

    bool Ground::draw(IDirect3DDevice7* device, const Draw& draw)
    {
        Device* d = Device::from(device);
        return d && d->drawGround(draw);
    }
}
