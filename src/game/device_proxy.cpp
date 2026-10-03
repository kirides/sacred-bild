#include "game/device_proxy.h"
#include "game/d3d_stats.h"
#include "game/ui_canvas.h"
#include "render/fvf.h"
#include "config.h"
#include "log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <intrin.h>
#include <unordered_set>

using namespace D3DStats;

namespace
{
    DeviceProxy* g_instance = nullptr;

    bool isPretransformed(DWORD fvf)
    {
        return Fvf::pretransformed(fvf);
    }

    bool isNear(float a, float b) { return std::fabs(a - b) < 0.05f; }

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

    // Logs each game call site once that draws in UI mode with geometry the canvas can't place exactly.
    void noteUiDraw(const void* site, const char* what)
    {
        static std::unordered_set<const void*> seen;
        if (seen.size() < 64 && seen.insert(site).second)
        {
            LOG("UI canvas: {} at {}", what, site);
        }
    }
}

DeviceProxy* DeviceProxy::wrap(IDirect3DDevice7* real, IDirectDraw7* ddraw)
{
    if (!real)
    {
        return nullptr;
    }
    g_instance = new DeviceProxy(real);
    if (g_config.batch)
    {
        Batcher::Options options;
        options.noClip = g_config.batchNoClip;
        options.atlas = g_config.atlas;
        options.atlasPageSize = g_config.atlasPageSize;
        options.atlasPages = g_config.atlasPages;
        options.atlasMaxTextureSize = g_config.atlasMaxTextureSize;
        g_instance->m_batcher = std::make_unique<Batcher>(real, ddraw, options);
    }
    return g_instance;
}

void DeviceProxy::beginBatch()
{
    Scope p{TProxy};
    std::scoped_lock lock(m_mutex);
    if (m_batcher && !m_batcher->active())
    {
        m_batcher->begin();
    }
}

void DeviceProxy::endBatch()
{
    Scope p{TProxy};
    std::scoped_lock lock(m_mutex);
    if (m_batcher && m_batcher->active())
    {
        m_batcher->end();
    }
}

void DeviceProxy::syncBatch()
{
    Scope p{TProxy};
    std::scoped_lock lock(m_mutex);
    if (batching())
    {
        m_batcher->sync();
    }
}

DeviceProxy* DeviceProxy::instance()
{
    return g_instance;
}

std::string DeviceProxy::renderStates()
{
    DWORD z = 0, zw = 0, zf = 0, ab = 0, at = 0;
    m_real->GetRenderState(D3DRENDERSTATE_ZENABLE, &z);
    m_real->GetRenderState(D3DRENDERSTATE_ZWRITEENABLE, &zw);
    m_real->GetRenderState(D3DRENDERSTATE_ZFUNC, &zf);
    m_real->GetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, &ab);
    m_real->GetRenderState(D3DRENDERSTATE_ALPHATESTENABLE, &at);
    return std::format("z {} zw {} zf {} ab {} at {}", z, zw, zf, ab, at);
}

