#include "render/batcher.h"
#include "render/atlas.h"
#include "render/fvf.h"
#include "game/d3d_stats.h"
#include "log.h"

#include <algorithm>
#include <cfloat>
#include <cstring>

using namespace D3DStats;

namespace
{
    enum : uint8_t
    {
        Known = 1,      // the game's value is recorded
        Applied = 2,    // the device's value is known
        Dirty = 4,      // in the dirty list: recorded value may differ from the device
    };

    // One pending batch: 16-bit indices, and well within what the runtime accepts per call.
    constexpr DWORD kMaxVerts = 0x4000;
    constexpr DWORD kMaxIndices = 0x6000;
    // Vertices per submit vertex buffer (one per vertex format): several batches before it wraps.
    constexpr DWORD kVertexBufferSize = 0xC000;

    // Render states that only affect vertices Direct3D transforms and lights; pretransformed draws ignore them.
    bool tnlOnly(DWORD state)
    {
        switch (state)
        {
        case D3DRENDERSTATE_LIGHTING:
        case D3DRENDERSTATE_AMBIENT:
        case D3DRENDERSTATE_FOGVERTEXMODE:
        case D3DRENDERSTATE_COLORVERTEX:
        case D3DRENDERSTATE_LOCALVIEWER:
        case D3DRENDERSTATE_NORMALIZENORMALS:
        case D3DRENDERSTATE_DIFFUSEMATERIALSOURCE:
        case D3DRENDERSTATE_SPECULARMATERIALSOURCE:
        case D3DRENDERSTATE_AMBIENTMATERIALSOURCE:
        case D3DRENDERSTATE_EMISSIVEMATERIALSOURCE:
        case D3DRENDERSTATE_VERTEXBLEND:
        case D3DRENDERSTATE_CLIPPLANEENABLE:
            return true;
        }
        return false;
    }

    Counter flushCounter(Batcher::Reason reason)
    {
        switch (reason)
        {
        case Batcher::Reason::Texture: return CFlushTexture;
        case Batcher::Reason::RenderState: return CFlushRenderState;
        case Batcher::Reason::StageState: return CFlushStageState;
        case Batcher::Reason::Viewport: return CFlushViewport;
        case Batcher::Reason::Format: return CFlushFormat;
        case Batcher::Reason::Full: return CFlushFull;
        case Batcher::Reason::Direct: return CFlushDirect;
        case Batcher::Reason::Atlas: return CFlushAtlas;
        case Batcher::Reason::Lighting: return CFlushLighting;
        case Batcher::Reason::World: return CFlushWorld;
        case Batcher::Reason::Other: break;
        }
        return CFlushOther;
    }

    int floorPowerOfTwo(int v)
    {
        int p = 1;
        while (p * 2 <= v)
        {
            p *= 2;
        }
        return p;
    }
}

Batcher::Batcher(IDirect3DDevice7* real, IDirectDraw7* ddraw, const Options& options)
    : m_real(real), m_submitFlags(options.noClip ? D3DDP_DONOTCLIP : 0), m_batchModels(options.models)
{
    TextureAtlas::Options atlas;
    atlas.copies = options.atlas && ddraw;
    atlas.pageSize = floorPowerOfTwo(std::max(options.atlasPageSize, 1));
    atlas.maxPagesPerFormat = std::max(options.atlasPages, 0);
    atlas.maxTextureSize = options.atlasMaxTextureSize;
    D3DDEVICEDESC7 caps = {};
    if (atlas.copies && SUCCEEDED(m_real->GetCaps(&caps)) && caps.dwMaxTextureWidth && caps.dwMaxTextureHeight)
    {
        atlas.pageSize = std::min(atlas.pageSize,
            floorPowerOfTwo(static_cast<int>(std::min(caps.dwMaxTextureWidth, caps.dwMaxTextureHeight))));
    }
    if (atlas.pageSize < 1024 || atlas.maxPagesPerFormat == 0 || atlas.maxTextureSize < 1)
    {
        atlas.copies = false;
    }
    m_useAtlas = atlas.copies;
    m_atlas = std::make_unique<TextureAtlas>(ddraw, atlas, &Batcher::beforeAtlasChange, this);
    if (options.vertexBuffers && FAILED(m_real->GetDirect3D(&m_d3d)))
    {
        m_d3d = nullptr;
    }
    m_verts.resize(size_t(kMaxVerts) * 64);
    m_indices.resize(kMaxIndices);
    m_rsDirty.reserve(kStates);
    m_tssDirty.reserve(kStages * kStageTypes);
    const char* submits = m_d3d ? "vertex buffers" : "user memory";
    if (m_useAtlas)
    {
        LOG("Batcher: on, submits from {}{}, atlas pages up to {}x{}, up to {} per texture format, textures up to {}x{}",
            submits, options.noClip ? " without software clipping" : "", atlas.pageSize, atlas.pageSize,
            atlas.maxPagesPerFormat, atlas.maxTextureSize, atlas.maxTextureSize);
    }
    else
    {
        LOG("Batcher: on, submits from {}{}, no texture atlas", submits,
            options.noClip ? " without software clipping" : "");
    }
}

