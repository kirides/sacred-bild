#include "game/device_proxy.h"
#include "game/d3d_stats.h"
#include "game/gpu_skin.h"
#include "game/ui_canvas.h"
#include "render/fvf.h"
#include "config/render.h"
#include "config/ui.h"
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

    bool isNear(float a, float b) { return std::fabs(a - b) < 0.05f; }

    // One axis of a textured UI rectangle: screen positions p0 < p1 carry texture coordinates e0, e1 (texels).
    // Replaces e0, e1 so that each covered pixel samples the texture at its middle, as the unscaled game's texels
    // were drawn, and the outermost pixels stay `margin` texels inside the rectangle's texels: UI textures are sheets
    // of images, and the next image is often transparent or dark (bilinear filtering blended it in as seams).
    void fitTexelAxis(float p0, float p1, float& e0, float& e1, float margin)
    {
        constexpr float eps = 0.02f;
        auto isWhole = [](float v) { return std::fabs(v - std::round(v)) < eps; };
        float lo = std::min(e0, e1), hi = std::max(e0, e1);
        // The game's UI image records reach half a texel past the image (FUN_00761580: u1 = (x1 + 0.5) / 256).
        if (isWhole(lo) && isWhole(hi - 0.5f) && hi - lo > 1.0f)
        {
            (e1 > e0 ? e1 : e0) -= 0.5f;
            hi -= 0.5f;
        }
        // Pixels lie on whole coordinates (Direct3D 7/9) and are covered from p0 up to, not including, p1.
        const float c0 = std::ceil(p0), c1 = std::ceil(p1) - 1.0f;
        if (c1 < c0)
        {
            return;
        }
        const float r = (e1 - e0) / (p1 - p0);
        float lowest = std::floor(lo + eps) + margin, highest = std::ceil(hi - eps) - margin;
        if (lowest > highest)
        {
            lowest = highest = (lo + hi) / 2.0f;
        }
        const float s0 = std::clamp(e0 + (c0 + 0.5f - p0) * r, lowest, highest);
        const float s1 = std::clamp(e0 + (c1 + 0.5f - p0) * r, lowest, highest);
        const float k = c1 > c0 ? (s1 - s0) / (c1 - c0) : 0.0f;
        e0 = s0 + (p0 - c0) * k;
        e1 = s0 + (p1 - c0) * k;
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
    if (Config::render.batch)
    {
        Batcher::Options options;
        options.noClip = Config::render.batchNoClip;
        options.models = Config::render.batchModels;
        options.vertexBuffers = Config::render.batchVertexBuffer;
        options.atlas = Config::render.atlas;
        options.atlasPageSize = Config::render.atlasPageSize;
        options.atlasPages = Config::render.atlasPages;
        options.atlasMaxTextureSize = Config::render.atlasMaxTextureSize;
        g_instance->m_batcher = std::make_unique<Batcher>(real, ddraw, options);
    }
    return g_instance;
}

DeviceProxy::CallLock::CallLock(DeviceProxy& proxy, void* site) : m_proxy(proxy)
{
    proxy.m_mutex.lock();
    const DWORD thread = SpinLock::currentThread();
    if (thread != proxy.m_presentThread && proxy.m_presentThread)
    {
        proxy.noteForeignCall(thread, site);
    }
}

void DeviceProxy::noteForeignCall(DWORD thread, void* site)
{
    ++m_foreignCalls;
    for (ForeignSite& s : m_foreignSites)
    {
        if (s.site == site && s.thread == thread)
        {
            ++s.calls;
            return;
        }
    }
    if (m_foreignSites.size() < 64)
    {
        m_foreignSites.push_back({site, thread, 1});
    }
}

void DeviceProxy::onPresent(DWORD thread)
{
    CallLock lock(*this, _ReturnAddress());
    m_presentThread = thread;
    const DWORD now = GetTickCount();
    if (!m_foreignReportTick)
    {
        m_foreignReportTick = now;
    }
    if (now - m_foreignReportTick < 10000)
    {
        return;
    }
    m_foreignReportTick = now;
    std::sort(m_foreignSites.begin(), m_foreignSites.end(), [](const auto& a, const auto& b) { return a.calls > b.calls; });
    std::string sites;
    for (size_t i = 0; i < std::min<size_t>(m_foreignSites.size(), 8); ++i)
    {
        sites += Fmt::format(" {}(thread {}) x{}", m_foreignSites[i].site, m_foreignSites[i].thread, m_foreignSites[i].calls);
    }
    LOG("Device calls from threads other than the presenting one ({}) in 10 s: {}{}", thread, m_foreignCalls,
        sites.empty() ? "" : ";" + sites);
    m_foreignSites.clear();
    m_foreignCalls = 0;
}

