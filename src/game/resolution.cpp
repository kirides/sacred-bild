#include "game/resolution.h"
#include "game/sacred_de.h"
#include "config.h"
#include "log.h"
#include "patch.h"

#include <windows.h>
#include <ddraw.h>
#include <intrin.h>
#include <algorithm>
#include <cstring>

namespace
{
    using namespace Sacred;

    enum class Kind
    {
        MemW, MemH,                                 // operand -> &screen width/height (float)
        ProjNegW, ProjPosW, ProjNegH, ProjPosH,     // operand -> scaled ortho extent (double)
        ImmPosW, ImmNegW, ImmPosH, ImmNegH,         // float immediate: unzoomed ortho extent
        ImmFW, ImmFH,                               // float immediate: screen width/height
        ImmIW, ImmIH,                               // int immediate: screen width/height
        ImmHalfW, ImmHalfH,                         // int immediate: screen center (was 512/384)
        MemHalfW, MemHalfH,                         // operand -> &screen center (float, was 512.0/384.0)
        MemCullH, MemCullH2,                        // operand -> &ground-tile cull bottom (float, was 818.0 / 888.0)
        ImmCullW, ImmCullH,                         // int immediate: object cull bounds (was 1024 + 200 / 768 + 200)
        MemUnzX, MemUnzY,                           // operand -> &g_unzoomedProjection._11 * 1024/W (._22 * 768/H)
    };

    struct Site
    {
        Kind kind;
        uint32_t addr;
        uint32_t expected;
    };
#include "game/resolution_sites.inc"

    int g_width = 1024;
    int g_height = 768;

    // Redirect targets for patched memory operands; must live at fixed addresses.
    float g_widthF = 1024.0f;
    float g_heightF = 768.0f;
    float g_halfWF = 512.0f;
    float g_halfHF = 384.0f;
    float g_cullHF = 818.0f;
    float g_cullH2F = 888.0f;
    // g_unzoomedProjection (+0x00 = _11, +0x14 = _22) is built from +-267/+-200 and never patched.
    constexpr uintptr_t kUnzoomed11 = 0x0182CCF0;
    constexpr uintptr_t kUnzoomed22 = 0x0182CD04;
    float g_unzXF = 2.0f / 534.0f;
    float g_unzYF = 2.0f / 400.0f;
    double g_projNegW = -267.0;
    double g_projPosW = 267.0;
    double g_projNegH = -200.0;
    double g_projPosH = 200.0;

    uint32_t floatBits(double v)
    {
        const float f = static_cast<float>(v);
        uint32_t u;
        std::memcpy(&u, &f, 4);
        return u;
    }

    uint32_t replacement(Kind kind)
    {
        switch (kind)
        {
        case Kind::MemW: return reinterpret_cast<uint32_t>(&g_widthF);
        case Kind::MemH: return reinterpret_cast<uint32_t>(&g_heightF);
        case Kind::ProjNegW: return reinterpret_cast<uint32_t>(&g_projNegW);
        case Kind::ProjPosW: return reinterpret_cast<uint32_t>(&g_projPosW);
        case Kind::ProjNegH: return reinterpret_cast<uint32_t>(&g_projNegH);
        case Kind::ProjPosH: return reinterpret_cast<uint32_t>(&g_projPosH);
        case Kind::ImmPosW: return floatBits(g_projPosW);
        case Kind::ImmNegW: return floatBits(g_projNegW);
        case Kind::ImmPosH: return floatBits(g_projPosH);
        case Kind::ImmNegH: return floatBits(g_projNegH);
        case Kind::ImmFW: return floatBits(g_widthF);
        case Kind::ImmFH: return floatBits(g_heightF);
        case Kind::ImmIW: return static_cast<uint32_t>(g_width);
        case Kind::ImmIH: return static_cast<uint32_t>(g_height);
        case Kind::ImmHalfW: return static_cast<uint32_t>(g_width / 2);
        case Kind::ImmHalfH: return static_cast<uint32_t>(g_height / 2);
        case Kind::MemHalfW: return reinterpret_cast<uint32_t>(&g_halfWF);
        case Kind::MemHalfH: return reinterpret_cast<uint32_t>(&g_halfHF);
        case Kind::MemCullH: return reinterpret_cast<uint32_t>(&g_cullHF);
        case Kind::MemCullH2: return reinterpret_cast<uint32_t>(&g_cullH2F);
        case Kind::ImmCullW: return static_cast<uint32_t>(g_width + 200);
        case Kind::ImmCullH: return static_cast<uint32_t>(g_height + 200);
        case Kind::MemUnzX: return reinterpret_cast<uint32_t>(&g_unzXF);
        case Kind::MemUnzY: return reinterpret_cast<uint32_t>(&g_unzYF);
        }
        return 0;
    }

