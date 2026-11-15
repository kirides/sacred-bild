#include "text.h"
#include "log.h"

#include <windows.h>
#include <ddraw.h>
#include <d3d.h>
#include <algorithm>
#include <cmath>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "dxguid.lib")

namespace
{
    constexpr int kPage = 1024;             // atlas pages are kPage x kPage
    constexpr size_t kMaxPages = 8;         // beyond: start over (every glyph is rasterized again when drawn)
    constexpr size_t kMaxFonts = 48;        // fonts (face, pixel size, style) kept; the least recently used goes
    constexpr int kPad = 1;                 // transparent texels around each glyph
    const wchar_t* const kDefaultFace = L"Segoe UI";

    struct Glyph
    {
        bool measured = false;
        int advance = 0;                    // pixels
        int originX = 0, originY = 0;       // the black box's top-left against the pen (y up from the baseline)
        int width = 0, height = 0;          // the black box
        int page = -1;                      // -1: not in an atlas (yet), or empty
        uint16_t u = 0, v = 0;              // in its page, the padding included
        uint32_t generation = 0;            // the atlas generation it was placed in
    };

    struct Font
    {
        HFONT handle = nullptr;
        int ascent = 0, height = 0;         // pixels
        int cap = 0;                        // the capitals' height ('H'), pixels
        std::unordered_map<wchar_t, Glyph> glyphs;
        uint64_t used = 0;
    };

    struct Page
    {
        IDirectDrawSurface7* surface = nullptr;
        bool wide = true;                   // A8R8G8B8, else A4R4G4B4
        int shelfX = 0, shelfY = 0, shelfH = 0;
    };

    std::mutex g_lock;
    HDC g_dc = nullptr;
    std::unordered_map<std::wstring, std::unique_ptr<Font>> g_fonts;
    uint64_t g_clock = 0;
    std::vector<Page> g_pages;
    IDirect3DDevice7* g_device = nullptr;   // the device the pages belong to
    uint32_t g_generation = 1;              // bumped when the atlases start over
    bool g_failed = false;                  // no texture could be made: draw nothing (logged once)

    std::wstring keyOf(const std::wstring& face, int px, uint32_t flags)
    {
        return face + L'\x1' + std::to_wstring(px) + L'\x1' + std::to_wstring(flags & (Text::Bold | Text::Italic));
    }

    int pixelSize(const Text::Style& style, float scale)
    {
        return std::clamp(static_cast<int>(std::lround(style.size * scale)), 4, 160);
    }