Batcher::~Batcher()
{
    for (VertexBuffer& vb : m_vertexBuffers)
    {
        vb.buffer->Release();
    }
    if (m_d3d)
    {
        m_d3d->Release();
    }
}

const Batcher::Layout& Batcher::layout(DWORD fvf)
{
    for (const Layout& l : m_layouts)
    {
        if (l.fvf == fvf)
        {
            return l;
        }
    }
    Layout& l = m_layouts[m_nextLayout++ % std::size(m_layouts)];
    l.fvf = fvf;
    l.stride = Fvf::stride(fvf);
    l.texCount = std::min<UINT>(Fvf::texCount(fvf), 8);
    for (UINT set = 0; set < 8; ++set)
    {
        const bool used = set < l.texCount;
        l.setOffset[set] = used ? Fvf::texCoordOffset(fvf, set) : 0;
        l.setBytes[set] = used ? Fvf::texCoordSize(fvf, set) * 4 : 0;
        l.texOffset[set] = used && Fvf::texCoordSize(fvf, set) == 2 ? l.setOffset[set] : 0;
    }
    // Components in FVF order after the position (models only use XYZ positions, 12 bytes).
    UINT offset = Fvf::texCoordOffset(fvf & (D3DFVF_POSITION_MASK | D3DFVF_RESERVED1), 0);
    l.normalOffset = (fvf & D3DFVF_NORMAL) ? offset : 0;
    offset += (fvf & D3DFVF_NORMAL) ? 12 : 0;
    l.diffuseOffset = (fvf & D3DFVF_DIFFUSE) ? offset : 0;
    offset += (fvf & D3DFVF_DIFFUSE) ? 4 : 0;
    l.specularOffset = (fvf & D3DFVF_SPECULAR) ? offset : 0;
    return l;
}

DWORD Batcher::triangles(D3DPRIMITIVETYPE type, DWORD count)
{
    if (type == D3DPT_TRIANGLELIST)
    {
        return count / 3;
    }
    if (type == D3DPT_TRIANGLESTRIP || type == D3DPT_TRIANGLEFAN)
    {
        return count >= 3 ? count - 2 : 0;
    }
    return 0;
}

void Batcher::beforeAtlasChange(void* self)
{
    static_cast<Batcher*>(self)->submit(Reason::Atlas);
}

void Batcher::begin()
{
    m_active = true;
    ++m_frame;
    invalidate();
    m_atlas->beginFrame();
}

void Batcher::end()
{
    sync();
    m_active = false;
}

void Batcher::invalidate()
{
    std::memset(m_rsFlags, 0, sizeof(m_rsFlags));
    std::memset(m_tssFlags, 0, sizeof(m_tssFlags));
    std::memset(m_texFlags, 0, sizeof(m_texFlags));
    m_vpFlags = 0;
    m_rsDirty.clear();
    m_tssDirty.clear();
    m_worldFlags = 0;
    std::memset(m_tnlRsKnown, 0, sizeof(m_tnlRsKnown));
    m_materialKnown = false;
    std::memset(m_lightKnown, 0, sizeof(m_lightKnown));
    std::memset(m_lightEnabledKnown, 0, sizeof(m_lightEnabledKnown));
}

DWORD Batcher::renderState(DWORD state)
{
    if (!(m_rsFlags[state] & Known))
    {
        // Nothing recorded since the last invalidate: the device holds the game's value.
        DWORD value = 0;
        m_real->GetRenderState(static_cast<D3DRENDERSTATETYPE>(state), &value);
        m_rs[state] = m_rsDevice[state] = value;
        m_rsFlags[state] |= Known | Applied;
    }
    return m_rs[state];
}

DWORD Batcher::stageState(DWORD stage, DWORD type)
{
    if (type == D3DTSS_ADDRESS)
    {
        type = D3DTSS_ADDRESSU;
    }
    const DWORD i = stage * kStageTypes + type;
    if (!(m_tssFlags[i] & Known))
    {
        DWORD value = 0;
        m_real->GetTextureStageState(stage, static_cast<D3DTEXTURESTAGESTATETYPE>(type), &value);
        m_tss[i] = m_tssDevice[i] = value;
        m_tssFlags[i] |= Known | Applied;
    }
    return m_tss[i];
}

IDirectDrawSurface7* Batcher::texture(DWORD stage)
{
    if (!(m_texFlags[stage] & Known))
    {
        IDirectDrawSurface7* texture = nullptr;
        m_real->GetTexture(stage, &texture);
        if (texture)
        {
            texture->Release();     // the device keeps its own reference
            m_atlas->entry(texture);
        }
        m_tex[stage] = m_texDevice[stage] = texture;
        m_texFlags[stage] |= Known | Applied;
    }
    return m_tex[stage];
}