void DeviceProxy::beginBatch()
{
    Scope p{TProxy};
    CallLock lock(*this, _ReturnAddress());
    if (m_batcher && !m_batcher->active())
    {
        m_batcher->begin();
    }
}

void DeviceProxy::endBatch()
{
    Scope p{TProxy};
    CallLock lock(*this, _ReturnAddress());
    if (m_batcher && m_batcher->active())
    {
        m_batcher->end();
    }
}

void DeviceProxy::syncBatch()
{
    Scope p{TProxy};
    CallLock lock(*this, _ReturnAddress());
    if (batching())
    {
        m_batcher->sync();
    }
}

DeviceProxy* DeviceProxy::instance()
{
    return g_instance;
}

D3DVIEWPORT7 DeviceProxy::canvasViewport(const D3DVIEWPORT7& virt) const
{
    const UiCanvas::Placement p = UiCanvas::placement();
    const float s = p.scale;
    const float l = std::max(p.clipLeft, p.originX);
    const float t = std::max(p.clipTop, p.originY);
    const float r = std::min(p.clipRight, p.originX + 1024.0f * s);
    const float b = std::min(p.clipBottom, p.originY + 768.0f * s);
    const float x0 = std::max(p.originX + virt.dwX * s, l);
    const float y0 = std::max(p.originY + virt.dwY * s, t);
    const float x1 = std::min(p.originX + (virt.dwX + virt.dwWidth) * s, r);
    const float y1 = std::min(p.originY + (virt.dwY + virt.dwHeight) * s, b);
    D3DVIEWPORT7 vp = virt;
    vp.dwX = static_cast<DWORD>(std::lround(x0));
    vp.dwY = static_cast<DWORD>(std::lround(y0));
    vp.dwWidth = static_cast<DWORD>(std::max(1L, std::lround(x1 - x0)));
    vp.dwHeight = static_cast<DWORD>(std::max(1L, std::lround(y1 - y0)));
    return vp;
}

bool DeviceProxy::beginOverlay3D(Overlay3D& restore)
{
    if (!m_ui || UiCanvas::frame().confine)
    {
        return false;
    }
    constexpr DWORD kProjection = D3DTRANSFORMSTATE_PROJECTION;
    D3DMATRIX projection;
    if (m_transformKnown[kProjection])
    {
        projection = m_transforms[kProjection];
    }
    else if (FAILED(m_real->GetTransform(D3DTRANSFORMSTATE_PROJECTION, &projection)))
    {
        return false;
    }
    // Where the game's viewport lands (x0, y0, w0, h0) ...
    const UiCanvas::Placement p = UiCanvas::placement();
    const float x0 = p.originX + m_uiViewport.dwX * p.scale, y0 = p.originY + m_uiViewport.dwY * p.scale;
    const float w0 = m_uiViewport.dwWidth * p.scale, h0 = m_uiViewport.dwHeight * p.scale;
    // ... inside the viewport the device gets: the frame's whole clip rect.
    D3DVIEWPORT7 vp = m_uiViewport;
    vp.dwX = static_cast<DWORD>(std::lround(p.clipLeft));
    vp.dwY = static_cast<DWORD>(std::lround(p.clipTop));
    vp.dwWidth = static_cast<DWORD>(std::max(0L, std::lround(p.clipRight - p.clipLeft)));
    vp.dwHeight = static_cast<DWORD>(std::max(0L, std::lround(p.clipBottom - p.clipTop)));
    if (vp.dwWidth == 0 || vp.dwHeight == 0 || w0 < 1.0f || h0 < 1.0f)
    {
        return false;
    }
    // Screen x = X + (1 + ndc) W / 2 (y: Y + (1 - ndc) H / 2): in clip space x' = a x + b w, y' = c y + d w keep
    // each point where the game's viewport would have put it.
    const float tw = static_cast<float>(vp.dwWidth), th = static_cast<float>(vp.dwHeight);
    const float a = w0 / tw, b = (2.0f * (x0 - vp.dwX) + w0) / tw - 1.0f;
    const float c = h0 / th, d = 1.0f - (2.0f * (y0 - vp.dwY) + h0) / th;
    D3DMATRIX adjusted = projection;
    for (int r = 0; r < 4; ++r)
    {
        D3DVALUE* row = &adjusted._11 + r * 4;
        row[0] = row[0] * a + row[3] * b;
        row[1] = row[1] * c + row[3] * d;
    }
    m_real->GetViewport(&restore.viewport);
    restore.projection = projection;
    m_real->SetViewport(&vp);
    m_real->SetTransform(D3DTRANSFORMSTATE_PROJECTION, &adjusted);
    return true;
}

