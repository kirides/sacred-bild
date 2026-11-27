#include "font_shadow.h"
#include "ddrawrhi/context.h"
#include "ddrawrhi/surface.h"
#include "engine/sacred_addr.h"
#include "settings/config/ddraw.h"
#include "settings/config/ui.h"
#include "ui/ui_canvas.h"
#include "log.h"
#include "patch.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <string>

namespace
{
    using namespace Sacred;

    // One glyph the game copied into a page: enough to draw it again at another scale.
    struct Glyph
    {
        LOGFONTW font;
        wchar_t ch;
        int srcX, srcY, w, h;       // in the game's 64x64 GDI cell (drawn from (1, 1))
        int dstX, dstY;             // in the page
    };

    struct Page
    {
        float scale = 0.0f;
        int size = 0;               // the copy's width and height
        int pageW = 0, pageH = 0;   // the game's page
        std::vector<uint32_t> pixels;
        std::vector<Glyph> glyphs;
        nvrhi::TextureHandle texture;
        bool dirty = false;
    };

    std::mutex g_lock;
    std::unordered_map<DDrawRhi::Surface*, Page> g_pages;

    // The glyph the game is rasterizing right now (between the rasterizer's start and its copy).
    struct Current
    {
        HFONT font = nullptr;
        wchar_t ch = 0;
    };
    thread_local Current t_current;

    // The UI's scale: the menus' canvas against 1024 (the in-game canvas is never larger), in steps of 1/8, rounded up.
    float uiScale()
    {
        const UiCanvas::Bounds b = UiCanvas::menuCanvas();
        const float s = (b.right - b.left) / 1024.0f;
        return std::clamp(std::ceil(s * 8.0f - 0.01f) / 8.0f, 1.0f, 4.0f);     // up: never coarser than the screen
    }

    // GDI at scale: a DIB section for one glyph and fonts by their LOGFONT (scaled).
    struct Gdi
    {
        HDC dc = nullptr;
        HBITMAP bitmap = nullptr;
        uint32_t* bits = nullptr;
        int size = 0;
        std::vector<std::pair<LOGFONTW, HFONT>> fonts;

        bool reserve(int s)
        {
            if (size >= s)
            {
                return true;
            }
            if (!dc)
            {
                dc = CreateCompatibleDC(nullptr);
            }
            if (bitmap)
            {
                DeleteObject(bitmap);
            }
            BITMAPINFO bi = {};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = s;
            bi.bmiHeader.biHeight = -s;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            void* p = nullptr;
            bitmap = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
            bits = static_cast<uint32_t*>(p);
            size = bitmap ? s : 0;
            if (bitmap)
            {
                SelectObject(dc, bitmap);
            }
            return bitmap != nullptr;
        }

        HFONT font(const LOGFONTW& lf)
        {
            for (auto& [key, f] : fonts)
            {
                if (std::memcmp(&key, &lf, sizeof(lf)) == 0)
                {
                    return f;
                }
            }
            HFONT f = CreateFontIndirectW(&lf);
            fonts.emplace_back(lf, f);
            return f;
        }
    };
    Gdi g_gdi;

    // Draws `g` at `page.scale` into the page's pixels. g_lock held.
    void render(Page& page, const Glyph& g)
    {
        const float s = page.scale;
        LOGFONTW lf = g.font;
        lf.lfHeight = static_cast<LONG>(std::lround(lf.lfHeight * s));
        lf.lfWidth = static_cast<LONG>(std::lround(lf.lfWidth * s));
        const int cell = static_cast<int>(std::ceil(66.0f * s));
        if (!g_gdi.reserve(cell))
        {
            return;
        }
        HFONT font = g_gdi.font(lf);
        if (!font)
        {
            return;
        }
        std::memset(g_gdi.bits, 0, size_t(g_gdi.size) * g_gdi.size * 4);
        HGDIOBJ old = SelectObject(g_gdi.dc, font);
        SetTextColor(g_gdi.dc, 0xFFFFFF);
        SetBkColor(g_gdi.dc, 0);
        SetBkMode(g_gdi.dc, OPAQUE);
        // As the game: the rectangle measured from (1, 1), then the character drawn into it.
        const int origin = static_cast<int>(std::lround(s));
        RECT r = {origin, origin, static_cast<LONG>(std::lround(64.0f * s)), static_cast<LONG>(std::lround(64.0f * s))};
        DrawTextW(g_gdi.dc, &g.ch, 1, &r, DT_SINGLELINE | DT_CALCRECT);
        DrawTextW(g_gdi.dc, &g.ch, 1, &r, DT_SINGLELINE);
        GdiFlush();
        SelectObject(g_gdi.dc, old);
        // The cell, scaled: the same scaled rectangle out of the drawing and into the page.
        const int x0 = static_cast<int>(std::lround(g.dstX * s)), y0 = static_cast<int>(std::lround(g.dstY * s));
        const int x1 = std::min(page.size, static_cast<int>(std::lround((g.dstX + g.w) * s)));
        const int y1 = std::min(page.size, static_cast<int>(std::lround((g.dstY + g.h) * s)));
        const int sx = static_cast<int>(std::lround(g.srcX * s)), sy = static_cast<int>(std::lround(g.srcY * s));
        for (int y = y0; y < y1; ++y)
        {
            uint32_t* out = page.pixels.data() + size_t(y) * page.size;
            const int srcRow = sy + (y - y0);
            for (int x = x0; x < x1; ++x)
            {
                const int srcCol = sx + (x - x0);
                uint32_t gray = 0;
                if (srcRow >= 0 && srcRow < g_gdi.size && srcCol >= 0 && srcCol < g_gdi.size)
                {
                    const uint32_t p = g_gdi.bits[size_t(srcRow) * g_gdi.size + srcCol];
                    gray = ((p >> 16 & 0xFF) + (p >> 8 & 0xFF) + (p & 0xFF)) / 3;
                }
                // White, the gray level as alpha (the game's 16-bit page does the same in 4 bits).
                out[x] = gray << 24 | 0x00FFFFFF;
            }
        }
        page.dirty = true;
    }