void Batcher::drainDestroyed()
{
    if (!TextureAtlas::destroyedPending())
    {
        return;
    }
    m_destroyed.clear();
    m_atlas->drain(m_destroyed);
    // The device holds a reference to what it has bound, so only recorded bindings can point at a destroyed
    // texture: the game released it while it was still selected. Select nothing instead of a dangling pointer.
    for (IDirectDrawSurface7* texture : m_destroyed)
    {
        for (DWORD stage = 0; stage < kStages; ++stage)
        {
            if ((m_texFlags[stage] & Known) && m_tex[stage] == texture)
            {
                m_tex[stage] = nullptr;
            }
        }
    }
}

HRESULT Batcher::setRenderState(DWORD state, DWORD value)
{
    if (state >= kStates)
    {
        sync();
        return m_real->SetRenderState(static_cast<D3DRENDERSTATETYPE>(state), value);
    }
    if (tnlOnly(state))
    {
        // Applied right away: only a pending model batch depends on it.
        if (m_tnlRsKnown[state] && m_tnlRs[state] == value)
        {
            return D3D_OK;
        }
        endModelBatch();
        m_tnlRs[state] = value;
        m_tnlRsKnown[state] = true;
        Scope s{TState};
        return m_real->SetRenderState(static_cast<D3DRENDERSTATETYPE>(state), value);
    }
    uint8_t& flags = m_rsFlags[state];
    if ((flags & Known) && m_rs[state] == value)
    {
        return D3D_OK;
    }
    m_rs[state] = value;
    flags |= Known;
    if (!(flags & Dirty))
    {
        flags |= Dirty;
        m_rsDirty.push_back(static_cast<uint16_t>(state));
    }
    return D3D_OK;
}

HRESULT Batcher::getRenderState(DWORD state, DWORD* value)
{
    if (!value)
    {
        return DDERR_INVALIDPARAMS;
    }
    if (state >= kStates)
    {
        return m_real->GetRenderState(static_cast<D3DRENDERSTATETYPE>(state), value);
    }
    *value = tnlOnly(state) ? tnlRenderState(state) : renderState(state);
    return D3D_OK;
}

DWORD Batcher::tnlRenderState(DWORD state)
{
    if (!m_tnlRsKnown[state])
    {
        DWORD value = 0;
        m_real->GetRenderState(static_cast<D3DRENDERSTATETYPE>(state), &value);
        m_tnlRs[state] = value;
        m_tnlRsKnown[state] = true;
    }
    return m_tnlRs[state];
}

void Batcher::endModelBatch()
{
    if (m_kind == Kind::Model && m_indexCount)
    {
        submit(Reason::Lighting);
    }
}

void Batcher::beforeModelStateChange()
{
    endModelBatch();
}

const D3DMATRIX& Batcher::world()
{
    if (!(m_worldFlags & Known))
    {
        m_real->GetTransform(D3DTRANSFORMSTATE_WORLD, &m_world);
        m_worldDevice = m_world;
        m_worldFlags |= Known | Applied;
    }
    return m_world;
}

void Batcher::bindWorld(const D3DMATRIX& m)
{
    if ((m_worldFlags & Applied) && std::memcmp(&m_worldDevice, &m, sizeof(m)) == 0)
    {
        return;
    }
    Scope s{TState};
    m_real->SetTransform(D3DTRANSFORMSTATE_WORLD, const_cast<D3DMATRIX*>(&m));
    m_worldDevice = m;
    m_worldFlags |= Applied;
}

HRESULT Batcher::setTransform(D3DTRANSFORMSTATETYPE type, const D3DMATRIX& m)
{
    if (type == D3DTRANSFORMSTATE_WORLD && m_batchModels)
    {
        // Only recorded: a pending model batch keeps the matrix it was started with.
        m_world = m;
        m_worldFlags |= Known;
        return D3D_OK;
    }
    endModelBatch();
    if (type == D3DTRANSFORMSTATE_WORLD)
    {
        m_world = m_worldDevice = m;
        m_worldFlags |= Known | Applied;
    }
    Scope s{TState};
    return m_real->SetTransform(type, const_cast<D3DMATRIX*>(&m));
}

HRESULT Batcher::getWorld(D3DMATRIX* m)
{
    if (!m)
    {
        return DDERR_INVALIDPARAMS;
    }
    *m = world();
    return D3D_OK;
}

HRESULT Batcher::setMaterial(const D3DMATERIAL7& material)
{
    if (m_materialKnown && std::memcmp(&m_material, &material, sizeof(material)) == 0)
    {
        return D3D_OK;
    }
    endModelBatch();
    const HRESULT hr = m_real->SetMaterial(const_cast<D3DMATERIAL7*>(&material));
    m_material = material;
    m_materialKnown = SUCCEEDED(hr);
    return hr;
}

HRESULT Batcher::setLight(DWORD index, const D3DLIGHT7& light)
{
    if (index < kCachedLights && m_lightKnown[index] && std::memcmp(&m_lights[index], &light, sizeof(light)) == 0)
    {
        return D3D_OK;
    }
    endModelBatch();
    const HRESULT hr = m_real->SetLight(index, const_cast<D3DLIGHT7*>(&light));
    if (index < kCachedLights)
    {
        m_lights[index] = light;
        m_lightKnown[index] = SUCCEEDED(hr);
    }
    return hr;
}