void DeviceProxy::endOverlay3D(const Overlay3D& restore)
{
    D3DVIEWPORT7 vp = restore.viewport;
    D3DMATRIX projection = restore.projection;
    m_real->SetViewport(&vp);
    m_real->SetTransform(D3DTRANSFORMSTATE_PROJECTION, &projection);
}

DWORD DeviceProxy::uiFilter(DWORD value) const
{
    return Config::ui.linearFilter && value == D3DTFG_POINT ? D3DTFG_LINEAR : value;
}

void DeviceProxy::beginUi()
{
    CallLock lock(*this, _ReturnAddress());
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
    m_real->GetViewport(&m_savedViewport);
    m_uiViewport = {0, 0, 1024, 768, m_savedViewport.dvMinZ, m_savedViewport.dvMaxZ};
    if (UiCanvas::frame().confine)
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

void DeviceProxy::uiFrameChanged()
{
    CallLock lock(*this, _ReturnAddress());
    if (!m_ui)
    {
        return;
    }
    // Confined frames clip through the viewport; unconfined ones draw on the whole screen.
    D3DVIEWPORT7 vp = UiCanvas::frame().confine ? canvasViewport(m_uiViewport) : m_savedViewport;
    m_real->SetViewport(&vp);
}

void DeviceProxy::endUi()
{
    CallLock lock(*this, _ReturnAddress());
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
    const UiCanvas::Placement place = UiCanvas::placement();
    const bool confine = UiCanvas::frame().confine;
    const float s = place.scale, ox = place.originX, oy = place.originY;
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
    if (confine && onScreen && (m_virtMaxX > 1100 || m_virtMaxY > 830 || m_virtMinX < -76 || m_virtMinY < -62))
    {
        noteUiDraw(m_site, "draw beyond 1024x768");
    }
    // Windows parked outside the 1024x768 screen must stay invisible.
    return !confine || maxX > place.clipLeft && minX < place.clipRight && maxY > place.clipTop && minY < place.clipBottom;
}

bool DeviceProxy::drawScreenDim(D3DPRIMITIVETYPE type, DWORD fvf, DWORD count, DWORD flags, HRESULT& hr)
{
    if (m_texture0 || count != 4 || !(fvf & D3DFVF_DIFFUSE) ||
        m_virtMinX > 0.5f || m_virtMinY > 0.5f || m_virtMaxX < 1022.5f || m_virtMaxY < 766.5f)
    {
        return false;
    }
    const UINT stride = Fvf::stride(fvf);
    const UINT diffuse = 16 + ((fvf & D3DFVF_RESERVED1) ? 4 : 0);
    for (DWORD i = 0; i < count; ++i)
    {
        DWORD color;
        std::memcpy(&color, m_scratch.data() + size_t(i) * stride + diffuse, sizeof(color));
        if ((color & 0xFFFFFF) != 0 || (color >> 24) == 0xFF)
        {
            return false;
        }
    }
    const UiCanvas::Placement place = UiCanvas::placement();
    const float midX = place.originX + 512.0f * place.scale, midY = place.originY + 384.0f * place.scale;
    const float l = static_cast<float>(m_savedViewport.dwX), t = static_cast<float>(m_savedViewport.dwY);
    const float r = l + m_savedViewport.dwWidth, b = t + m_savedViewport.dwHeight;
    for (DWORD i = 0; i < count; ++i)
    {
        auto* p = reinterpret_cast<float*>(m_scratch.data() + size_t(i) * stride);
        p[0] = p[0] < midX ? l : r;
        p[1] = p[1] < midY ? t : b;
    }
    D3DVIEWPORT7 restore;
    m_real->GetViewport(&restore);
    m_real->SetViewport(&m_savedViewport);
    D3DStats::count(CSubmit);
    {
        Scope s{TDraw};
        hr = m_real->DrawPrimitive(type, fvf, m_scratch.data(), count, flags);
    }
    m_real->SetViewport(&restore);
    return true;
}

bool DeviceProxy::drawScreenBar(D3DPRIMITIVETYPE type, DWORD fvf, DWORD count, DWORD flags, HRESULT& hr)
{
    const bool top = m_virtMinY < 0.5f, bottom = m_virtMaxY > 767.5f;
    if (m_texture0 || count != 4 || !(fvf & D3DFVF_DIFFUSE) || m_virtMinX > 0.5f || m_virtMaxX < 1023.5f ||
        top == bottom || !UiCanvas::inGame())
    {
        return false;
    }
    const UINT stride = Fvf::stride(fvf);
    const UINT diffuse = 16 + ((fvf & D3DFVF_RESERVED1) ? 4 : 0);
    for (DWORD i = 0; i < count; ++i)
    {
        DWORD color;
        std::memcpy(&color, m_scratch.data() + size_t(i) * stride + diffuse, sizeof(color));
        if ((color & 0xFFFFFF) != 0)
        {
            return false;
        }
    }
    const UiCanvas::Placement place = UiCanvas::placement();
    const float midX = place.originX + 512.0f * place.scale;
    const float l = static_cast<float>(m_savedViewport.dwX), t = static_cast<float>(m_savedViewport.dwY);
    const float r = l + m_savedViewport.dwWidth, b = t + m_savedViewport.dwHeight;
    // Moved against the screen's edge with its height kept.
    const float shift = top ? t - place.originY : b - (place.originY + 768.0f * place.scale);
    for (DWORD i = 0; i < count; ++i)
    {
        auto* p = reinterpret_cast<float*>(m_scratch.data() + size_t(i) * stride);
        p[0] = p[0] < midX ? l : r;
        p[1] += shift;
    }
    D3DVIEWPORT7 restore;
    m_real->GetViewport(&restore);
    m_real->SetViewport(&m_savedViewport);
    D3DStats::count(CSubmit);
    {
        Scope s{TDraw};
        hr = m_real->DrawPrimitive(type, fvf, m_scratch.data(), count, flags);
    }
    m_real->SetViewport(&restore);
    return true;
}

void DeviceProxy::traceUiDraw(const char* what, DWORD count)
{
    UiCanvas::trace(Fmt::format("{} {} vertices {:.0f},{:.0f} .. {:.0f},{:.0f} texture {}", what, count, m_virtMinX,
        m_virtMinY, m_virtMaxX, m_virtMaxY, static_cast<void*>(m_texture0)));
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
    const UiCanvas::Placement place = UiCanvas::placement();
    const float l = place.clipLeft, t = place.clipTop, r = place.clipRight, b = place.clipBottom;
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

void DeviceProxy::fitTexels(DWORD fvf, uint8_t* verts)
{
    if (!m_texture0 || Fvf::texCount(fvf) == 0)
    {
        return;
    }
    if (m_texture0Width == 0)
    {
        DDSURFACEDESC2 desc = {};
        desc.dwSize = sizeof(desc);
        if (FAILED(m_texture0->GetSurfaceDesc(&desc)) || !desc.dwWidth || !desc.dwHeight)
        {
            return;
        }
        m_texture0Width = desc.dwWidth;
        m_texture0Height = desc.dwHeight;
    }
    const UINT stride = Fvf::stride(fvf);
    const UINT uv = Fvf::texCoordOffset(fvf, 0) / 4;
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
    if (maxX - minX < 0.5f || maxY - minY < 0.5f)
    {
        return;
    }
    // Only axis-aligned rectangles whose texture coordinates follow one axis each.
    float u[2] = {}, t[2] = {};
    bool haveU[2] = {}, haveT[2] = {};
    for (int i = 0; i < 4; ++i)
    {
        const bool left = isNear(v[i][0], minX), top = isNear(v[i][1], minY);
        if (!(left || isNear(v[i][0], maxX)) || !(top || isNear(v[i][1], maxY)))
        {
            return;
        }
        const int x = left ? 0 : 1, y = top ? 0 : 1;
        if ((haveU[x] && std::fabs(u[x] - v[i][uv]) > 0.0001f) || (haveT[y] && std::fabs(t[y] - v[i][uv + 1]) > 0.0001f))
        {
            return;
        }
        u[x] = v[i][uv];
        t[y] = v[i][uv + 1];
        haveU[x] = haveT[y] = true;
    }
    const float w = static_cast<float>(m_texture0Width), h = static_cast<float>(m_texture0Height);
    float eu[2] = {u[0] * w, u[1] * w}, ev[2] = {t[0] * h, t[1] * h};
    // Bilinear sampling reads half a texel around the sample position; point sampling only the texel under it.
    const bool linear = uiFilter(m_filters[0][0]) != D3DTFG_POINT || uiFilter(m_filters[0][1]) != D3DTFN_POINT;
    const float margin = linear ? 0.5f : 0.05f;
    fitTexelAxis(minX, maxX, eu[0], eu[1], margin);
    fitTexelAxis(minY, maxY, ev[0], ev[1], margin);
    for (int i = 0; i < 4; ++i)
    {
        v[i][uv] = eu[isNear(v[i][0], minX) ? 0 : 1] / w;
        v[i][uv + 1] = ev[isNear(v[i][1], minY) ? 0 : 1] / h;
    }
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
    CallLock lock(*this, _ReturnAddress());
    if (batching())
    {
        m_batcher->sync();
    }
    return m_real->EndScene();
}

HRESULT DeviceProxy::GetDirect3D(LPDIRECT3D7* d3d) { return m_real->GetDirect3D(d3d); }

HRESULT DeviceProxy::SetRenderTarget(LPDIRECTDRAWSURFACE7 surface, DWORD flags)
{
    CallLock lock(*this, _ReturnAddress());
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
    CallLock lock(*this, _ReturnAddress());
    if (batching())
    {
        m_batcher->sync();      // clears the viewport, after everything drawn so far
    }
    if (m_ui && count && rects)
    {
        const UiCanvas::Placement place = UiCanvas::placement();
        auto mapX = [&](LONG x) { return static_cast<LONG>(std::lround(x * place.scale + place.originX)); };
        auto mapY = [&](LONG y) { return static_cast<LONG>(std::lround(y * place.scale + place.originY)); };
        std::vector<D3DRECT> mapped(rects, rects + count);
        for (D3DRECT& r : mapped)
        {
            r.x1 = mapX(r.x1);
            r.y1 = mapY(r.y1);
            r.x2 = mapX(r.x2);
            r.y2 = mapY(r.y2);
        }
        return m_real->Clear(count, mapped.data(), flags, color, z, stencil);
    }
    return m_real->Clear(count, rects, flags, color, z, stencil);
}

HRESULT DeviceProxy::SetTransform(D3DTRANSFORMSTATETYPE type, LPD3DMATRIX m)
{
    Scope p{TProxy};
    D3DStats::count(CTransform);
    CallLock lock(*this, _ReturnAddress());
    HRESULT hr;
    if (batching() && m)
    {
        hr = m_batcher->setTransform(type, *m);
    }
    else
    {
        Scope s{TState};
        hr = m_real->SetTransform(type, m);
    }
    if (static_cast<DWORD>(type) < kTransforms)
    {
        // While a state block records, the call may not reach the device state.
        m_transformKnown[type] = SUCCEEDED(hr) && m && !m_recording;
        if (m_transformKnown[type])
        {
            m_transforms[type] = *m;
        }
    }
    return hr;
}

HRESULT DeviceProxy::GetTransform(D3DTRANSFORMSTATETYPE type, LPD3DMATRIX m)
{
    CallLock lock(*this, _ReturnAddress());
    const DWORD i = static_cast<DWORD>(type);
    if (m && i < kTransforms && m_transformKnown[i])
    {
        *m = m_transforms[i];
        return D3D_OK;
    }
    // While batching, the device may hold an identity world matrix for a model batch.
    const HRESULT hr = batching() && type == D3DTRANSFORMSTATE_WORLD ? m_batcher->getWorld(m) : m_real->GetTransform(type, m);
    if (SUCCEEDED(hr) && m && i < kTransforms && !m_recording)
    {
        m_transforms[i] = *m;
        m_transformKnown[i] = true;
    }
    return hr;
}
HRESULT DeviceProxy::SetViewport(LPD3DVIEWPORT7 vp)
{
    Scope p{TProxy};
    CallLock lock(*this, _ReturnAddress());
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
HRESULT DeviceProxy::MultiplyTransform(D3DTRANSFORMSTATETYPE type, LPD3DMATRIX m)
{
    CallLock lock(*this, _ReturnAddress());
    if (static_cast<DWORD>(type) < kTransforms)
    {
        m_transformKnown[type] = false;
    }
    return m_real->MultiplyTransform(type, m);
}
HRESULT DeviceProxy::GetViewport(LPD3DVIEWPORT7 vp)
{
    Scope p{TProxy};
    CallLock lock(*this, _ReturnAddress());
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
HRESULT DeviceProxy::SetMaterial(LPD3DMATERIAL7 mat)
{
    CallLock lock(*this, _ReturnAddress());
    if (batching() && mat)
    {
        return m_batcher->setMaterial(*mat);
    }
    return m_real->SetMaterial(mat);
}

HRESULT DeviceProxy::GetMaterial(LPD3DMATERIAL7 mat) { return m_real->GetMaterial(mat); }
HRESULT DeviceProxy::SetLight(DWORD index, LPD3DLIGHT7 light)
{
    CallLock lock(*this, _ReturnAddress());
    if (batching() && light)
    {
        return m_batcher->setLight(index, *light);
    }
    return m_real->SetLight(index, light);
}

HRESULT DeviceProxy::GetLight(DWORD index, LPD3DLIGHT7 light) { return m_real->GetLight(index, light); }

HRESULT DeviceProxy::SetRenderState(D3DRENDERSTATETYPE state, DWORD value)
{
    Scope p{TProxy};
    D3DStats::count(CRenderState);
    D3DStats::onRenderState(state, value);
    CallLock lock(*this, _ReturnAddress());
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
    CallLock lock(*this, _ReturnAddress());
    if (batching())
    {
        return m_batcher->getRenderState(state, value);
    }
    return m_real->GetRenderState(state, value);
}

HRESULT DeviceProxy::BeginStateBlock()
{
    CallLock lock(*this, _ReturnAddress());
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
    CallLock lock(*this, _ReturnAddress());
    const HRESULT hr = m_real->EndStateBlock(handle);
    m_recording = false;
    forgetTransforms();
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
    CallLock lock(*this, _ReturnAddress());
    if (batching())
    {
        if (Fvf::pretransformed(fvf))
        {
            return m_batcher->draw(type, fvf, verts, count, nullptr, 0, flags);
        }
        if (m_batcher->drawModel(type, fvf, verts, count, nullptr, 0, flags))
        {
            return D3D_OK;
        }
        D3DStats::count(CModelDirect);
        m_batcher->sync(Batcher::Reason::Direct);
    }
    if (m_ui && verts && Fvf::pretransformed(fvf))
    {
        m_site = _ReturnAddress();
        const void* mapped = nullptr;
        const bool visible = mapToCanvas(fvf, verts, count, mapped);
        if (UiCanvas::tracing())
        {
            traceUiDraw(visible ? "DrawPrimitive" : "DrawPrimitive (culled)", count);
        }
        if (!visible)
        {
            return D3D_OK;
        }
        HRESULT dimmed;
        if (quad && (drawScreenDim(type, fvf, count, flags, dimmed) || drawScreenBar(type, fvf, count, flags, dimmed)))
        {
            return dimmed;
        }
        if (quad)
        {
            fitTexels(fvf, m_scratch.data());
            if (UiCanvas::frame().confine)
            {
                clipQuad(fvf, m_scratch.data());
            }
        }
        D3DStats::count(CSubmit);
        Scope s{TDraw};
        return m_real->DrawPrimitive(type, fvf, const_cast<void*>(mapped), count, flags);
    }
    if (m_ui)
    {
        noteUiDraw(_ReturnAddress(), "3D DrawPrimitive (viewport-mapped)");
    }
    D3DStats::count(CSubmit);
    Scope s{TDraw};
    Overlay3D restore;
    const bool overlay = beginOverlay3D(restore);
    const HRESULT hr = m_real->DrawPrimitive(type, fvf, verts, count, flags);
    if (overlay)
    {
        endOverlay3D(restore);
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
    CallLock lock(*this, _ReturnAddress());
    if (batching())
    {
        if (Fvf::pretransformed(fvf))
        {
            return m_batcher->draw(type, fvf, verts, vertCount, indices, indexCount, flags);
        }
        if (m_batcher->drawModel(type, fvf, verts, vertCount, indices, indexCount, flags))
        {
            return D3D_OK;
        }
        D3DStats::count(CModelDirect);
        m_batcher->sync(Batcher::Reason::Direct);
    }
    if (m_ui && verts && Fvf::pretransformed(fvf))
    {
        m_site = _ReturnAddress();
        const void* mapped = nullptr;
        const bool visible = mapToCanvas(fvf, verts, vertCount, mapped);
        if (UiCanvas::tracing())
        {
            traceUiDraw(visible ? "DrawIndexedPrimitive" : "DrawIndexedPrimitive (culled)", vertCount);
        }
        if (!visible)
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
    D3DStats::count(CSubmit);
    Scope s{TDraw};
    Overlay3D restore;
    const bool overlay = beginOverlay3D(restore);
    const HRESULT hr = m_real->DrawIndexedPrimitive(type, fvf, verts, vertCount, indices, indexCount, flags);
    if (overlay)
    {
        endOverlay3D(restore);
    }
    return hr;
}

HRESULT DeviceProxy::drawQuads(TextureLookup lookup, uint32_t texture0, uint32_t texture1, DWORD fvf, const void* verts,
    DWORD vertCount, const WORD* indices, DWORD indexCount)
{
    CallLock lock(*this, _ReturnAddress());
    if (!batching())
    {
        SetTexture(0, lookup(texture0));
        if (texture1)
        {
            SetTexture(1, lookup(texture1));
        }
        return DrawIndexedPrimitive(D3DPT_TRIANGLELIST, fvf, const_cast<void*>(verts), vertCount,
            const_cast<WORD*>(indices), indexCount, 0);
    }
    // The stage 0 lookup counts as game time, as it did inside the game's flush; the stage 1 lookup (layers only)
    // falls into the proxy time below.
    IDirectDrawSurface7* surface0 = lookup(texture0);
    // What SetTexture and DrawIndexedPrimitive count and record, once.
    Scope p{TProxy};
    D3DStats::count(CSetTexture, texture1 ? 2 : 1);
    D3DStats::count(CDrawIndexed);
    D3DStats::count(CVerts, vertCount);
    D3DStats::count(CDrawTL);
    D3DStats::onDraw(D3DPT_TRIANGLELIST, fvf, indexCount, true);
    m_texture0Width = m_texture0Height = 0;
    if (surface0 != m_texture0)
    {
        m_texture0 = surface0;
        D3DStats::count(CTexSwitch);
        D3DStats::onTexture(surface0);
    }
    m_batcher->setTexture(0, surface0);
    if (texture1)
    {
        m_batcher->setTexture(1, lookup(texture1));
    }
    return m_batcher->drawQuads(fvf, verts, vertCount, indices, indexCount);
}

bool DeviceProxy::drawDirect(DirectDrawFn draw, void* context)
{
    CallLock lock(*this, _ReturnAddress());
    {
        Scope p{TProxy};
        if (batching())
        {
            m_batcher->sync(Batcher::Reason::Direct);
        }
    }
    D3DStats::count(CSubmit);
    Scope s{TDraw};
    return draw(m_real, context);
}

HRESULT DeviceProxy::SetClipStatus(LPD3DCLIPSTATUS status)
{
    CallLock lock(*this, _ReturnAddress());
    if (batching())
    {
        m_batcher->sync();
    }
    return m_real->SetClipStatus(status);
}

HRESULT DeviceProxy::GetClipStatus(LPD3DCLIPSTATUS status)
{
    CallLock lock(*this, _ReturnAddress());
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
    CallLock lock(*this, _ReturnAddress());
    if (batching())
    {
        if (data && m_batcher->drawModel(type, fvf, *data, count, nullptr, 0, flags))
        {
            return D3D_OK;
        }
        D3DStats::count(CModelDirect);
        m_batcher->sync(Batcher::Reason::Direct);
    }
    if (m_ui)
    {
        noteUiDraw(_ReturnAddress(), "DrawPrimitiveStrided (not mapped)");
    }
    D3DStats::count(CSubmit);
    Scope s{TDraw};
    Overlay3D restore;
    const bool overlay = beginOverlay3D(restore);
    const HRESULT hr = m_real->DrawPrimitiveStrided(type, fvf, data, count, flags);
    if (overlay)
    {
        endOverlay3D(restore);
    }
    return hr;
}

HRESULT DeviceProxy::DrawIndexedPrimitiveStrided(D3DPRIMITIVETYPE type, DWORD fvf, LPD3DDRAWPRIMITIVESTRIDEDDATA data,
    DWORD vertCount, LPWORD indices, DWORD indexCount, DWORD flags)
{
    Scope p{TProxy};
    D3DStats::count(CDrawIndexed);
    D3DStats::count(CVerts, vertCount);
    CallLock lock(*this, _ReturnAddress());
    if (data && GpuSkin::active())
    {
        // Vertices whose skinning was left to the GPU: drawn by the backend's shader once the batcher's recorded state
        // is applied, or skinned on the CPU after all and drawn below.
        auto prepare = [](void* self) {
            auto* proxy = static_cast<DeviceProxy*>(self);
            if (proxy->batching())
            {
                proxy->m_batcher->sync(Batcher::Reason::Direct);
            }
        };
        if (GpuSkin::draw(m_real, !m_ui, prepare, this, type, fvf, *data, vertCount, indices, indexCount))
        {
            D3DStats::count(CSubmit);
            return D3D_OK;
        }
    }
    if (batching())
    {
        if (data && m_batcher->drawModel(type, fvf, *data, vertCount, indices, indexCount, flags))
        {
            return D3D_OK;
        }
        D3DStats::count(CModelDirect);
        m_batcher->sync(Batcher::Reason::Direct);
    }
    if (m_ui)
    {
        noteUiDraw(_ReturnAddress(), "DrawIndexedPrimitiveStrided (not mapped)");
    }
    D3DStats::count(CSubmit);
    Scope s{TDraw};
    Overlay3D restore;
    const bool overlay = beginOverlay3D(restore);
    const HRESULT hr = m_real->DrawIndexedPrimitiveStrided(type, fvf, data, vertCount, indices, indexCount, flags);
    if (overlay)
    {
        endOverlay3D(restore);
    }
    return hr;
}

HRESULT DeviceProxy::DrawPrimitiveVB(D3DPRIMITIVETYPE type, LPDIRECT3DVERTEXBUFFER7 vb, DWORD start, DWORD count, DWORD flags)
{
    Scope p{TProxy};
    D3DStats::count(CDrawVB);
    D3DStats::count(CVerts, count);
    D3DStats::onDraw(type, 0xFFFFFF, count, false);
    CallLock lock(*this, _ReturnAddress());
    if (batching())
    {
        m_batcher->sync(Batcher::Reason::Direct);
    }
    if (m_ui)
    {
        noteUiDraw(_ReturnAddress(), "DrawPrimitiveVB (not mapped)");
    }
    D3DStats::count(CSubmit);
    Scope s{TDraw};
    Overlay3D restore;
    const bool overlay = beginOverlay3D(restore);
    const HRESULT hr = m_real->DrawPrimitiveVB(type, vb, start, count, flags);
    if (overlay)
    {
        endOverlay3D(restore);
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
    CallLock lock(*this, _ReturnAddress());
    if (batching())
    {
        m_batcher->sync(Batcher::Reason::Direct);
    }
    if (m_ui)
    {
        noteUiDraw(_ReturnAddress(), "DrawIndexedPrimitiveVB (not mapped)");
    }
    D3DStats::count(CSubmit);
    Scope s{TDraw};
    Overlay3D restore;
    const bool overlay = beginOverlay3D(restore);
    const HRESULT hr = m_real->DrawIndexedPrimitiveVB(type, vb, start, vertCount, indices, indexCount, flags);
    if (overlay)
    {
        endOverlay3D(restore);
    }
    return hr;
}

HRESULT DeviceProxy::ComputeSphereVisibility(LPD3DVECTOR centers, LPD3DVALUE radii, DWORD count, DWORD flags, LPDWORD result)
{
    CallLock lock(*this, _ReturnAddress());
    if (batching())
    {
        m_batcher->sync();      // uses the device's world matrix
    }
    return m_real->ComputeSphereVisibility(centers, radii, count, flags, result);
}

HRESULT DeviceProxy::GetTexture(DWORD stage, LPDIRECTDRAWSURFACE7* texture)
{
    Scope p{TProxy};
    CallLock lock(*this, _ReturnAddress());
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
    CallLock lock(*this, _ReturnAddress());
    if (stage == 0)
    {
        m_texture0Width = m_texture0Height = 0;     // the pointer may name a new texture of another size
    }
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
    CallLock lock(*this, _ReturnAddress());
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
    CallLock lock(*this, _ReturnAddress());
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
    CallLock lock(*this, _ReturnAddress());
    if (batching())
    {
        m_batcher->sync();
    }
    return m_real->ValidateDevice(passes);
}

HRESULT DeviceProxy::ApplyStateBlock(DWORD handle)
{
    CallLock lock(*this, _ReturnAddress());
    forgetTransforms();
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
    CallLock lock(*this, _ReturnAddress());
    if (batching())
    {
        m_batcher->sync();
    }
    return m_real->CaptureStateBlock(handle);
}

HRESULT DeviceProxy::DeleteStateBlock(DWORD handle) { return m_real->DeleteStateBlock(handle); }

HRESULT DeviceProxy::CreateStateBlock(D3DSTATEBLOCKTYPE type, LPDWORD handle)
{
    CallLock lock(*this, _ReturnAddress());
    if (batching())
    {
        m_batcher->sync();
    }
    return m_real->CreateStateBlock(type, handle);
}

HRESULT DeviceProxy::Load(LPDIRECTDRAWSURFACE7 dst, LPPOINT dstPoint, LPDIRECTDRAWSURFACE7 src, LPRECT srcRect, DWORD flags)
{
    CallLock lock(*this, _ReturnAddress());
    if (batching())
    {
        m_batcher->sync();      // pending draws may sample the destination
    }
    return m_real->Load(dst, dstPoint, src, srcRect, flags);
}

HRESULT DeviceProxy::LightEnable(DWORD index, BOOL enable)
{
    CallLock lock(*this, _ReturnAddress());
    if (batching())
    {
        return m_batcher->lightEnable(index, enable);
    }
    return m_real->LightEnable(index, enable);
}

HRESULT DeviceProxy::GetLightEnable(DWORD index, BOOL* enable) { return m_real->GetLightEnable(index, enable); }
HRESULT DeviceProxy::SetClipPlane(DWORD index, D3DVALUE* plane)
{
    CallLock lock(*this, _ReturnAddress());
    if (batching())
    {
        m_batcher->beforeModelStateChange();
    }
    return m_real->SetClipPlane(index, plane);
}

HRESULT DeviceProxy::GetClipPlane(DWORD index, D3DVALUE* plane) { return m_real->GetClipPlane(index, plane); }
HRESULT DeviceProxy::GetInfo(DWORD id, LPVOID info, DWORD size) { return m_real->GetInfo(id, info, size); }