    // The page at the current UI scale: made (again) when the scale changed, every glyph drawn anew. g_lock held.
    void fit(Page& page, DDrawRhi::Surface& surface)
    {
        const float s = uiScale();
        if (page.scale == s && !page.pixels.empty())
        {
            return;
        }
        page.scale = s;
        page.pageW = static_cast<int>(surface.width());
        page.pageH = static_cast<int>(surface.height());
        page.size = static_cast<int>(std::lround(std::max(page.pageW, page.pageH) * s));
        page.pixels.assign(size_t(page.size) * page.size, 0x00FFFFFF);
        page.texture = nullptr;
        for (const Glyph& g : page.glyphs)
        {
            render(page, g);
        }
        page.dirty = true;
    }

    // Surface::texture() for a font page: the sharper copy, uploaded when it changed (Context lock held there).
    nvrhi::ITexture* pageTexture(DDrawRhi::Surface& surface)
    {
        if (!Config::ui.nativeFonts)
        {
            return nullptr;
        }
        std::scoped_lock lock(g_lock);
        const auto it = g_pages.find(&surface);
        if (it == g_pages.end() || it->second.glyphs.empty() || surface.width() != surface.height())
        {
            return nullptr;
        }
        Page& page = it->second;
        fit(page, surface);
        if (page.scale <= 1.0f)
        {
            return nullptr;
        }
        if (!page.texture)
        {
            page.texture = DDrawRhi::Context::device()->createTexture(nvrhi::TextureDesc().setWidth(page.size)
                .setHeight(page.size).setFormat(nvrhi::Format::BGRA8_UNORM).setDebugName("font page (UI scale)")
                .setInitialState(nvrhi::ResourceStates::ShaderResource).setKeepInitialState(true).setSharesMemory(true));
            page.dirty = true;
        }
        if (page.texture && page.dirty)
        {
            DDrawRhi::Context::commands()->writeTexture(page.texture, 0, 0, page.pixels.data(), size_t(page.size) * 4);
            page.dirty = false;
        }
        return page.texture;
    }

    void pageGone(DDrawRhi::Surface* surface)
    {
        std::scoped_lock lock(g_lock);
        g_pages.erase(surface);
    }

    // thiscall (the caller sets ECX, the copy does not read it) with four stack arguments it pops itself.
    using CopyFn = void(__fastcall*)(void* ecx, void* edx, const DDSURFACEDESC2* dst, const DDSURFACEDESC2* src, const int* dstPos,
        const int* srcRect);
    CopyFn g_origCopy = nullptr;
    CopyFn g_origCopyTtf = nullptr;
    using DrawTextAFn = int(WINAPI*)(HDC, LPCSTR, int, LPRECT, UINT);
    DrawTextAFn g_origDrawTextA = nullptr;

    using DrawTextWFn = int(WINAPI*)(HDC, LPCWSTR, int, LPRECT, UINT);
    DrawTextWFn g_origDrawTextW = nullptr;
    DrawTextWFn* g_drawTextWSlot = nullptr;
    int WINAPI hookDrawTextW(HDC dc, LPCWSTR text, int count, LPRECT rect, UINT format);