HRESULT Batcher::lightEnable(DWORD index, BOOL enable)
{
    if (index < kCachedLights && m_lightEnabledKnown[index] && !m_lightEnabled[index] == !enable)
    {
        return D3D_OK;
    }
    endModelBatch();
    const HRESULT hr = m_real->LightEnable(index, enable);
    if (index < kCachedLights)
    {
        m_lightEnabled[index] = enable;
        m_lightEnabledKnown[index] = SUCCEEDED(hr);
    }
    return hr;
}

HRESULT Batcher::setStageState(DWORD stage, DWORD type, DWORD value)
{
    if (stage >= kStages || type >= kStageTypes)
    {
        sync();
        return m_real->SetTextureStageState(stage, static_cast<D3DTEXTURESTAGESTATETYPE>(type), value);
    }
    if (type == D3DTSS_ADDRESS)
    {
        setStageState(stage, D3DTSS_ADDRESSU, value);
        return setStageState(stage, D3DTSS_ADDRESSV, value);
    }
    const DWORD i = stage * kStageTypes + type;
    uint8_t& flags = m_tssFlags[i];
    if ((flags & Known) && m_tss[i] == value)
    {
        return D3D_OK;
    }
    m_tss[i] = value;
    flags |= Known;
    if (!(flags & Dirty))
    {
        flags |= Dirty;
        m_tssDirty.push_back(static_cast<uint16_t>(i));
    }
    return D3D_OK;
}

HRESULT Batcher::getStageState(DWORD stage, DWORD type, DWORD* value)
{
    if (!value)
    {
        return DDERR_INVALIDPARAMS;
    }
    if (stage >= kStages || type >= kStageTypes)
    {
        return m_real->GetTextureStageState(stage, static_cast<D3DTEXTURESTAGESTATETYPE>(type), value);
    }
    *value = stageState(stage, type);
    return D3D_OK;
}

HRESULT Batcher::setTexture(DWORD stage, IDirectDrawSurface7* texture)
{
    if (stage >= kStages)
    {
        sync();
        return m_real->SetTexture(stage, texture);
    }
    drainDestroyed();
    if (texture)
    {
        m_atlas->entry(texture);    // registered so its destruction is noticed while only recorded
    }
    m_tex[stage] = texture;
    m_texFlags[stage] |= Known;
    return D3D_OK;
}

HRESULT Batcher::getTexture(DWORD stage, IDirectDrawSurface7** texture)
{
    if (!texture)
    {
        return DDERR_INVALIDPARAMS;
    }
    if (stage >= kStages)
    {
        return m_real->GetTexture(stage, texture);
    }
    drainDestroyed();
    *texture = this->texture(stage);
    if (*texture)
    {
        (*texture)->AddRef();
    }
    return D3D_OK;
}

HRESULT Batcher::setViewport(const D3DVIEWPORT7& vp)
{
    if ((m_vpFlags & Known) && std::memcmp(&m_vp, &vp, sizeof(vp)) == 0)
    {
        return D3D_OK;
    }
    m_vp = vp;
    m_vpFlags |= Known | Dirty;
    return D3D_OK;
}

HRESULT Batcher::getViewport(D3DVIEWPORT7* vp)
{
    if (!vp)
    {
        return DDERR_INVALIDPARAMS;
    }
    if (!(m_vpFlags & Known))
    {
        m_real->GetViewport(&m_vp);
        m_vpDevice = m_vp;
        m_vpFlags |= Known | Applied;
    }
    *vp = m_vp;
    return D3D_OK;
}

bool Batcher::stateChanged(Reason& reason)
{
    for (size_t i = 0; i < m_rsDirty.size();)
    {
        const uint16_t state = m_rsDirty[i];
        if (!(m_rsFlags[state] & Applied) || m_rsDevice[state] != m_rs[state])
        {
            reason = Reason::RenderState;
            onFlushCause(state);
            return true;
        }
        m_rsFlags[state] &= ~Dirty;     // changed and changed back: nothing to do
        m_rsDirty[i] = m_rsDirty.back();
        m_rsDirty.pop_back();
    }
    for (size_t i = 0; i < m_tssDirty.size();)
    {
        const uint16_t index = m_tssDirty[i];
        if (!(m_tssFlags[index] & Applied) || m_tssDevice[index] != m_tss[index])
        {
            reason = Reason::StageState;
            onFlushCause(0x10000u | (uint32_t(index / kStageTypes) << 8) | (index % kStageTypes));
            return true;
        }
        m_tssFlags[index] &= ~Dirty;
        m_tssDirty[i] = m_tssDirty.back();
        m_tssDirty.pop_back();
    }
    if (m_vpFlags & Dirty)
    {
        if (!(m_vpFlags & Applied) || std::memcmp(&m_vp, &m_vpDevice, sizeof(m_vp)) != 0)
        {
            reason = Reason::Viewport;
            return true;
        }
        m_vpFlags &= ~Dirty;
    }
    return false;
}