    Font* font(const Text::Style& style, int px)
    {
        const std::wstring& face = style.face.empty() ? std::wstring(kDefaultFace) : style.face;
        const std::wstring key = keyOf(face, px, style.flags);
        auto it = g_fonts.find(key);
        if (it != g_fonts.end())
        {
            it->second->used = ++g_clock;
            return it->second.get();
        }
        if (!g_dc)
        {
            g_dc = CreateCompatibleDC(nullptr);
            if (!g_dc)
            {
                return nullptr;
            }
        }
        if (g_fonts.size() >= kMaxFonts)
        {
            auto oldest = std::min_element(g_fonts.begin(), g_fonts.end(),
                                           [](const auto& a, const auto& b) { return a.second->used < b.second->used; });
            DeleteObject(oldest->second->handle);
            g_fonts.erase(oldest);
        }
        HFONT handle = CreateFontW(-px, 0, 0, 0, (style.flags & Text::Bold) ? FW_BOLD : FW_NORMAL,
                                   (style.flags & Text::Italic) ? TRUE : FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH,
                                   face.substr(0, LF_FACESIZE - 1).c_str());
        if (!handle)
        {
            return nullptr;
        }
        auto f = std::make_unique<Font>();
        f->handle = handle;
        SelectObject(g_dc, handle);
        TEXTMETRICW tm{};
        GetTextMetricsW(g_dc, &tm);
        f->ascent = tm.tmAscent;
        f->height = tm.tmHeight;
        GLYPHMETRICS gm{};
        const MAT2 identity = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};
        f->cap = GetGlyphOutlineW(g_dc, L'H', GGO_METRICS, &gm, 0, nullptr, &identity) != GDI_ERROR
            ? gm.gmptGlyphOrigin.y : tm.tmAscent - tm.tmInternalLeading;
        f->used = ++g_clock;
        Font* out = f.get();
        g_fonts.emplace(key, std::move(f));
        return out;
    }

    const MAT2 kIdentity = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};

    Glyph& glyph(Font& f, wchar_t ch)
    {
        Glyph& g = f.glyphs[ch];
        if (!g.measured)
        {
            g.measured = true;
            SelectObject(g_dc, f.handle);
            GLYPHMETRICS gm{};
            if (GetGlyphOutlineW(g_dc, ch, GGO_METRICS, &gm, 0, nullptr, &kIdentity) != GDI_ERROR)
            {
                g.advance = gm.gmCellIncX;
                g.originX = gm.gmptGlyphOrigin.x;
                g.originY = gm.gmptGlyphOrigin.y;
                g.width = static_cast<int>(gm.gmBlackBoxX);
                g.height = static_cast<int>(gm.gmBlackBoxY);
            }
        }
        return g;
    }

    // Splits at '\n' (a '\r' before it is dropped).
    std::vector<std::wstring_view> lines(const std::wstring& text)
    {
        std::vector<std::wstring_view> out;
        size_t start = 0;
        while (true)
        {
            size_t end = text.find(L'\n', start);
            std::wstring_view line(text.data() + start, (end == std::wstring::npos ? text.size() : end) - start);
            if (!line.empty() && line.back() == L'\r')
            {
                line.remove_suffix(1);
            }
            out.push_back(line);
            if (end == std::wstring::npos)
            {
                return out;
            }
            start = end + 1;
        }
    }

    int lineWidth(Font& f, std::wstring_view line)
    {
        int w = 0;
        for (wchar_t ch : line)
        {
            w += glyph(f, ch).advance;
        }
        return w;
    }

    // ---- Atlas pages ----

    IDirectDraw7* directDraw(IDirect3DDevice7* device)
    {
        IDirect3D7* d3d = nullptr;
        if (FAILED(device->GetDirect3D(&d3d)) || !d3d)
        {
            return nullptr;
        }
        IDirectDraw7* ddraw = nullptr;
        d3d->QueryInterface(IID_IDirectDraw7, reinterpret_cast<void**>(&ddraw));
        d3d->Release();
        return ddraw;
    }

    bool createPage(IDirect3DDevice7* device, Page& page)
    {
        IDirectDraw7* ddraw = directDraw(device);
        if (!ddraw)
        {
            return false;
        }
        HRESULT hr = E_FAIL;
        for (const bool wide : {true, false})
        {
            DDSURFACEDESC2 desc = {};
            desc.dwSize = sizeof(desc);
            desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
            desc.dwWidth = kPage;
            desc.dwHeight = kPage;
            desc.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
            desc.ddsCaps.dwCaps2 = DDSCAPS2_TEXTUREMANAGE;
            desc.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
            desc.ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_ALPHAPIXELS;
            desc.ddpfPixelFormat.dwRGBBitCount = wide ? 32 : 16;
            desc.ddpfPixelFormat.dwRBitMask = wide ? 0xFF0000 : 0xF00;
            desc.ddpfPixelFormat.dwGBitMask = wide ? 0xFF00 : 0xF0;
            desc.ddpfPixelFormat.dwBBitMask = wide ? 0xFF : 0xF;
            desc.ddpfPixelFormat.dwRGBAlphaBitMask = wide ? 0xFF000000 : 0xF000;
            hr = ddraw->CreateSurface(&desc, &page.surface, nullptr);
            if (SUCCEEDED(hr))
            {
                page.wide = wide;
                break;
            }
            page.surface = nullptr;
        }
        ddraw->Release();
        if (FAILED(hr))
        {
            LOG("UI: no texture for the text atlas ({:08x}): vector text is not drawn", static_cast<uint32_t>(hr));
            return false;
        }
        // Cleared to transparent white.
        DDSURFACEDESC2 desc = {};
        desc.dwSize = sizeof(desc);
        if (SUCCEEDED(page.surface->Lock(nullptr, &desc, DDLOCK_WAIT | DDLOCK_WRITEONLY, nullptr)))
        {
            for (int y = 0; y < kPage; ++y)
            {
                auto* row = static_cast<uint8_t*>(desc.lpSurface) + size_t(y) * desc.lPitch;
                if (page.wide)
                {
                    std::fill_n(reinterpret_cast<uint32_t*>(row), kPage, 0x00FFFFFFu);
                }
                else
                {
                    std::fill_n(reinterpret_cast<uint16_t*>(row), kPage, uint16_t(0x0FFF));
                }
            }
            page.surface->Unlock(nullptr);
        }
        return true;
    }

    // Forgets the pages of a device that is gone and starts the atlases over.
    void startOver(bool release)
    {
        for (Page& p : g_pages)
        {
            if (release && p.surface)
            {
                p.surface->Release();
            }
        }
        g_pages.clear();
        ++g_generation;
    }

    // A place for a w x h cell (padding included); false when every page is full.
    bool allocate(IDirect3DDevice7* device, int w, int h, int& pageIndex, int& u, int& v)
    {
        for (size_t i = 0; i <= g_pages.size(); ++i)
        {
            if (i == g_pages.size())
            {
                if (g_pages.size() >= kMaxPages)
                {
                    return false;
                }
                Page page;
                if (!createPage(device, page))
                {
                    g_failed = true;
                    return false;
                }
                g_pages.push_back(page);
            }
            Page& p = g_pages[i];
            if (p.shelfX + w > kPage)
            {
                p.shelfX = 0;
                p.shelfY += p.shelfH;
                p.shelfH = 0;
            }
            if (p.shelfY + h <= kPage && w <= kPage)
            {
                pageIndex = static_cast<int>(i);
                u = p.shelfX;
                v = p.shelfY;
                p.shelfX += w;
                p.shelfH = std::max(p.shelfH, h);
                return true;
            }
        }
        return false;
    }

    // Rasterizes the glyph into its page.
    bool upload(IDirect3DDevice7* device, Font& f, wchar_t ch, Glyph& g)
    {
        // The bitmap first: its box can differ from GGO_METRICS' by a pixel, and the cell must hold all of it.
        SelectObject(g_dc, f.handle);
        GLYPHMETRICS gm{};
        const DWORD size = GetGlyphOutlineW(g_dc, ch, GGO_GRAY8_BITMAP, &gm, 0, nullptr, &kIdentity);
        if (size == GDI_ERROR || size == 0)
        {
            g.page = -1;
            g.generation = g_generation;
            return true;
        }
        std::vector<uint8_t> bits(size);
        if (GetGlyphOutlineW(g_dc, ch, GGO_GRAY8_BITMAP, &gm, size, bits.data(), &kIdentity) == GDI_ERROR)
        {
            return false;
        }
        const int bw = static_cast<int>(gm.gmBlackBoxX), bh = static_cast<int>(gm.gmBlackBoxY);
        const int pitch = (bw + 3) & ~3;
        const int cw = bw + 2 * kPad, chh = bh + 2 * kPad;
        int pageIndex = -1, u = 0, v = 0;
        if (!allocate(device, cw, chh, pageIndex, u, v))
        {
            return false;
        }
        g.originX = gm.gmptGlyphOrigin.x;
        g.originY = gm.gmptGlyphOrigin.y;
        Page& page = g_pages[pageIndex];
        RECT r{u, v, u + cw, v + chh};
        DDSURFACEDESC2 desc = {};
        desc.dwSize = sizeof(desc);
        if (FAILED(page.surface->Lock(&r, &desc, DDLOCK_WAIT | DDLOCK_WRITEONLY, nullptr)))
        {
            return false;
        }
        for (int y = 0; y < chh; ++y)
        {
            auto* row = static_cast<uint8_t*>(desc.lpSurface) + size_t(y) * desc.lPitch;
            for (int x = 0; x < cw; ++x)
            {
                const int gx = x - kPad, gy = y - kPad;
                uint32_t a = 0;
                if (gx >= 0 && gy >= 0 && gx < bw && gy < bh && size_t(gy) * pitch + gx < bits.size())
                {
                    a = std::min<uint32_t>(64, bits[size_t(gy) * pitch + gx]) * 255 / 64;
                }
                if (page.wide)
                {
                    reinterpret_cast<uint32_t*>(row)[x] = (a << 24) | 0xFFFFFF;
                }
                else
                {
                    reinterpret_cast<uint16_t*>(row)[x] = static_cast<uint16_t>(((a >> 4) << 12) | 0x0FFF);
                }
            }
        }
        page.surface->Unlock(&r);
        g.page = pageIndex;
        g.u = static_cast<uint16_t>(u);
        g.v = static_cast<uint16_t>(v);
        g.width = bw;
        g.height = bh;
        g.generation = g_generation;
        return true;
    }

    struct TLVertex
    {
        float x, y, z, rhw;
        DWORD diffuse, specular;
        float u, v;
    };
    constexpr DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1;

    struct Quad
    {
        int page;
        TLVertex v[6];
    };
}