    // cDxDevices::findMode(w, h, bpp, flags) returns a 0x84-byte mode record (DDSURFACEDESC2 + extras).
    using FindModeFn = void*(__fastcall*)(void* self, void* edx, int w, int h, int bpp, int flags);
    FindModeFn g_origFindMode = reinterpret_cast<FindModeFn>(Addr::cDxDevices_findMode);
    uint8_t g_syntheticMode[0x84];

    void* __fastcall hookFindMode(void* self, void* edx, int w, int h, int bpp, int flags)
    {
        if (static_cast<uint16_t>(w) != 1024 || static_cast<uint16_t>(h) != 768)
        {
            return g_origFindMode(self, edx, w, h, bpp, flags);
        }
        if (void* mode = g_origFindMode(self, edx, g_width, g_height, bpp, flags))
        {
            LOG("Display mode {}x{}x{}", g_width, g_height, bpp);
            return mode;
        }
        void* base = g_origFindMode(self, edx, w, h, bpp, flags);
        if (!base)
        {
            return nullptr;
        }
        std::memcpy(g_syntheticMode, base, sizeof(g_syntheticMode));
        auto* desc = reinterpret_cast<DDSURFACEDESC2*>(g_syntheticMode);
        desc->dwWidth = static_cast<DWORD>(g_width);
        desc->dwHeight = static_cast<DWORD>(g_height);
        LOG("Display mode {}x{}x{} is not enumerated, using a synthetic mode", g_width, g_height, bpp);
        return g_syntheticMode;
    }

    // The loading screen and splash are GDI blits into a 1024x768 frame; center them.
    using BitBltFn = BOOL(WINAPI*)(HDC, int, int, int, int, HDC, int, int, DWORD);
    using DrawTextAFn = int(WINAPI*)(HDC, LPCSTR, int, LPRECT, UINT);
    using CreateWindowExAFn = HWND(WINAPI*)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
    BitBltFn g_origBitBlt;
    DrawTextAFn g_origDrawTextA;
    CreateWindowExAFn g_origCreateWindowExA;
    bool g_centerGdi = false;

    BOOL WINAPI hookBitBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop)
    {
        if (g_centerGdi || (x == 0 && y == 0 && w == 1024 && h == 768))
        {
            x += Resolution::centerX();
            y += Resolution::centerY();
        }
        return g_origBitBlt(dst, x, y, w, h, src, sx, sy, rop);
    }

    int WINAPI hookDrawTextA(HDC dc, LPCSTR text, int len, LPRECT rect, UINT format)
    {
        if (g_centerGdi && rect)
        {
            RECT r = *rect;
            OffsetRect(&r, Resolution::centerX(), Resolution::centerY());
            return g_origDrawTextA(dc, text, len, &r, format);
        }
        return g_origDrawTextA(dc, text, len, rect, format);
    }

    HWND WINAPI hookCreateWindowExA(DWORD exStyle, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h,
        HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
    {
        // Main game window: borderless so the client area matches the back buffer.
        if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == Addr::mainWindowCreateReturn && g_config.borderless)
        {
            LOG("Main window style {:08x} -> borderless", style);
            style = (style & WS_VISIBLE) | WS_POPUP;
            exStyle &= ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME);
        }
        return g_origCreateWindowExA(exStyle, cls, name, style, x, y, w, h, parent, menu, inst, param);
    }

    // The conversion helpers turn projection units into pixels with 1024/768. Callers pass either
    // g_unzoomedProjection (a 1024x768 view, kept original) or the device projection fetched with GetTransform
    // (widened to W x H): scale by the pixel size of whichever matrix they pass. Same math as the originals.
    using ConvertFn = float*(__cdecl*)(float* out, const float* p, const float* m, const float* proj);
    ConvertFn g_origPixelsToWorld = reinterpret_cast<ConvertFn>(Addr::pixelsToWorld);
    ConvertFn g_origWorldToPixels = reinterpret_cast<ConvertFn>(Addr::worldToPixels);

    bool isUnzoomed(const float* proj)
    {
        return proj == reinterpret_cast<const float*>(Addr::g_unzoomedProjection);
    }

    float* __cdecl hookPixelsToWorld(float* out, const float* p, const float* m, const float* proj)
    {
        const double sx = isUnzoomed(proj) ? 1024.0 : g_widthF;
        const double sy = isUnzoomed(proj) ? 768.0 : g_heightF;
        const double inv = 1.0 / (double(m[0]) * m[5] - double(m[4]) * m[1]);
        const double a = 2.0 / proj[0] * p[0] / sx;
        const double b = 2.0 / proj[5] * p[1] / sy;
        const float z = p[2];
        out[0] = static_cast<float>((m[5] * a - b * m[4]) * inv);
        out[1] = static_cast<float>((m[1] * a - m[0] * b) * inv);
        out[2] = z;
        return out;
    }

    float* __cdecl hookWorldToPixels(float* out, const float* p, const float* m, const float* proj)
    {
        const double sx = isUnzoomed(proj) ? 1024.0 : g_widthF;
        const double sy = isUnzoomed(proj) ? 768.0 : g_heightF;
        const double x = double(m[4]) * p[1] + double(m[8]) * p[2] + double(p[0]) * m[0];
        const double y = -(double(m[1]) * p[0]) - double(m[5]) * p[1] - double(m[9]) * p[2];
        const float z = p[2];
        out[0] = static_cast<float>(x * sx / (2.0 / proj[0]));
        out[1] = static_cast<float>(y * sy / (2.0 / proj[5]));
        out[2] = z;
        return out;
    }

    using LoadingScreenFn = void(__fastcall*)(void* self, void* edx, uint32_t progress, const char* text);
    LoadingScreenFn g_origLoadingScreen = reinterpret_cast<LoadingScreenFn>(Addr::dxDriver7_drawLoadingScreen);

    void __fastcall hookLoadingScreen(void* self, void* edx, uint32_t progress, const char* text)
    {
        auto* back = *reinterpret_cast<IDirectDrawSurface7**>(reinterpret_cast<uint8_t*>(self) + DxDriver::back);
        if (back)
        {
            DDBLTFX fx = {};
            fx.dwSize = sizeof(fx);
            back->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
        }
        g_centerGdi = true;
        g_origLoadingScreen(self, edx, progress, text);
        g_centerGdi = false;
    }

    void chooseSize()
    {
        int w = g_config.width;
        int h = g_config.height;
        if (w <= 0 || h <= 0)
        {
            w = GetSystemMetrics(SM_CXSCREEN);
            h = GetSystemMetrics(SM_CYSCREEN);
        }
        // The UI layout needs at least the original canvas.
        g_width = std::max(w, 1024);
        g_height = std::max(h, 768);
        g_widthF = static_cast<float>(g_width);
        g_heightF = static_cast<float>(g_height);
        g_halfWF = static_cast<float>(g_width / 2);
        g_halfHF = static_cast<float>(g_height / 2);
        g_cullHF = g_heightF + 50.0f;
        g_cullH2F = g_heightF + 120.0f;
        g_unzXF = 2.0f / 534.0f * 1024.0f / g_widthF;
        g_unzYF = 2.0f / 400.0f * 768.0f / g_heightF;
        // Keep the original world scale: 534x400 world units per 1024x768 pixels.
        g_projPosW = 267.0 * g_width / 1024.0;
        g_projNegW = -g_projPosW;
        g_projPosH = 200.0 * g_height / 768.0;
        g_projNegH = -g_projPosH;
    }
}