void Batcher::applyStates()
{
    if (m_rsDirty.empty() && m_tssDirty.empty() && !(m_vpFlags & Dirty))
    {
        return;
    }
    Scope s{TState};
    for (uint16_t state : m_rsDirty)
    {
        if (!(m_rsFlags[state] & Applied) || m_rsDevice[state] != m_rs[state])
        {
            m_real->SetRenderState(static_cast<D3DRENDERSTATETYPE>(state), m_rs[state]);
            m_rsDevice[state] = m_rs[state];
        }
        m_rsFlags[state] = static_cast<uint8_t>((m_rsFlags[state] | Applied) & ~Dirty);
    }
    m_rsDirty.clear();
    for (uint16_t index : m_tssDirty)
    {
        if (!(m_tssFlags[index] & Applied) || m_tssDevice[index] != m_tss[index])
        {
            m_real->SetTextureStageState(index / kStageTypes, static_cast<D3DTEXTURESTAGESTATETYPE>(index % kStageTypes),
                m_tss[index]);
            m_tssDevice[index] = m_tss[index];
        }
        m_tssFlags[index] = static_cast<uint8_t>((m_tssFlags[index] | Applied) & ~Dirty);
    }
    m_tssDirty.clear();
    if (m_vpFlags & Dirty)
    {
        if (!(m_vpFlags & Applied) || std::memcmp(&m_vp, &m_vpDevice, sizeof(m_vp)) != 0)
        {
            m_real->SetViewport(&m_vp);
            m_vpDevice = m_vp;
        }
        m_vpFlags = static_cast<uint8_t>((m_vpFlags | Applied) & ~Dirty);
    }
}

void Batcher::bindTexture(DWORD stage, IDirectDrawSurface7* texture)
{
    if ((m_texFlags[stage] & Applied) && m_texDevice[stage] == texture)
    {
        return;
    }
    Scope s{TState};
    m_real->SetTexture(stage, texture);
    m_texDevice[stage] = texture;
    m_texFlags[stage] |= Applied;
}

void Batcher::submit(Reason reason)
{
    if (!m_indexCount)
    {
        return;
    }
    {
        Scope s{TDraw};
        if (!m_d3d || !submitVertexBuffer())
        {
            m_real->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, m_fvf, m_verts.data(), m_vertCount, m_indices.data(),
                m_indexCount, submitFlags());
        }
    }
    count(CSubmit);
    count(flushCounter(reason));
    m_vertCount = 0;
    m_indexCount = 0;
}

// A vertex buffer spares the runtime and the driver copying the vertices out of user memory again.
bool Batcher::submitVertexBuffer()
{
    VertexBuffer* vb = vertexBuffer(m_fvf);
    if (!vb)
    {
        return false;
    }
    DWORD lockFlags = DDLOCK_WAIT | DDLOCK_WRITEONLY | DDLOCK_NOOVERWRITE;
    if (vb->cursor + m_vertCount > kVertexBufferSize)
    {
        vb->cursor = 0;
        lockFlags = DDLOCK_WAIT | DDLOCK_WRITEONLY | DDLOCK_DISCARDCONTENTS;
    }
    void* data = nullptr;
    DWORD size = 0;
    HRESULT hr = vb->buffer->Lock(lockFlags, &data, &size);
    if (FAILED(hr) || !data)
    {
        vertexBufferFailed("Lock", hr);
        return false;
    }
    std::memcpy(static_cast<uint8_t*>(data) + size_t(vb->cursor) * m_stride, m_verts.data(), size_t(m_vertCount) * m_stride);
    vb->buffer->Unlock();
    hr = m_real->DrawIndexedPrimitiveVB(D3DPT_TRIANGLELIST, vb->buffer, vb->cursor, m_vertCount, m_indices.data(),
        m_indexCount, submitFlags());
    vb->cursor += m_vertCount;
    if (FAILED(hr))
    {
        vertexBufferFailed("DrawIndexedPrimitiveVB", hr);
        return false;
    }
    return true;
}

Batcher::VertexBuffer* Batcher::vertexBuffer(DWORD fvf)
{
    for (VertexBuffer& vb : m_vertexBuffers)
    {
        if (vb.fvf == fvf)
        {
            return &vb;
        }
    }
    D3DVERTEXBUFFERDESC desc = {};
    desc.dwSize = sizeof(desc);
    desc.dwCaps = D3DVBCAPS_WRITEONLY |
        ((m_submitFlags & D3DDP_DONOTCLIP) && Fvf::pretransformed(fvf) ? D3DVBCAPS_DONOTCLIP : 0);
    desc.dwFVF = fvf;
    desc.dwNumVertices = kVertexBufferSize;
    IDirect3DVertexBuffer7* buffer = nullptr;
    const HRESULT hr = m_d3d->CreateVertexBuffer(&desc, &buffer, 0);
    if (FAILED(hr) || !buffer)
    {
        vertexBufferFailed("CreateVertexBuffer", hr);
        return nullptr;
    }
    LOG("Batcher: vertex buffer for FVF {:x}, {} vertices", fvf, kVertexBufferSize);
    m_vertexBuffers.push_back({buffer, fvf, 0});
    return &m_vertexBuffers.back();
}