bool Text::measure(const Style& style, const std::wstring& text, float scale, float& width, float& height)
{
    if (scale <= 0.0f)
    {
        return false;
    }
    std::scoped_lock lock(g_lock);
    Font* f = font(style, pixelSize(style, scale));
    if (!f)
    {
        return false;
    }
    int w = 0;
    const auto ls = lines(text);
    for (std::wstring_view line : ls)
    {
        w = std::max(w, lineWidth(*f, line));
    }
    width = w / scale;
    height = static_cast<float>(f->height * static_cast<int>(ls.size())) / scale;
    return true;
}

void Text::draw(IDirect3DDevice7* device, const Placement& placement, const Style& style, const std::wstring& text,
                float x, float y, float width, float height, int align, int valign, uint32_t color,
                SetTextureFn setTexture, void* ctx)
{
    if (!device || text.empty() || !(color >> 24) || placement.scale <= 0.0f)
    {
        return;
    }
    std::scoped_lock lock(g_lock);
    if (g_device != device)
    {
        // Another device (a new game, a mode change): its pages are not ours to release.
        startOver(false);
        g_device = device;
        g_failed = false;
    }
    if (g_failed)
    {
        return;
    }
    Font* f = font(style, pixelSize(style, placement.scale));
    if (!f)
    {
        return;
    }
    // Glyphs first (an atlas that fills up starts over once), then quads per page.
    std::vector<Quad> quads;
    const auto ls = lines(text);
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        quads.clear();
        bool full = false;
        // Screen pixels: the box's top-left on the pixel grid. Without SacredBild's canvas (plain Direct3D 7) a
        // pixel's middle lies half a unit in.
        const float bias = placement.scale == 1.0f && placement.originX == 0.0f && placement.originY == 0.0f ? -0.5f : 0.0f;
        const float left = std::round(placement.toScreenX(x));
        // The first line's baseline: under the box's top by the ascent; centred, the capitals' middle on the box's
        // middle (labels start with one; every line of a size shares its baseline, the lines' block centred around
        // it); at the bottom, the last line's descent on the box's bottom.
        const float top = placement.toScreenY(y), boxH = height * placement.scale;
        const float block = static_cast<float>(f->height * (static_cast<int>(ls.size()) - 1));
        float baseline = std::round(top) + f->ascent;
        if (valign == 1 && height > 0)
        {
            baseline = std::round(top + (boxH - block) / 2 + f->cap / 2.0f);
        }
        else if (valign == 2 && height > 0)
        {
            baseline = std::round(top + boxH) - (f->height - f->ascent) - block;
        }
        else if (valign == 3)
        {
            baseline = std::round(top);
        }
        const float boxW = width * placement.scale;
        for (std::wstring_view line : ls)
        {
            float pen = left;
            if (width > 0 && align)
            {
                const float lw = static_cast<float>(lineWidth(*f, line));
                pen += std::round(align == 1 ? (boxW - lw) / 2 : boxW - lw);
            }
            for (wchar_t ch : line)
            {
                Glyph& g = glyph(*f, ch);
                if (g.width > 0 && g.height > 0)
                {
                    if (g.generation != g_generation)
                    {
                        if (!upload(device, *f, ch, g))
                        {
                            full = !g_failed;
                            break;
                        }
                    }
                    if (g.page >= 0)
                    {
                        const float x0 = pen + g.originX - kPad + bias, y0 = baseline - g.originY - kPad + bias;
                        const float x1 = x0 + g.width + 2 * kPad, y1 = y0 + g.height + 2 * kPad;
                        const float u0 = float(g.u) / kPage, v0 = float(g.v) / kPage;
                        const float u1 = float(g.u + g.width + 2 * kPad) / kPage;
                        const float v1 = float(g.v + g.height + 2 * kPad) / kPage;
                        const float lx0 = placement.toLayoutX(x0), ly0 = placement.toLayoutY(y0);
                        const float lx1 = placement.toLayoutX(x1), ly1 = placement.toLayoutY(y1);
                        Quad q{g.page};
                        q.v[0] = {lx0, ly0, 0, 1, color, 0, u0, v0};
                        q.v[1] = {lx1, ly0, 0, 1, color, 0, u1, v0};
                        q.v[2] = {lx0, ly1, 0, 1, color, 0, u0, v1};
                        q.v[3] = {lx1, ly0, 0, 1, color, 0, u1, v0};
                        q.v[4] = {lx1, ly1, 0, 1, color, 0, u1, v1};
                        q.v[5] = {lx0, ly1, 0, 1, color, 0, u0, v1};
                        quads.push_back(q);
                    }
                }
                pen += g.advance;
            }
            if (full || g_failed)
            {
                break;
            }
            baseline += f->height;
        }
        if (g_failed)
        {
            return;
        }
        if (!full)
        {
            break;
        }
        startOver(true);
    }
    // One draw per page and quad: separate quads keep SacredBild's per-quad texel fitting (4 vertices each).
    for (size_t p = 0; p < g_pages.size(); ++p)
    {
        bool set = false;
        for (const Quad& q : quads)
        {
            if (q.page != static_cast<int>(p))
            {
                continue;
            }
            if (!set)
            {
                setTexture(ctx, g_pages[p].surface);
                set = true;
            }
            TLVertex strip[4] = {q.v[0], q.v[1], q.v[2], q.v[4]};
            device->DrawPrimitive(D3DPT_TRIANGLESTRIP, kFvf, strip, 4, 0);
        }
    }
}