    // cFontTTF2 draws each character with DrawTextW through its Unicode layer's pointer (measure, then draw) into
    // the GDI surface it copies from: the character and the font selected, for the copy. The pointer resolves
    // itself on its first call (writing over this hook): put back, the function it resolved to called from then on.
    int WINAPI hookDrawTextW(HDC dc, LPCWSTR text, int count, LPRECT rect, UINT format)
    {
        const int result = g_origDrawTextW(dc, text, count, rect, format);
        if (*g_drawTextWSlot != &hookDrawTextW)
        {
            g_origDrawTextW = *g_drawTextWSlot;
            *g_drawTextWSlot = &hookDrawTextW;
        }
        if (text && count == 1 && !(format & DT_CALCRECT))
        {
            t_current = {static_cast<HFONT>(GetCurrentObject(dc, OBJ_FONT)), text[0]};
        }
        return result;
    }

    // cFontTTF draws each character with DrawTextA (measure, then draw) into the GDI surface it copies from: the
    // character and the font selected, for the copy.
    int WINAPI hookDrawTextA(HDC dc, LPCSTR text, int count, LPRECT rect, UINT format)
    {
        const int result = g_origDrawTextA(dc, text, count, rect, format);
        if (text && count == 1 && !(format & DT_CALCRECT))
        {
            wchar_t ch = 0;
            if (MultiByteToWideChar(CP_ACP, 0, text, 1, &ch, 1) == 1)
            {
                t_current = {static_cast<HFONT>(GetCurrentObject(dc, OBJ_FONT)), ch};
            }
        }
        return result;
    }

    void addGlyph(const DDSURFACEDESC2* dst, const int* dstPos, const int* srcRect)
    {
        const Current c = t_current;
        if (!c.font || !Config::ui.nativeFonts || !dst || !dstPos || !srcRect)
        {
            return;
        }
        DDrawRhi::Surface* page = DDrawRhi::Surface::lastLocked(dst->lpSurface);
        if (!page)
        {
            return;
        }
        Glyph g = {};
        if (!GetObjectW(c.font, sizeof(g.font), &g.font))
        {
            return;
        }
        g.ch = c.ch;
        g.srcX = srcRect[0];
        g.srcY = srcRect[1];
        g.w = *reinterpret_cast<const uint16_t*>(srcRect + 2);
        g.h = *reinterpret_cast<const uint16_t*>(srcRect + 3);
        g.dstX = dstPos[0];
        g.dstY = dstPos[1];
        std::scoped_lock lock(g_lock);
        Page& p = g_pages[page];
        page->setFontPage(true);
        // A cell used again replaces its old glyph.
        std::erase_if(p.glyphs, [&](const Glyph& o) { return o.dstX == g.dstX && o.dstY == g.dstY; });
        p.glyphs.push_back(g);
        if (!p.pixels.empty() && p.scale == uiScale())
        {
            render(p, g);
        }
    }

    void __fastcall hookCopy(void* ecx, void* edx, const DDSURFACEDESC2* dst, const DDSURFACEDESC2* src, const int* dstPos,
        const int* srcRect)
    {
        g_origCopy(ecx, edx, dst, src, dstPos, srcRect);
        addGlyph(dst, dstPos, srcRect);
        t_current = {};
    }

    void __fastcall hookCopyTtf(void* ecx, void* edx, const DDSURFACEDESC2* dst, const DDSURFACEDESC2* src, const int* dstPos,
        const int* srcRect)
    {
        g_origCopyTtf(ecx, edx, dst, src, dstPos, srcRect);
        addGlyph(dst, dstPos, srcRect);
        t_current = {};
    }
}

void FontShadow::install()
{
    if (!Config::ddraw.nvrhi || !Config::ui.nativeFonts || !Addr::fontDrawTextW || !Addr::fontCopyGlyph)
    {
        return;
    }
    g_drawTextWSlot = reinterpret_cast<DrawTextWFn*>(static_cast<uintptr_t>(Addr::fontDrawTextW));
    g_origDrawTextW = *g_drawTextWSlot;
    if (!g_origDrawTextW)
    {
        LOG("Native fonts: the game's DrawTextW pointer is not set yet; fonts stay as the game draws them");
        return;
    }
    *g_drawTextWSlot = &hookDrawTextW;
    DDrawRhi::Surface::setFontPageHooks(&pageTexture, &pageGone);
    Patch::hook(g_origCopy, static_cast<uintptr_t>(Addr::fontCopyGlyph), &hookCopy, "glyph copy into a font page");
    if (Addr::fontCopyGlyphTtf)
    {
        g_origDrawTextA = reinterpret_cast<DrawTextAFn>(Patch::iat("USER32.dll", "DrawTextA", reinterpret_cast<void*>(&hookDrawTextA)));
        if (g_origDrawTextA)
        {
            Patch::hook(g_origCopyTtf, static_cast<uintptr_t>(Addr::fontCopyGlyphTtf), &hookCopyTtf, "cFontTTF glyph copy");
        }
    }
    LOG("Native fonts: the game's glyphs are drawn again at the UI's scale");
}