void Batcher::vertexBufferFailed(const char* what, HRESULT hr)
{
    LOG("Batcher: {} failed: {:08x}", what, static_cast<uint32_t>(hr));
    if (++m_vertexBufferFailures >= 3 && m_d3d)
    {
        LOG("Batcher: submitting from user memory from now on");
        for (VertexBuffer& vb : m_vertexBuffers)
        {
            vb.buffer->Release();
        }
        m_vertexBuffers.clear();
        m_d3d->Release();
        m_d3d = nullptr;
    }
}

void Batcher::sync(Reason reason)
{
    drainDestroyed();
    submit(reason);
    applyStates();
    for (DWORD stage = 0; stage < kStages; ++stage)
    {
        if (m_texFlags[stage] & Known)
        {
            bindTexture(stage, m_tex[stage]);
        }
    }
    if (m_worldFlags & Known)
    {
        bindWorld(m_world);
    }
}

HRESULT Batcher::drawDirect(D3DPRIMITIVETYPE type, DWORD fvf, const void* verts, DWORD vertCount, const WORD* indices,
    DWORD indexCount, DWORD flags)
{
    sync(Reason::Direct);
    count(CSubmit);
    Scope s{TDraw};
    if (indices)
    {
        return m_real->DrawIndexedPrimitive(type, fvf, const_cast<void*>(verts), vertCount, const_cast<WORD*>(indices),
            indexCount, flags);
    }
    return m_real->DrawPrimitive(type, fvf, const_cast<void*>(verts), vertCount, flags);
}

void Batcher::useAtlas(DWORD stage, IDirectDrawSurface7* texture, const Layout& layout, const void* verts,
    DWORD vertCount, IDirectDrawSurface7*& binding, Remap* remaps, UINT& remapCount)
{
    TextureAtlas::Entry& e = m_atlas->entry(texture);
    if (!e.eligible)
    {
        count(CAtlasSkipTexture);
        return;
    }
    // The copy only behaves like the original for plain 2D coordinates, and its gutter for wrap addressing or
    // for clamp/mirror (identical within half a texel of the edges) on both axes.
    const DWORD set = stageState(stage, D3DTSS_TEXCOORDINDEX);   // also rejects generated coordinates (high bits)
    if (set >= layout.texCount || !layout.texOffset[set] ||
        stageState(stage, D3DTSS_TEXTURETRANSFORMFLAGS) != D3DTTFF_DISABLE ||
        renderState(D3DRENDERSTATE_WRAP0 + set) != 0)
    {
        count(CAtlasSkipSetup);
        return;
    }
    const DWORD addressU = stageState(stage, D3DTSS_ADDRESSU), addressV = stageState(stage, D3DTSS_ADDRESSV);
    auto edgeClamped = [](DWORD mode) { return mode == D3DTADDRESS_CLAMP || mode == D3DTADDRESS_MIRROR; };
    bool clampEdges = false;
    if (edgeClamped(addressU) && edgeClamped(addressV))
    {
        clampEdges = true;
    }
    else if (addressU != D3DTADDRESS_WRAP || addressV != D3DTADDRESS_WRAP)
    {
        count(CAtlasSkipSetup);
        return;
    }
    const UINT offset = layout.texOffset[set];
    const UINT stride = layout.stride;
    // Coordinates may reach half a texel beyond the edges: the gutter covers that.
    float u0 = FLT_MAX, u1 = -FLT_MAX, v0 = FLT_MAX, v1 = -FLT_MAX;
    const uint8_t* p = static_cast<const uint8_t*>(verts) + offset;
    for (DWORD i = 0; i < vertCount; ++i, p += stride)
    {
        const float* uv = reinterpret_cast<const float*>(p);
        u0 = std::min(u0, uv[0]);
        u1 = std::max(u1, uv[0]);
        v0 = std::min(v0, uv[1]);
        v1 = std::max(v1, uv[1]);
    }
    const float mu = 0.5f / e.width, mv = 0.5f / e.height;
    if (!(u0 >= -mu && u1 <= 1.0f + mu && v0 >= -mv && v1 <= 1.0f + mv))
    {
        count(CAtlasRange);
        return;
    }
    if (!m_atlas->place(texture, e, m_frame, clampEdges))
    {
        count(CAtlasSkipFull);
        return;
    }
    binding = e.page->surface;
    remaps[remapCount++] = {offset, e.scaleU, e.scaleV, e.offsetU, e.offsetV};
}

HRESULT Batcher::draw(D3DPRIMITIVETYPE type, DWORD fvf, const void* verts, DWORD vertCount, const WORD* indices,
    DWORD indexCount, DWORD flags)
{
    drainDestroyed();
    const DWORD tris = triangles(type, indices ? indexCount : vertCount);
    if (!verts || !Fvf::pretransformed(fvf) || tris == 0 || vertCount > kMaxVerts || tris * 3 > kMaxIndices)
    {
        return drawDirect(type, fvf, verts, vertCount, indices, indexCount, flags);
    }
    Source source;
    source.interleaved = verts;
    append(Kind::Pretransformed, type, fvf, source, vertCount, indices, tris, flags);
    return D3D_OK;
}