void DeviceProxy::probeTL(DWORD fvf, const void* verts, DWORD count, const void* site)
{
    if (!m_probe || m_ui || !verts || (fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW)
    {
        return;
    }
    const UINT stride = Fvf::stride(fvf);
    float z0 = 1e30f, z1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
    for (DWORD i = 0; i < count; ++i)
    {
        const float* p = reinterpret_cast<const float*>(static_cast<const uint8_t*>(verts) + size_t(i) * stride);
        z0 = std::min(z0, p[2]); z1 = std::max(z1, p[2]);
        y0 = std::min(y0, p[1]); y1 = std::max(y1, p[1]);
    }
    for (TLSite& s : m_probeTL)
    {
        if (s.site == site)
        {
            ++s.draws;
            s.zMin = std::min(s.zMin, z0); s.zMax = std::max(s.zMax, z1);
            s.yMin = std::min(s.yMin, y0); s.yMax = std::max(s.yMax, y1);
            return;
        }
    }
    if (m_probeTL.size() < 64)
    {
        DWORD z = 0, zw = 0, zf = 0;
        m_real->GetRenderState(D3DRENDERSTATE_ZENABLE, &z);
        m_real->GetRenderState(D3DRENDERSTATE_ZWRITEENABLE, &zw);
        m_real->GetRenderState(D3DRENDERSTATE_ZFUNC, &zf);
        m_probeTL.push_back({site, 1, z0, z1, y0, y1, z, zw, zf});
    }
}

void DeviceProxy::dumpProbeTL()
{
    for (const TLSite& s : m_probeTL)
    {
        LOG("ProbeTL: {} draws {} z {:.4f}..{:.4f} y {:.0f}..{:.0f} (z {} zw {} zf {})", s.site, s.draws, s.zMin, s.zMax,
            s.yMin, s.yMax, s.zEnable, s.zWrite, s.zFunc);
    }
    m_probeTL.clear();
}

void DeviceProxy::setProbe(bool enabled)
{
    std::scoped_lock lock(m_mutex);
    if (m_probe && !enabled)
    {
        dumpProbeTL();
    }
    m_probe = enabled;
    m_probeLogged = 0;
    if (enabled)
    {
        D3DVIEWPORT7 vp = {};
        m_real->GetViewport(&vp);
        LOG("Probe3D: frame start, vp {},{} {}x{} z {:.2f}..{:.2f} | proj diag {:.5f} {:.5f} {:.5f} t ({:.3f},{:.3f},{:.3f}) | "
            "view row0 ({:.3f},{:.3f},{:.3f}) row1 ({:.3f},{:.3f},{:.3f}) row2 ({:.3f},{:.3f},{:.3f}) t ({:.1f},{:.1f},{:.1f})",
            vp.dwX, vp.dwY, vp.dwWidth, vp.dwHeight, vp.dvMinZ, vp.dvMaxZ, m_proj._11, m_proj._22, m_proj._33,
            m_proj._41, m_proj._42, m_proj._43, m_view._11, m_view._12, m_view._13, m_view._21, m_view._22, m_view._23,
            m_view._31, m_view._32, m_view._33, m_view._41, m_view._42, m_view._43);
    }
}

void DeviceProxy::probe3D(const char* what, DWORD fvf, const void* positions, DWORD stride, DWORD count, const void* site)
{
    if (!m_probe || m_ui || m_probeLogged >= 48 || (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW)
    {
        return;
    }
    ++m_probeLogged;
    if (!positions)
    {
        LOG("Probe3D: {} at {} fvf {:x} n {} (no positions)", what, site, fvf, count);
        return;
    }
    D3DVIEWPORT7 vp = {};
    m_real->GetViewport(&vp);
    const D3DMATRIX wvp = multiply(multiply(m_world, m_view), m_proj);
    const float(*M)[4] = reinterpret_cast<const float(*)[4]>(&wvp);
    float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
    int front = 0, sampled = 0;
    const DWORD step = std::max<DWORD>(1, count / 128);
    for (DWORD i = 0; i < count; i += step, ++sampled)
    {
        const float* p = reinterpret_cast<const float*>(static_cast<const uint8_t*>(positions) + size_t(i) * stride);
        float o[4];
        for (int j = 0; j < 4; ++j)
            o[j] = p[0] * M[0][j] + p[1] * M[1][j] + p[2] * M[2][j] + M[3][j];
        if (o[3] <= 1e-6f)
        {
            continue;
        }
        ++front;
        const float sx = vp.dwX + (1.0f + o[0] / o[3]) * vp.dwWidth * 0.5f;
        const float sy = vp.dwY + (1.0f - o[1] / o[3]) * vp.dwHeight * 0.5f;
        const float sz = o[2] / o[3];
        x0 = std::min(x0, sx); x1 = std::max(x1, sx);
        y0 = std::min(y0, sy); y1 = std::max(y1, sy);
        z0 = std::min(z0, sz); z1 = std::max(z1, sz);
    }
    LOG("Probe3D: {} at {} fvf {:x} n {} -> x {:.0f}..{:.0f} y {:.0f}..{:.0f} z {:.4f}..{:.4f} ({}/{} in front) {} "
        "world.t=({:.0f},{:.0f},{:.0f})",
        what, site, fvf, count, x0, x1, y0, y1, z0, z1, front, sampled, renderStates(),
        m_world._41, m_world._42, m_world._43);
}

D3DVIEWPORT7 DeviceProxy::canvasViewport(const D3DVIEWPORT7& virt) const
{
    const float s = UiCanvas::scale();
    const float l = m_confine ? UiCanvas::left() : 0.0f, t = m_confine ? UiCanvas::top() : 0.0f;
    const float r = m_confine ? UiCanvas::right() : static_cast<float>(m_savedViewport.dwX + m_savedViewport.dwWidth);
    const float b = m_confine ? UiCanvas::bottom() : static_cast<float>(m_savedViewport.dwY + m_savedViewport.dwHeight);
    const float x0 = std::max(UiCanvas::left() + virt.dwX * s, l);
    const float y0 = std::max(UiCanvas::top() + virt.dwY * s, t);
    const float x1 = std::min(UiCanvas::left() + (virt.dwX + virt.dwWidth) * s, r);
    const float y1 = std::min(UiCanvas::top() + (virt.dwY + virt.dwHeight) * s, b);
    D3DVIEWPORT7 vp = virt;
    vp.dwX = static_cast<DWORD>(std::lround(x0));
    vp.dwY = static_cast<DWORD>(std::lround(y0));
    vp.dwWidth = static_cast<DWORD>(std::max(1L, std::lround(x1 - x0)));
    vp.dwHeight = static_cast<DWORD>(std::max(1L, std::lround(y1 - y0)));
    return vp;
}

bool DeviceProxy::beginOverlay3D(D3DVIEWPORT7& restore)
{
    if (!m_ui || m_confine)
    {
        return false;
    }
    m_real->GetViewport(&restore);
    m_confine = true;
    D3DVIEWPORT7 vp = canvasViewport(m_uiViewport);
    m_confine = false;
    m_real->SetViewport(&vp);
    return true;
}

DWORD DeviceProxy::uiFilter(DWORD value) const
{
    return g_config.uiLinearFilter && value == D3DTFG_POINT ? D3DTFG_LINEAR : value;
}

void DeviceProxy::beginUi(bool confine)
{
    std::scoped_lock lock(m_mutex);
    if (m_ui)
    {
        return;
    }
    // UI mode talks to the device directly.
    if (batching())
    {
        m_batcher->sync();
    }
    m_ui = true;
    m_confine = confine;
    m_real->GetViewport(&m_savedViewport);
    m_uiViewport = {0, 0, 1024, 768, m_savedViewport.dvMinZ, m_savedViewport.dvMaxZ};
    if (m_confine)
    {
        D3DVIEWPORT7 vp = canvasViewport(m_uiViewport);
        m_real->SetViewport(&vp);
    }
    for (DWORD stage = 0; stage < 2; ++stage)
    {
        m_real->SetTextureStageState(stage, D3DTSS_MAGFILTER, uiFilter(m_filters[stage][0]));
        m_real->SetTextureStageState(stage, D3DTSS_MINFILTER, uiFilter(m_filters[stage][1]));
    }
}

void DeviceProxy::endUi()
{
    std::scoped_lock lock(m_mutex);
    if (!m_ui)
    {
        return;
    }
    m_ui = false;
    m_real->SetViewport(&m_savedViewport);
    for (DWORD stage = 0; stage < 2; ++stage)
    {
        m_real->SetTextureStageState(stage, D3DTSS_MAGFILTER, m_filters[stage][0]);
        m_real->SetTextureStageState(stage, D3DTSS_MINFILTER, m_filters[stage][1]);
    }
    if (batching())
    {
        m_batcher->invalidate();
    }
}

bool DeviceProxy::mapToCanvas(DWORD fvf, const void* verts, DWORD count, const void*& mapped)
{
    const UINT stride = Fvf::stride(fvf);
    const size_t bytes = size_t(stride) * count;
    if (m_scratch.size() < bytes)
    {
        m_scratch.resize(bytes);
    }
    std::memcpy(m_scratch.data(), verts, bytes);
    m_virtMinX = m_virtMinY = 1e30f;
    m_virtMaxX = m_virtMaxY = -1e30f;
    const float s = UiCanvas::scale(), ox = UiCanvas::left(), oy = UiCanvas::top();
    float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
    for (DWORD i = 0; i < count; ++i)
    {
        auto* p = reinterpret_cast<float*>(m_scratch.data() + size_t(i) * stride);
        m_virtMinX = std::min(m_virtMinX, p[0]);
        m_virtMaxX = std::max(m_virtMaxX, p[0]);
        m_virtMinY = std::min(m_virtMinY, p[1]);
        m_virtMaxY = std::max(m_virtMaxY, p[1]);
        p[0] = p[0] * s + ox;
        p[1] = p[1] * s + oy;
        minX = std::min(minX, p[0]);
        maxX = std::max(maxX, p[0]);
        minY = std::min(minY, p[1]);
        maxY = std::max(maxY, p[1]);
    }
    mapped = m_scratch.data();
    const bool onScreen = m_virtMaxX > 0 && m_virtMinX < 1024 && m_virtMaxY > 0 && m_virtMinY < 768;
    if (m_confine && onScreen && (m_virtMaxX > 1100 || m_virtMaxY > 830 || m_virtMinX < -76 || m_virtMinY < -62))
    {
        noteUiDraw(m_site, "draw beyond 1024x768");
    }
    // Windows parked outside the 1024x768 screen must stay invisible.
    return !m_confine || maxX > UiCanvas::left() && minX < UiCanvas::right() && maxY > UiCanvas::top() && minY < UiCanvas::bottom();
}

bool DeviceProxy::clipQuad(DWORD fvf, uint8_t* verts)
{
    const UINT texOffset = Fvf::texCoordOffset(fvf, 0);
    const UINT stride = Fvf::stride(fvf);
    const UINT texCount = Fvf::texCount(fvf);
    float* v[4];
    float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
    for (int i = 0; i < 4; ++i)
    {
        v[i] = reinterpret_cast<float*>(verts + i * stride);
        minX = std::min(minX, v[i][0]);
        maxX = std::max(maxX, v[i][0]);
        minY = std::min(minY, v[i][1]);
        maxY = std::max(maxY, v[i][1]);
    }
    const float l = UiCanvas::left(), t = UiCanvas::top(), r = UiCanvas::right(), b = UiCanvas::bottom();
    if (minX >= l && maxX <= r && minY >= t && maxY <= b)
    {
        return true;
    }
    if (maxX - minX < 0.5f || maxY - minY < 0.5f)
    {
        return false;
    }
    // Only axis-aligned rectangles whose texture coordinates follow one axis each.
    for (int i = 0; i < 4; ++i)
    {
        if (!(isNear(v[i][0], minX) || isNear(v[i][0], maxX)) || !(isNear(v[i][1], minY) || isNear(v[i][1], maxY)))
        {
            return false;
        }
    }
    struct Edge { bool have = false; float value = 0; };
    auto agree = [](Edge& e, float x) {
        if (e.have && std::fabs(e.value - x) > 0.001f) return false;
        e.have = true;
        e.value = x;
        return true;
    };
    const UINT sets = std::min(texCount, 2u);
    Edge u0[2], u1[2], v0[2], v1[2];
    for (UINT set = 0; set < sets; ++set)
    {
        const UINT uv = (texOffset + set * 8) / 4;
        for (int i = 0; i < 4; ++i)
        {
            if (!agree(isNear(v[i][0], minX) ? u0[set] : u1[set], v[i][uv]) ||
                !agree(isNear(v[i][1], minY) ? v0[set] : v1[set], v[i][uv + 1]))
            {
                return false;
            }
        }
    }
    for (int i = 0; i < 4; ++i)
    {
        const float x = std::clamp(v[i][0], l, r), y = std::clamp(v[i][1], t, b);
        for (UINT set = 0; set < sets; ++set)
        {
            const UINT uv = (texOffset + set * 8) / 4;
            v[i][uv] = u0[set].value + (x - minX) / (maxX - minX) * (u1[set].value - u0[set].value);
            v[i][uv + 1] = v0[set].value + (y - minY) / (maxY - minY) * (v1[set].value - v0[set].value);
        }
        v[i][0] = x;
        v[i][1] = y;
    }
    return true;
}

HRESULT DeviceProxy::QueryInterface(REFIID riid, LPVOID* ppvObj)
{
    if (ppvObj && (riid == IID_IDirect3DDevice7 || riid == IID_IUnknown))
    {
        *ppvObj = this;
        AddRef();
        return S_OK;
    }
    return m_real->QueryInterface(riid, ppvObj);
}

ULONG DeviceProxy::AddRef()
{
    return m_real->AddRef();
}

ULONG DeviceProxy::Release()
{
    const ULONG refs = m_real->Release();
    if (refs == 0)
    {
        if (g_instance == this)
        {
            g_instance = nullptr;
        }
        delete this;
    }
    return refs;
}

HRESULT DeviceProxy::GetCaps(LPD3DDEVICEDESC7 desc) { return m_real->GetCaps(desc); }
HRESULT DeviceProxy::EnumTextureFormats(LPD3DENUMPIXELFORMATSCALLBACK cb, LPVOID ctx) { return m_real->EnumTextureFormats(cb, ctx); }
HRESULT DeviceProxy::BeginScene() { return m_real->BeginScene(); }

HRESULT DeviceProxy::EndScene()
{
    Scope p{TProxy};
    std::scoped_lock lock(m_mutex);
    if (batching())
    {
        m_batcher->sync();
    }
    return m_real->EndScene();
}

HRESULT DeviceProxy::GetDirect3D(LPDIRECT3D7* d3d) { return m_real->GetDirect3D(d3d); }

HRESULT DeviceProxy::SetRenderTarget(LPDIRECTDRAWSURFACE7 surface, DWORD flags)
{
    std::scoped_lock lock(m_mutex);
    if (!batching())
    {
        return m_real->SetRenderTarget(surface, flags);
    }
    m_batcher->sync();
    const HRESULT hr = m_real->SetRenderTarget(surface, flags);
    m_batcher->invalidate();    // the viewport may follow the new target
    return hr;
}

HRESULT DeviceProxy::GetRenderTarget(LPDIRECTDRAWSURFACE7* surface) { return m_real->GetRenderTarget(surface); }

HRESULT DeviceProxy::Clear(DWORD count, LPD3DRECT rects, DWORD flags, D3DCOLOR color, D3DVALUE z, DWORD stencil)
{
    Scope p{TProxy};
    D3DStats::count(CClear);
    std::scoped_lock lock(m_mutex);
    if (batching())
    {
        m_batcher->sync();      // clears the viewport, after everything drawn so far
    }
    if (m_ui && count && rects)
    {
        std::vector<D3DRECT> mapped(rects, rects + count);
        for (D3DRECT& r : mapped)
        {
            r.x1 = UiCanvas::toPhysicalX(r.x1);
            r.y1 = UiCanvas::toPhysicalY(r.y1);
            r.x2 = UiCanvas::toPhysicalX(r.x2);
            r.y2 = UiCanvas::toPhysicalY(r.y2);
        }
        return m_real->Clear(count, mapped.data(), flags, color, z, stencil);
    }
    return m_real->Clear(count, rects, flags, color, z, stencil);
}

HRESULT DeviceProxy::SetTransform(D3DTRANSFORMSTATETYPE type, LPD3DMATRIX m)
{
    Scope p{TProxy};
    D3DStats::count(CTransform);
    if (m)
    {
        std::scoped_lock lock(m_mutex);
        if (type == D3DTRANSFORMSTATE_WORLD) m_world = *m;
        else if (type == D3DTRANSFORMSTATE_VIEW) m_view = *m;
        else if (type == D3DTRANSFORMSTATE_PROJECTION) m_proj = *m;
    }
    Scope s{TState};
    return m_real->SetTransform(type, m);
}

HRESULT DeviceProxy::GetTransform(D3DTRANSFORMSTATETYPE type, LPD3DMATRIX m) { return m_real->GetTransform(type, m); }
HRESULT DeviceProxy::SetViewport(LPD3DVIEWPORT7 vp)
{
    Scope p{TProxy};
    std::scoped_lock lock(m_mutex);
    if (m_ui && vp)
    {
        m_uiViewport = *vp;
        D3DVIEWPORT7 mapped = canvasViewport(*vp);
        return m_real->SetViewport(&mapped);
    }
    if (batching() && vp)
    {
        return m_batcher->setViewport(*vp);
    }
    return m_real->SetViewport(vp);
}
HRESULT DeviceProxy::MultiplyTransform(D3DTRANSFORMSTATETYPE type, LPD3DMATRIX m) { return m_real->MultiplyTransform(type, m); }
HRESULT DeviceProxy::GetViewport(LPD3DVIEWPORT7 vp)
{
    Scope p{TProxy};
    std::scoped_lock lock(m_mutex);
    if (m_ui && vp)
    {
        *vp = m_uiViewport;
        return D3D_OK;
    }
    if (batching())
    {
        return m_batcher->getViewport(vp);
    }
    return m_real->GetViewport(vp);
}
HRESULT DeviceProxy::SetMaterial(LPD3DMATERIAL7 mat) { return m_real->SetMaterial(mat); }
HRESULT DeviceProxy::GetMaterial(LPD3DMATERIAL7 mat) { return m_real->GetMaterial(mat); }
HRESULT DeviceProxy::SetLight(DWORD index, LPD3DLIGHT7 light) { return m_real->SetLight(index, light); }
HRESULT DeviceProxy::GetLight(DWORD index, LPD3DLIGHT7 light) { return m_real->GetLight(index, light); }

HRESULT DeviceProxy::SetRenderState(D3DRENDERSTATETYPE state, DWORD value)
{
    Scope p{TProxy};
    D3DStats::count(CRenderState);
    D3DStats::onRenderState(state, value);
    std::scoped_lock lock(m_mutex);
    if (batching())
    {
        return m_batcher->setRenderState(state, value);
    }
    Scope s{TState};
    return m_real->SetRenderState(state, value);
}

HRESULT DeviceProxy::GetRenderState(D3DRENDERSTATETYPE state, LPDWORD value)
{
    Scope p{TProxy};
    std::scoped_lock lock(m_mutex);
    if (batching())
    {
        return m_batcher->getRenderState(state, value);
    }
    return m_real->GetRenderState(state, value);
}

HRESULT DeviceProxy::BeginStateBlock()
{
    std::scoped_lock lock(m_mutex);
    if (batching())
    {
        m_batcher->sync();
    }
    const HRESULT hr = m_real->BeginStateBlock();
    m_recording = SUCCEEDED(hr);    // recorded calls must reach the device
    return hr;
}

HRESULT DeviceProxy::EndStateBlock(LPDWORD handle)
{
    std::scoped_lock lock(m_mutex);
    const HRESULT hr = m_real->EndStateBlock(handle);
    m_recording = false;
    if (batching())
    {
        m_batcher->invalidate();
    }
    return hr;
}

HRESULT DeviceProxy::PreLoad(LPDIRECTDRAWSURFACE7 texture) { return m_real->PreLoad(texture); }

HRESULT DeviceProxy::DrawPrimitive(D3DPRIMITIVETYPE type, DWORD fvf, LPVOID verts, DWORD count, DWORD flags)
{
    Scope p{TProxy};
    D3DStats::count(CDraw);
    D3DStats::count(CVerts, count);
    if (fvf & D3DFVF_XYZRHW) D3DStats::count(CDrawTL);
    const bool quad = count == 4 && (type == D3DPT_TRIANGLESTRIP || type == D3DPT_TRIANGLEFAN);
    if (quad) D3DStats::count(CDrawQuad);
    D3DStats::onDraw(type, fvf, count, false);
    std::scoped_lock lock(m_mutex);
    probeTL(fvf, verts, count, _ReturnAddress());
    if (batching())
    {
        if (isPretransformed(fvf))
        {
            return m_batcher->draw(type, fvf, verts, count, nullptr, 0, flags);
        }
        m_batcher->sync(Batcher::Reason::Direct);
    }
    if (m_ui && verts && isPretransformed(fvf))
    {
        m_site = _ReturnAddress();
        const void* mapped = nullptr;
        if (!mapToCanvas(fvf, verts, count, mapped))
        {
            return D3D_OK;
        }
        if (quad && m_confine)
        {
            clipQuad(fvf, m_scratch.data());
        }
        D3DStats::count(CSubmit);
        Scope s{TDraw};
        return m_real->DrawPrimitive(type, fvf, const_cast<void*>(mapped), count, flags);
    }
    if (m_ui)
    {
        noteUiDraw(_ReturnAddress(), "3D DrawPrimitive (viewport-mapped)");
    }
    probe3D("DP", fvf, verts, Fvf::stride(fvf), count, _ReturnAddress());
    D3DStats::count(CSubmit);
    Scope s{TDraw};
    D3DVIEWPORT7 restore;
    const bool overlay = beginOverlay3D(restore);
    const HRESULT hr = m_real->DrawPrimitive(type, fvf, verts, count, flags);
    if (overlay)
    {
        m_real->SetViewport(&restore);
    }
    return hr;
}

HRESULT DeviceProxy::DrawIndexedPrimitive(D3DPRIMITIVETYPE type, DWORD fvf, LPVOID verts, DWORD vertCount,
    LPWORD indices, DWORD indexCount, DWORD flags)
{
    Scope p{TProxy};
    D3DStats::count(CDrawIndexed);
    D3DStats::count(CVerts, vertCount);
    if (fvf & D3DFVF_XYZRHW) D3DStats::count(CDrawTL);
    D3DStats::onDraw(type, fvf, indexCount, true);
    std::scoped_lock lock(m_mutex);
    probeTL(fvf, verts, vertCount, _ReturnAddress());
    if (batching())
    {
        if (isPretransformed(fvf))
        {
            return m_batcher->draw(type, fvf, verts, vertCount, indices, indexCount, flags);
        }
        m_batcher->sync(Batcher::Reason::Direct);
    }
    if (m_ui && verts && isPretransformed(fvf))
    {
        m_site = _ReturnAddress();
        const void* mapped = nullptr;
        if (!mapToCanvas(fvf, verts, vertCount, mapped))
        {
            return D3D_OK;
        }
        D3DStats::count(CSubmit);
        Scope s{TDraw};
        return m_real->DrawIndexedPrimitive(type, fvf, const_cast<void*>(mapped), vertCount, indices, indexCount, flags);
    }
    if (m_ui)
    {
        noteUiDraw(_ReturnAddress(), "3D DrawIndexedPrimitive (viewport-mapped)");
    }
    probe3D("DIP", fvf, verts, Fvf::stride(fvf), vertCount, _ReturnAddress());
    D3DStats::count(CSubmit);
    Scope s{TDraw};
    D3DVIEWPORT7 restore;
    const bool overlay = beginOverlay3D(restore);
    const HRESULT hr = m_real->DrawIndexedPrimitive(type, fvf, verts, vertCount, indices, indexCount, flags);
    if (overlay)
    {
        m_real->SetViewport(&restore);
    }
    return hr;
}

HRESULT DeviceProxy::SetClipStatus(LPD3DCLIPSTATUS status)
{
    std::scoped_lock lock(m_mutex);
    if (batching())
    {
        m_batcher->sync();
    }
    return m_real->SetClipStatus(status);
}

HRESULT DeviceProxy::GetClipStatus(LPD3DCLIPSTATUS status)
{
    std::scoped_lock lock(m_mutex);
    if (batching())
    {
        m_batcher->sync();
    }
    return m_real->GetClipStatus(status);
}

HRESULT DeviceProxy::DrawPrimitiveStrided(D3DPRIMITIVETYPE type, DWORD fvf, LPD3DDRAWPRIMITIVESTRIDEDDATA data,
    DWORD count, DWORD flags)
{
    Scope p{TProxy};
    D3DStats::count(CDraw);
    D3DStats::count(CVerts, count);
    std::scoped_lock lock(m_mutex);
    if (batching())
    {
        m_batcher->sync(Batcher::Reason::Direct);
    }
    if (m_ui)
    {
        noteUiDraw(_ReturnAddress(), "DrawPrimitiveStrided (not mapped)");
    }
    probe3D("DPS", fvf, data ? data->position.lpvData : nullptr, data ? data->position.dwStride : 0, count, _ReturnAddress());
    D3DStats::count(CSubmit);
    Scope s{TDraw};
    D3DVIEWPORT7 restore;
    const bool overlay = beginOverlay3D(restore);
    const HRESULT hr = m_real->DrawPrimitiveStrided(type, fvf, data, count, flags);
    if (overlay)
    {
        m_real->SetViewport(&restore);
    }
    return hr;
}

HRESULT DeviceProxy::DrawIndexedPrimitiveStrided(D3DPRIMITIVETYPE type, DWORD fvf, LPD3DDRAWPRIMITIVESTRIDEDDATA data,
    DWORD vertCount, LPWORD indices, DWORD indexCount, DWORD flags)
{
    Scope p{TProxy};
    D3DStats::count(CDrawIndexed);
    D3DStats::count(CVerts, vertCount);
    std::scoped_lock lock(m_mutex);
    if (batching())
    {
        m_batcher->sync(Batcher::Reason::Direct);
    }
    if (m_ui)
    {
        noteUiDraw(_ReturnAddress(), "DrawIndexedPrimitiveStrided (not mapped)");
    }
    probe3D("DIPS", fvf, data ? data->position.lpvData : nullptr, data ? data->position.dwStride : 0, vertCount,
        _ReturnAddress());
    D3DStats::count(CSubmit);
    Scope s{TDraw};
    D3DVIEWPORT7 restore;
    const bool overlay = beginOverlay3D(restore);
    const HRESULT hr = m_real->DrawIndexedPrimitiveStrided(type, fvf, data, vertCount, indices, indexCount, flags);
    if (overlay)
    {
        m_real->SetViewport(&restore);
    }
    return hr;
}

HRESULT DeviceProxy::DrawPrimitiveVB(D3DPRIMITIVETYPE type, LPDIRECT3DVERTEXBUFFER7 vb, DWORD start, DWORD count, DWORD flags)
{
    Scope p{TProxy};
    D3DStats::count(CDrawVB);
    D3DStats::count(CVerts, count);
    D3DStats::onDraw(type, 0xFFFFFF, count, false);
    std::scoped_lock lock(m_mutex);
    if (batching())
    {
        m_batcher->sync(Batcher::Reason::Direct);
    }
    if (m_ui)
    {
        noteUiDraw(_ReturnAddress(), "DrawPrimitiveVB (not mapped)");
    }
    probe3D("DPVB", 0, nullptr, 0, count, _ReturnAddress());
    D3DStats::count(CSubmit);
    Scope s{TDraw};
    D3DVIEWPORT7 restore;
    const bool overlay = beginOverlay3D(restore);
    const HRESULT hr = m_real->DrawPrimitiveVB(type, vb, start, count, flags);
    if (overlay)
    {
        m_real->SetViewport(&restore);
    }
    return hr;
}

HRESULT DeviceProxy::DrawIndexedPrimitiveVB(D3DPRIMITIVETYPE type, LPDIRECT3DVERTEXBUFFER7 vb, DWORD start,
    DWORD vertCount, LPWORD indices, DWORD indexCount, DWORD flags)
{
    Scope p{TProxy};
    D3DStats::count(CDrawVB);
    D3DStats::count(CVerts, vertCount);
    D3DStats::onDraw(type, 0xFFFFFF, indexCount, true);
    std::scoped_lock lock(m_mutex);
    if (batching())
    {
        m_batcher->sync(Batcher::Reason::Direct);
    }
    if (m_ui)
    {
        noteUiDraw(_ReturnAddress(), "DrawIndexedPrimitiveVB (not mapped)");
    }
    probe3D("DIPVB", 0, nullptr, 0, vertCount, _ReturnAddress());
    D3DStats::count(CSubmit);
    Scope s{TDraw};
    D3DVIEWPORT7 restore;
    const bool overlay = beginOverlay3D(restore);
    const HRESULT hr = m_real->DrawIndexedPrimitiveVB(type, vb, start, vertCount, indices, indexCount, flags);
    if (overlay)
    {
        m_real->SetViewport(&restore);
    }
    return hr;
}

HRESULT DeviceProxy::ComputeSphereVisibility(LPD3DVECTOR centers, LPD3DVALUE radii, DWORD count, DWORD flags, LPDWORD result)
{
    return m_real->ComputeSphereVisibility(centers, radii, count, flags, result);
}

HRESULT DeviceProxy::GetTexture(DWORD stage, LPDIRECTDRAWSURFACE7* texture)
{
    Scope p{TProxy};
    std::scoped_lock lock(m_mutex);
    if (batching())
    {
        return m_batcher->getTexture(stage, texture);
    }
    return m_real->GetTexture(stage, texture);
}

HRESULT DeviceProxy::SetTexture(DWORD stage, LPDIRECTDRAWSURFACE7 texture)
{
    Scope p{TProxy};
    D3DStats::count(CSetTexture);
    std::scoped_lock lock(m_mutex);
    if (stage == 0 && texture != m_texture0)
    {
        m_texture0 = texture;
        D3DStats::count(CTexSwitch);
        D3DStats::onTexture(texture);
    }
    if (batching())
    {
        return m_batcher->setTexture(stage, texture);
    }
    Scope s{TState};
    return m_real->SetTexture(stage, texture);
}

HRESULT DeviceProxy::GetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, LPDWORD value)
{
    Scope p{TProxy};
    std::scoped_lock lock(m_mutex);
    if (batching())
    {
        return m_batcher->getStageState(stage, type, value);
    }
    return m_real->GetTextureStageState(stage, type, value);
}

HRESULT DeviceProxy::SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
    Scope p{TProxy};
    D3DStats::count(CStageState);
    D3DStats::onStageState(stage, type, value);
    std::scoped_lock lock(m_mutex);
    if (stage < 2 && (type == D3DTSS_MAGFILTER || type == D3DTSS_MINFILTER))
    {
        m_filters[stage][type == D3DTSS_MAGFILTER ? 0 : 1] = value;
        if (m_ui)
        {
            value = uiFilter(value);
        }
    }
    if (batching())
    {
        return m_batcher->setStageState(stage, type, value);
    }
    Scope s{TState};
    return m_real->SetTextureStageState(stage, type, value);
}