int Resolution::width() { return g_width; }
int Resolution::height() { return g_height; }
bool Resolution::active() { return g_width != 1024 || g_height != 768; }

void Resolution::refresh()
{
    const float m11 = *reinterpret_cast<const float*>(kUnzoomed11);
    const float m22 = *reinterpret_cast<const float*>(kUnzoomed22);
    if (m11 != 0.0f && m22 != 0.0f)
    {
        g_unzXF = m11 * 1024.0f / g_widthF;
        g_unzYF = m22 * 768.0f / g_heightF;
    }
}
int Resolution::centerX() { return (g_width - 1024) / 2; }
int Resolution::centerY() { return (g_height - 768) / 2; }

void Resolution::install()
{
    chooseSize();
    LOG("Resolution: {}x{}", g_width, g_height);
    if (!active())
    {
        return;
    }

    int patched = 0;
    for (const Site& s : kResolutionSites)
    {
        patched += Patch::imm32(s.addr, s.expected, replacement(s.kind)) ? 1 : 0;
    }
    LOG("Resolution: patched {}/{} sites", patched, std::size(kResolutionSites));

    g_origBitBlt = static_cast<BitBltFn>(Patch::iat("GDI32.dll", "BitBlt", reinterpret_cast<void*>(&hookBitBlt)));
    g_origDrawTextA = static_cast<DrawTextAFn>(Patch::iat("USER32.dll", "DrawTextA", reinterpret_cast<void*>(&hookDrawTextA)));
    g_origCreateWindowExA = static_cast<CreateWindowExAFn>(
        Patch::iat("USER32.dll", "CreateWindowExA", reinterpret_cast<void*>(&hookCreateWindowExA)));

    Patch::hook(g_origFindMode, &hookFindMode, "cDxDevices::findMode");
    Patch::hook(g_origLoadingScreen, &hookLoadingScreen, "dxDriver7::drawLoadingScreen");
    Patch::hook(g_origPixelsToWorld, &hookPixelsToWorld, "pixelsToWorld");
    Patch::hook(g_origWorldToPixels, &hookWorldToPixels, "worldToPixels");
}