bool Batcher::drawModel(D3DPRIMITIVETYPE type, DWORD fvf, const void* verts, DWORD vertCount, const WORD* indices,
    DWORD indexCount, DWORD flags)
{
    if (!verts || (fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZ)
    {
        return false;
    }
    // Interleaved vertices as strided streams.
    const Layout& l = layout(fvf);
    const auto* base = static_cast<const uint8_t*>(verts);
    D3DDRAWPRIMITIVESTRIDEDDATA data = {};
    data.position = {const_cast<uint8_t*>(base), l.stride};
    if (l.normalOffset) data.normal = {const_cast<uint8_t*>(base + l.normalOffset), l.stride};
    if (l.diffuseOffset) data.diffuse = {const_cast<uint8_t*>(base + l.diffuseOffset), l.stride};
    if (l.specularOffset) data.specular = {const_cast<uint8_t*>(base + l.specularOffset), l.stride};
    for (UINT set = 0; set < l.texCount; ++set)
    {
        data.textureCoords[set] = {const_cast<uint8_t*>(base + l.setOffset[set]), l.stride};
    }
    return drawModel(type, fvf, data, vertCount, indices, indexCount, flags);
}

bool Batcher::drawModel(D3DPRIMITIVETYPE type, DWORD fvf, const D3DDRAWPRIMITIVESTRIDEDDATA& data, DWORD vertCount,
    const WORD* indices, DWORD indexCount, DWORD flags)
{
    // Plain XYZ positions (no blend weights, no D3DLVERTEX reserved field) in triangles.
    if (!m_batchModels || (fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZ || (fvf & D3DFVF_RESERVED1) ||
        !data.position.lpvData)
    {
        return false;
    }
    drainDestroyed();
    const DWORD tris = triangles(type, indices ? indexCount : vertCount);
    if (tris == 0 || vertCount == 0 || vertCount > kMaxVerts || tris * 3 > kMaxIndices)
    {
        return false;
    }
    const Layout& l = layout(fvf);
    if ((l.normalOffset && !data.normal.lpvData) || (l.diffuseOffset && !data.diffuse.lpvData) ||
        (l.specularOffset && !data.specular.lpvData))
    {
        return false;
    }
    for (UINT set = 0; set < l.texCount; ++set)
    {
        if (!data.textureCoords[set].lpvData)
        {
            return false;
        }
    }
    count(CModelDraw);
    count(CModelVerts, vertCount);
    Source source;
    source.strided = &data;
    append(Kind::Model, type, fvf, source, vertCount, indices, tris, flags);
    return true;
}

namespace
{
    template <UINT Bytes>
    void copyStream(uint8_t* dst, UINT dstStride, const uint8_t* src, DWORD srcStride, DWORD count)
    {
        for (DWORD i = 0; i < count; ++i, dst += dstStride, src += srcStride)
        {
            std::memcpy(dst, src, Bytes);
        }
    }

    void copyStream(uint8_t* dst, UINT dstStride, const void* src, DWORD srcStride, DWORD count, UINT bytes)
    {
        const auto* from = static_cast<const uint8_t*>(src);
        switch (bytes)
        {
        case 4: copyStream<4>(dst, dstStride, from, srcStride, count); break;
        case 8: copyStream<8>(dst, dstStride, from, srcStride, count); break;
        case 12: copyStream<12>(dst, dstStride, from, srcStride, count); break;
        default: copyStream<16>(dst, dstStride, from, srcStride, count); break;
        }
    }
}

void Batcher::gather(uint8_t* dst, const Layout& l, const D3DDRAWPRIMITIVESTRIDEDDATA& d, DWORD count)
{
    copyStream(dst, l.stride, d.position.lpvData, d.position.dwStride, count, 12);
    if (l.normalOffset) copyStream(dst + l.normalOffset, l.stride, d.normal.lpvData, d.normal.dwStride, count, 12);
    if (l.diffuseOffset) copyStream(dst + l.diffuseOffset, l.stride, d.diffuse.lpvData, d.diffuse.dwStride, count, 4);
    if (l.specularOffset) copyStream(dst + l.specularOffset, l.stride, d.specular.lpvData, d.specular.dwStride, count, 4);
    for (UINT set = 0; set < l.texCount; ++set)
    {
        copyStream(dst + l.setOffset[set], l.stride, d.textureCoords[set].lpvData, d.textureCoords[set].dwStride, count,
            l.setBytes[set]);
    }
}

void Batcher::append(Kind kind, D3DPRIMITIVETYPE type, DWORD fvf, const Source& source, DWORD vertCount,
    const WORD* indices, DWORD tris, DWORD flags)
{
    const Layout& vertexLayout = layout(fvf);
    const UINT stride = vertexLayout.stride;

    // Textures the draw samples, replaced by atlas pages where possible. A coordinate set shared by two
    // stages keeps the original textures: remapping it for one would break the other.
    IDirectDrawSurface7* binding[kStages];
    DWORD sets[kStages];
    DWORD stages = 0;
    auto enabled = [this](DWORD stage) {
        const DWORD op = stageState(stage, D3DTSS_COLOROP);
        return op != D3DTOP_DISABLE && op != 0;     // 0: stage not supported by the device
    };
    for (; stages < kStages && enabled(stages); ++stages)
    {
        binding[stages] = texture(stages);
        sets[stages] = stageState(stages, D3DTSS_TEXCOORDINDEX);
    }
    Remap remaps[kStages];
    UINT remapCount = 0;
    // Models keep their textures: they rarely merge, and checking their coordinates costs more than it saves.
    if (m_useAtlas && source.interleaved)
    {
        for (DWORD s = 0; s < stages; ++s)
        {
            if (!binding[s])
            {
                continue;
            }
            bool shared = false;
            for (DWORD o = 0; o < stages; ++o)
            {
                shared |= o != s && m_tex[o] && sets[o] == sets[s];
            }
            if (shared)
            {
                count(CAtlasSkipShared);
            }
            else
            {
                useAtlas(s, binding[s], vertexLayout, source.interleaved, vertCount, binding[s], remaps, remapCount);
            }
        }
    }

    // Append to the pending batch if the device state it was drawn with is still right.
    Reason reason = Reason::Other;
    bool start = m_indexCount == 0;
    if (!start)
    {
        if (m_kind != kind || m_fvf != fvf || m_flags != flags)
        {
            reason = Reason::Format;
            start = true;
        }
        else if (kind == Kind::Model && std::memcmp(&m_batchWorld, &world(), sizeof(D3DMATRIX)) != 0)
        {
            reason = Reason::World;
            start = true;
        }
        else if (m_vertCount + vertCount > kMaxVerts || m_indexCount + tris * 3 > kMaxIndices)
        {
            reason = Reason::Full;
            start = true;
        }
        else
        {
            for (DWORD s = 0; s < stages && !start; ++s)
            {
                if (!(m_texFlags[s] & Applied) || m_texDevice[s] != binding[s])
                {
                    reason = Reason::Texture;
                    start = true;
                    const bool pages = (m_texFlags[s] & Applied) && m_atlas->isPage(m_texDevice[s]) &&
                        m_atlas->isPage(binding[s]);
                    count(pages ? CFlushPages : CFlushOriginal);
                }
            }
            start = start || stateChanged(reason);
        }
    }
    if (start)
    {
        submit(reason);
        applyStates();
        for (DWORD s = 0; s < stages; ++s)
        {
            bindTexture(s, binding[s]);
        }
        if (kind == Kind::Model)
        {
            m_batchWorld = world();
            bindWorld(m_batchWorld);
        }
        m_kind = kind;
        m_fvf = fvf;
        m_flags = flags;
        m_stride = stride;
        if (m_verts.size() < size_t(kMaxVerts) * stride)
        {
            m_verts.resize(size_t(kMaxVerts) * stride);
        }
    }
    else
    {
        count(CMerged);
    }
    if (remapCount)
    {
        count(CAtlasDraw);
    }

    uint8_t* dst = m_verts.data() + size_t(m_vertCount) * stride;
    if (source.interleaved)
    {
        std::memcpy(dst, source.interleaved, size_t(vertCount) * stride);
    }
    else
    {
        gather(dst, vertexLayout, *source.strided, vertCount);
    }
    for (UINT r = 0; r < remapCount; ++r)
    {
        const Remap& m = remaps[r];
        uint8_t* p = dst + m.offset;
        for (DWORD i = 0; i < vertCount; ++i, p += stride)
        {
            float* uv = reinterpret_cast<float*>(p);
            uv[0] = uv[0] * m.scaleU + m.offsetU;
            uv[1] = uv[1] * m.scaleV + m.offsetV;
        }
    }

    WORD* out = m_indices.data() + m_indexCount;
    const DWORD base = m_vertCount;
    auto at = [&](DWORD i) { return static_cast<WORD>(base + (indices ? indices[i] : i)); };
    switch (type)
    {
    case D3DPT_TRIANGLELIST:
        for (DWORD i = 0; i < tris * 3; ++i)
        {
            out[i] = at(i);
        }
        break;
    case D3DPT_TRIANGLESTRIP:
        // Every second triangle of a strip is reversed to keep the winding.
        for (DWORD t = 0; t < tris; ++t, out += 3)
        {
            out[0] = at(t);
            out[1] = at(t + 1 + (t & 1));
            out[2] = at(t + 2 - (t & 1));
        }
        break;
    default:    // fan
        for (DWORD t = 0; t < tris; ++t, out += 3)
        {
            out[0] = at(0);
            out[1] = at(t + 1);
            out[2] = at(t + 2);
        }
        break;
    }
    m_vertCount += vertCount;
    m_indexCount += tris * 3;
}