HRESULT DeviceProxy::ValidateDevice(LPDWORD passes)
{
    std::scoped_lock lock(m_mutex);
    if (batching())
    {
        m_batcher->sync();
    }
    return m_real->ValidateDevice(passes);
}

HRESULT DeviceProxy::ApplyStateBlock(DWORD handle)
{
    std::scoped_lock lock(m_mutex);
    if (!batching())
    {
        return m_real->ApplyStateBlock(handle);
    }
    m_batcher->sync();
    const HRESULT hr = m_real->ApplyStateBlock(handle);
    m_batcher->invalidate();
    return hr;
}

HRESULT DeviceProxy::CaptureStateBlock(DWORD handle)
{
    std::scoped_lock lock(m_mutex);
    if (batching())
    {
        m_batcher->sync();
    }
    return m_real->CaptureStateBlock(handle);
}

HRESULT DeviceProxy::DeleteStateBlock(DWORD handle) { return m_real->DeleteStateBlock(handle); }

HRESULT DeviceProxy::CreateStateBlock(D3DSTATEBLOCKTYPE type, LPDWORD handle)
{
    std::scoped_lock lock(m_mutex);
    if (batching())
    {
        m_batcher->sync();
    }
    return m_real->CreateStateBlock(type, handle);
}

HRESULT DeviceProxy::Load(LPDIRECTDRAWSURFACE7 dst, LPPOINT dstPoint, LPDIRECTDRAWSURFACE7 src, LPRECT srcRect, DWORD flags)
{
    std::scoped_lock lock(m_mutex);
    if (batching())
    {
        m_batcher->sync();      // pending draws may sample the destination
    }
    return m_real->Load(dst, dstPoint, src, srcRect, flags);
}

HRESULT DeviceProxy::LightEnable(DWORD index, BOOL enable) { return m_real->LightEnable(index, enable); }
HRESULT DeviceProxy::GetLightEnable(DWORD index, BOOL* enable) { return m_real->GetLightEnable(index, enable); }
HRESULT DeviceProxy::SetClipPlane(DWORD index, D3DVALUE* plane) { return m_real->SetClipPlane(index, plane); }
HRESULT DeviceProxy::GetClipPlane(DWORD index, D3DVALUE* plane) { return m_real->GetClipPlane(index, plane); }
HRESULT DeviceProxy::GetInfo(DWORD id, LPVOID info, DWORD size) { return m_real->GetInfo(id, info, size); }
