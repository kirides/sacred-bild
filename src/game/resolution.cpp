#include "game/resolution.h"
#include "game/focus.h"
#include "game/sacred_addr.h"
#include "game/ui_canvas.h"
#include "config.h"
#include "log.h"
#include "patch.h"
#include "sig.h"

#include <windows.h>
#include <ddraw.h>
#include <intrin.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

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
        ImmNegHalfW, ImmNegHalfH,                   // int immediate: minus screen center (was -512/-384)
        MemHalfW, MemHalfH,                         // operand -> &screen center (float, was 512.0/384.0)
        MemCullH, MemCullH2,                        // operand -> &ground-tile cull bottom (float, was 818.0 / 888.0)
        ImmCullW, ImmCullH,                         // int immediate: object cull bounds (was 1024 + 200 / 768 + 200)
        ImmPickLimit,                               // int immediate: max objects in the pick list (was 1000)
        ImmLayerCache,                              // int immediate: tile layer record cache size (was 0x1000)
        MemUnzX, MemUnzY,                           // operand -> &g_unzoomedProjection._11 * 1024/W (._22 * 768/H)
        ImmNear, ImmFar,                            // float immediate: world projection near/far plane (was -600 / 2500)
        MemDepthHalfH, MemDepthNear,                // operand -> &sprite depth constants (was 200.0 = 384 px, 600.0 = -near)
        MemDepthScale, MemDepthBias,                // (was 1/3100 = 1/(far - near), 0.002)
    };

    struct Site
    {
        Kind kind;
        const char* pattern;    // signature of the instruction (tools/gen_res_sites.py)
        uint16_t offset;        // of the patched operand in the match
        // What the operand holds before patching: immediates the value; memory operands the float bits of the
        // constant they point to (a double for Proj*); MemUnzX/Y the offset into g_unzoomedProjection.
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
    constexpr uintptr_t kUnzoomed22 = 0x14;
    float g_unzXF = 2.0f / 534.0f;
    float g_unzYF = 2.0f / 400.0f;
    double g_projNegW = -267.0;
    double g_projPosW = 267.0;
    double g_projNegH = -200.0;
    double g_projPosH = 200.0;
    double g_near = -600.0;
    double g_far = 2500.0;
    float g_depthHalfHF = 200.0f;
    float g_depthNegNearF = 600.0f;
    float g_depthScaleF = 1.0f / 3100.0f;
    float g_depthBiasF = 0.002f;

    uint32_t floatBits(double v)
    {
        const float f = static_cast<float>(v);
        uint32_t u;
        std::memcpy(&u, &f, 4);
        return u;
    }

    bool inExe(uintptr_t addr, size_t size)
    {
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        return addr >= base && addr + size <= base + nt->OptionalHeader.SizeOfImage;
    }

    // The site holds what the table expects: its address came from a signature, so check before patching.
    bool holdsExpected(const Site& s, uintptr_t addr)
    {
        uint32_t operand;
        std::memcpy(&operand, reinterpret_cast<const void*>(addr), 4);
        switch (s.kind)
        {
        case Kind::MemUnzX:
        case Kind::MemUnzY:
            return operand == Addr::g_unzoomedProjection + s.expected;
        case Kind::ProjNegW:
        case Kind::ProjPosW:
        case Kind::ProjNegH:
        case Kind::ProjPosH:
            return inExe(operand, 8) && floatBits(*reinterpret_cast<const double*>(operand)) == s.expected;
        case Kind::MemW:
        case Kind::MemH:
        case Kind::MemHalfW:
        case Kind::MemHalfH:
        case Kind::MemCullH:
        case Kind::MemCullH2:
        case Kind::MemDepthHalfH:
        case Kind::MemDepthNear:
        case Kind::MemDepthScale:
        case Kind::MemDepthBias:
            return inExe(operand, 4) && *reinterpret_cast<const uint32_t*>(operand) == s.expected;
        default:
            return operand == s.expected;
        }
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
        case Kind::ImmNegHalfW: return static_cast<uint32_t>(-(g_width / 2));
        case Kind::ImmNegHalfH: return static_cast<uint32_t>(-(g_height / 2));
        case Kind::MemHalfW: return reinterpret_cast<uint32_t>(&g_halfWF);
        case Kind::MemHalfH: return reinterpret_cast<uint32_t>(&g_halfHF);
        case Kind::MemCullH: return reinterpret_cast<uint32_t>(&g_cullHF);
        case Kind::MemCullH2: return reinterpret_cast<uint32_t>(&g_cullH2F);
        case Kind::ImmCullW: return static_cast<uint32_t>(g_width + 200);
        case Kind::ImmCullH: return static_cast<uint32_t>(g_height + 200);
        case Kind::ImmPickLimit: return 8000;
        case Kind::ImmLayerCache: return 0x8000;
        case Kind::MemUnzX: return reinterpret_cast<uint32_t>(&g_unzXF);
        case Kind::MemUnzY: return reinterpret_cast<uint32_t>(&g_unzYF);
        case Kind::ImmNear: return floatBits(g_near);
        case Kind::ImmFar: return floatBits(g_far);
        case Kind::MemDepthHalfH: return reinterpret_cast<uint32_t>(&g_depthHalfHF);
        case Kind::MemDepthNear: return reinterpret_cast<uint32_t>(&g_depthNegNearF);
        case Kind::MemDepthScale: return reinterpret_cast<uint32_t>(&g_depthScaleF);
        case Kind::MemDepthBias: return reinterpret_cast<uint32_t>(&g_depthBiasF);
        }
        return 0;
    }

    // cDxDevices::findMode(w, h, bpp, flags) returns a 0x84-byte mode record (DDSURFACEDESC2 + extras).
    using FindModeFn = void*(__fastcall*)(void* self, void* edx, int w, int h, int bpp, int flags);
    FindModeFn g_origFindMode = nullptr;
    uint8_t g_syntheticMode[0x84];

    void* __fastcall hookFindMode(void* self, void* edx, int w, int h, int bpp, int flags)
    {
        // Always 32 bits per pixel: GFX32 : 0 asks for a 16-bit mode, which only a fullscreen game uses (windowed
        // takes the desktop's format, textures their own formats either way).
        bpp = 32;
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
    thread_local bool t_nativeGdi = false;      // drawing into the 1024x768 loading canvas: nothing to move

    BOOL WINAPI hookBitBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop)
    {
        if (!t_nativeGdi && (g_centerGdi || (x == 0 && y == 0 && w == 1024 && h == 768)))
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

    // The game's main window is a frameless popup; dxDriver7::init sizes it with SetWindowPos (the screen in
    // fullscreen mode, the mode's size centered on the screen when windowed). A framed window gets that as its client
    // area instead.
    using SetWindowPosFn = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
    SetWindowPosFn g_origSetWindowPos;
    HWND g_mainWindow = nullptr;
    bool g_framed = false;
    constexpr DWORD kFramedStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

    bool wantFrame()
    {
        switch (g_config.frame)
        {
        case Config::Frame::Never: return false;
        case Config::Frame::Always: return true;
        case Config::Frame::Auto: break;
        }
        return g_width < GetSystemMetrics(SM_CXSCREEN) || g_height < GetSystemMetrics(SM_CYSCREEN);
    }

    // Window rectangle around the client rectangle `rect`, at the window's DPI.
    void addFrame(HWND window, RECT& rect)
    {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        using AdjustForDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
        static const HMODULE user32 = GetModuleHandleW(L"user32.dll");
        static const auto getDpi = reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(user32, "GetDpiForWindow"));
        static const auto adjustForDpi = reinterpret_cast<AdjustForDpiFn>(GetProcAddress(user32, "AdjustWindowRectExForDpi"));
        const DWORD style = static_cast<DWORD>(GetWindowLongA(window, GWL_STYLE));
        const DWORD exStyle = static_cast<DWORD>(GetWindowLongA(window, GWL_EXSTYLE));
        if (getDpi && adjustForDpi)
        {
            adjustForDpi(&rect, style, FALSE, exStyle, getDpi(window));
        }
        else
        {
            AdjustWindowRectEx(&rect, style, FALSE, exStyle);
        }
    }

    BOOL WINAPI hookSetWindowPos(HWND window, HWND after, int x, int y, int cx, int cy, UINT flags)
    {
        // dxDriver7::init's fullscreen branch sizes the window screen width x screen width (GetSystemMetrics(SM_CXSCREEN)
        // for both), which an exclusive mode never showed. The Direct3D 9 backend emulates fullscreen in the window and
        // presents into its client area: the mode's size centered on the screen, as when windowed.
        const int screenW = GetSystemMetrics(SM_CXSCREEN);
        if (g_config.ddrawD3D9 && x == 0 && y == 0 && cx == screenW && cy == screenW && flags == SWP_SHOWWINDOW)
        {
            const int screenH = GetSystemMetrics(SM_CYSCREEN);
            cx = g_width;
            cy = g_height;
            x = (screenW - cx) / 2;
            y = (screenH - cy) / 2;
            LOG("Main window: fullscreen as a {}x{} window", cx, cy);
        }
        if (window && window == g_mainWindow && g_framed && !(flags & SWP_NOSIZE))
        {
            RECT rect = {x, y, x + cx, y + cy};
            addFrame(window, rect);
            if (!(flags & SWP_NOMOVE))
            {
                // Keep the caption on the screen.
                MONITORINFO monitor = {};
                monitor.cbSize = sizeof(monitor);
                if (GetMonitorInfoW(MonitorFromRect(&rect, MONITOR_DEFAULTTOPRIMARY), &monitor))
                {
                    OffsetRect(&rect, std::max(0L, monitor.rcWork.left - rect.left), std::max(0L, monitor.rcWork.top - rect.top));
                }
            }
            x = rect.left;
            y = rect.top;
            cx = rect.right - rect.left;
            cy = rect.bottom - rect.top;
        }
        return g_origSetWindowPos(window, after, x, y, cx, cy, flags);
    }

    HWND WINAPI hookCreateWindowExA(DWORD exStyle, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h,
        HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
    {
        const bool mainWindow = reinterpret_cast<uintptr_t>(_ReturnAddress()) == Addr::mainWindowCreateReturn;
        if (mainWindow)
        {
            g_framed = wantFrame();
            if (g_framed)
            {
                style = (style & WS_VISIBLE) | kFramedStyle;
            }
            LOG("Main window: {}", g_framed ? "framed" : "borderless");
        }
        HWND window = g_origCreateWindowExA(exStyle, cls, name, style, x, y, w, h, parent, menu, inst, param);
        if (mainWindow && window)
        {
            g_mainWindow = window;
            Focus::windowCreated(window);
        }
        return window;
    }

    // The conversion helpers turn projection units into pixels with 1024/768. Callers pass either
    // g_unzoomedProjection (a 1024x768 view, kept original) or the device projection fetched with GetTransform
    // (widened to W x H): scale by the pixel size of whichever matrix they pass. Same math as the originals.
    using ConvertFn = float*(__cdecl*)(float* out, const float* p, const float* m, const float* proj);
    ConvertFn g_origPixelsToWorld = nullptr;
    ConvertFn g_origWorldToPixels = nullptr;

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

    // The ground layer array holds 0x6D5 tiles and the water/lava tile array 1750: enough for the original view,
    // not for a large zoomed-out one (later rows lost their blend layers; too many water tiles overwrite the
    // water count and crash). Layers are drawn early, between rows, before their array fills up: ground, then
    // layers, the order they have at the end of the frame. Water tiles are moved out of their array into a longer
    // list between rows and drawn at the end of the frame as usual, in parts the array holds; the water ambience
    // each part sets is summed up into the one the whole list gives. (Drawing them early too drew the ground of
    // later rows over them and left the ambience to the last part.) A row adds at most one entry per tile column
    // (~92 at 3840 wide, zoomed out).
    using TileRowFn = void(__fastcall*)(void* self, void* edx, void* device, void* rowPos, int detail);
    using DeviceFn = void(__fastcall*)(void* self, void* edx, void* device);
    using AmbienceFn = void(__fastcall*)(void* sound, void* edx, uint32_t count, int32_t x, int32_t y);
    TileRowFn g_origTileRow = nullptr;
    DeviceFn g_flushBatcher = nullptr;
    DeviceFn g_drawTileLayers = nullptr;
    DeviceFn g_drawWaterTiles = nullptr;
    DeviceFn g_origDrawWaterTiles = nullptr;
    AmbienceFn g_origAmbience = nullptr;
    constexpr uint32_t kRowMargin = 256;

    struct WaterList
    {
        void* view = nullptr;           // the view whose row walk started last (cleared there)
        std::vector<uint8_t> entries;   // whole entries moved out of its array, in walk order
        uint32_t most = 0;              // most tiles in one frame so far
        uint32_t logs = 0;
    };
    WaterList g_water;
    bool g_waterReady = false;          // both hooks in place

    // The ambience calls of the parts while they are drawn.
    struct AmbienceSum
    {
        bool capturing = false;
        void* sound = nullptr;          // set once a part called
        uint32_t part = 0;              // tiles in the part being drawn
        uint32_t tiles = 0;             // tiles in the parts drawn so far
        bool drawn = false;             // a part had tiles drawn (a position)
        int64_t x = 0, y = 0;           // their positions, weighted by their parts' sizes
    };
    AmbienceSum g_ambience;

    void __fastcall hookAmbience(void* sound, void* edx, uint32_t count, int32_t x, int32_t y)
    {
        if (!g_ambience.capturing)
        {
            g_origAmbience(sound, edx, count, x, y);
            return;
        }
        g_ambience.sound = sound;
        if (static_cast<uint16_t>(count) != 0)
        {
            g_ambience.drawn = true;
            g_ambience.x += static_cast<int64_t>(x) * g_ambience.part;
            g_ambience.y += static_cast<int64_t>(y) * g_ambience.part;
        }
    }

    void __fastcall hookDrawWaterTiles(void* self, void* edx, void* device)
    {
        if (self != g_water.view || g_water.entries.empty())
        {
            g_origDrawWaterTiles(self, edx, device);
            return;
        }
        auto* base = static_cast<uint8_t*>(self);
        auto& count = *reinterpret_cast<uint32_t*>(base + WorldView::waterTileCount);
        uint8_t* array = base + WorldView::waterTiles;
        auto& entries = g_water.entries;
        entries.insert(entries.end(), array, array + count * WorldView::waterTileSize);
        const auto total = static_cast<uint32_t>(entries.size() / WorldView::waterTileSize);
        if (total > g_water.most)
        {
            g_water.most = total;
            if (g_water.logs < 10)
            {
                ++g_water.logs;
                LOG("Water tiles: {} in one frame (the game's list holds {})", total, WorldView::waterTileCapacity);
            }
        }

        g_ambience = {};
        g_ambience.capturing = true;
        for (uint32_t first = 0; first < total; first += WorldView::waterTileCapacity)
        {
            const uint32_t n = std::min(WorldView::waterTileCapacity, total - first);
            std::memcpy(array, entries.data() + first * WorldView::waterTileSize, n * WorldView::waterTileSize);
            count = n;
            g_ambience.part = n;
            g_ambience.tiles += n;
            g_origDrawWaterTiles(self, edx, device);
        }
        g_ambience.capturing = false;
        entries.clear();
        if (g_ambience.sound)
        {
            const AmbienceSum& a = g_ambience;
            if (a.drawn)
            {
                g_origAmbience(a.sound, nullptr, std::min<uint32_t>(a.tiles, 0xFFFF),
                    static_cast<int32_t>(a.x / a.tiles), static_cast<int32_t>(a.y / a.tiles));
            }
            else
            {
                g_origAmbience(a.sound, nullptr, 0, 0, 0);
            }
        }
    }

    uintptr_t callTarget(uintptr_t site)
    {
        if (*reinterpret_cast<const uint8_t*>(site) != 0xE8)
        {
            return 0;
        }
        int32_t rel;
        std::memcpy(&rel, reinterpret_cast<const void*>(site + 1), sizeof(rel));
        return site + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(rel));
    }

    // The sound system's water ambience setter, from its calls in drawWaterTiles (0 if they do not agree).
    uintptr_t findAmbience()
    {
        if (!Addr::cWorldView_drawWaterTiles)
        {
            return 0;
        }
        uintptr_t target = 0;
        for (uintptr_t offset : WorldView::drawWaterTilesAmbienceCalls)
        {
            const uintptr_t t = callTarget(Addr::cWorldView_drawWaterTiles + offset);
            if (!t || (target && t != target))
            {
                return 0;
            }
            target = t;
        }
        return target;
    }

    // Row walk. Only the 3x3 sectors around the camera (64x64 tiles each) are loaded. cWorldView0_render walks the
    // rows by tile and sector index from the top-left corner of the view (initRowWalk) and steps over sector edges
    // with fixed index changes. The original view plus its margins (6 tiles left, 5 rows up, 19 rows down) always
    // lies inside the loaded sectors; a large zoomed-out one does not near a sector edge. The corner then lies in no
    // sector (-1, its tile index computed from out-of-range tables), the steps turn that into a valid sector with a
    // negative tile index, and renderTileRow reads before the tile table (crash at 0x62B093). Rows that run out of
    // the loaded sectors on the other side step into wrong sectors. Instead, every row gets its position in the
    // 192x192 loaded tiles from one anchor per frame and is clipped to them.
    struct RowPos
    {
        float x, y;
        int16_t tile;       // row * 64 + column within the sector
        int16_t sector;     // 3x3 grid: row * 3 + column
        int16_t steps;      // rows until the walk leaves the sector (cWorldView0_render only)
        int16_t pad;
    };

    enum class Walk { Off, Clip, Skip };

    struct RowWalkState
    {
        void* view = nullptr;
        Walk mode = Walk::Off;
        bool cornerInside = false;
        int row = 0, col = 0;   // loaded-tile position of the even start: +1/+1 per row down, -1/+1 per tile right
        int rows[2] = {};       // even/odd rows drawn so far (cWorldView0_render rebases x/y after initRowWalk)
    };
    RowWalkState g_walk;
    Walk g_walkOutside = Walk::Off;     // mode of the last frame whose corner was outside (Off: inside)
    uint32_t g_walkStateLogs = 0;       // logged changes into an outside corner
    uint32_t g_walkMismatchLogs = 0;    // logged disagreements with the game's walk (would be bugs here)

    using InitRowWalkFn = void(__fastcall*)(void* self, void* edx, const int32_t* pos, void* map);
    InitRowWalkFn g_origInitRowWalk = nullptr;

    RowPos* rowPos(void* view, uintptr_t offset)
    {
        return reinterpret_cast<RowPos*>(static_cast<uint8_t*>(view) + offset);
    }

    bool loadedTile(const RowPos& p, int& row, int& col)
    {
        if (p.sector < 0 || p.sector > 8 || p.tile < 0 || p.tile > 0xFFF)
        {
            return false;
        }
        row = p.sector / 3 * 64 + p.tile / 64;
        col = p.sector % 3 * 64 + p.tile % 64;
        return true;
    }

    int floorDiv(int a, int b)
    {
        return a / b - ((a % b != 0) && ((a < 0) != (b < 0)) ? 1 : 0);
    }

    void __fastcall hookInitRowWalk(void* self, void* edx, const int32_t* pos, void* map)
    {
        g_origInitRowWalk(self, edx, pos, map);
        RowPos* even = rowPos(self, WorldView::rowEven);
        RowPos* odd = rowPos(self, WorldView::rowOdd);
        g_walk.view = self;
        g_walk.rows[0] = 0;
        g_walk.rows[1] = 0;
        if (g_waterReady)
        {
            g_water.view = self;
            g_water.entries.clear();
        }
        g_walk.cornerInside = loadedTile(*even, g_walk.row, g_walk.col);

        // Look up a point next to the camera (always in the middle sector) and step back to the corner in whole
        // tiles: (96, 0) on screen is one tile right (row - 1, column + 1), (0, 48) one row down (+1, +1); both keep
        // the tile snapping of initRowWalk. Done every frame so a disagreement with the game's own corner shows up.
        const int a = floorDiv(*reinterpret_cast<const int32_t*>(Addr::g_viewCameraX) - pos[1], 96);
        const int b = floorDiv(*reinterpret_cast<const int32_t*>(Addr::g_viewCameraY) - pos[2], 48);
        const RowPos saved[2] = {*even, *odd};
        const int32_t probe[3] = {pos[0], pos[1] + a * 96, pos[2] + b * 48};
        g_origInitRowWalk(self, edx, probe, map);
        int row = 0;
        int col = 0;
        const bool probed = loadedTile(*even, row, col);
        *even = saved[0];
        *odd = saved[1];
        row -= b - a;
        col -= a + b;

        const Walk previous = g_walkOutside;
        if (g_walk.cornerInside)
        {
            g_walk.mode = Walk::Clip;
            g_walkOutside = Walk::Off;
            if (probed && (row != g_walk.row || col != g_walk.col) && g_walkMismatchLogs < 10)
            {
                ++g_walkMismatchLogs;
                LOG("Row walk: probe gives tile ({}, {}), the game's corner ({}, {})", row, col, g_walk.row, g_walk.col);
            }
            return;
        }
        g_walk.mode = probed ? Walk::Clip : Walk::Skip;
        g_walk.row = row;
        g_walk.col = col;
        g_walkOutside = g_walk.mode;
        if (g_walk.mode != previous && g_walkStateLogs < 50)
        {
            ++g_walkStateLogs;
            LOG("Row walk: view corner outside the loaded sectors (sector {}, tile {}), {}", saved[0].sector,
                saved[0].tile, probed ? Fmt::format("clipping rows from tile ({}, {})", row, col)
                                      : std::string("no tile next to the camera either, skipping the ground"));
        }
    }

    // Draws the part of walk row k that lies in the loaded sectors.
    void renderRowClipped(void* self, void* edx, void* device, RowPos* pos, int detail, int odd, int k)
    {
        auto* base = static_cast<uint8_t*>(self);
        const int row = g_walk.row + k;
        const int col = g_walk.col + k + odd;
        auto& length = *reinterpret_cast<int32_t*>(base + WorldView::rowLength);
        const int32_t n0 = length;
        // Tile j of the row is (row - j, col + j).
        const int first = std::max({0, row - 191, -col});
        const int last = std::min({n0 - 1, row, 191 - col});
        if (first > last)
        {
            return;
        }
        RowPos clipped = *pos;
        clipped.x += static_cast<float>(first) * 96.0f;
        const int r = row - first;
        const int c = col + first;
        clipped.sector = static_cast<int16_t>(r / 64 * 3 + c / 64);
        clipped.tile = static_cast<int16_t>(r % 64 * 64 + c % 64);
        if (first == 0 && last == n0 - 1)
        {
            if (clipped.sector == pos->sector && clipped.tile == pos->tile)
            {
                g_origTileRow(self, edx, device, pos, detail);
                return;
            }
            if (g_walk.cornerInside)
            {
                // The game's walk is right whenever its corner is: this would be a bug here, keep the game's row.
                if (g_walkMismatchLogs < 10)
                {
                    ++g_walkMismatchLogs;
                    LOG("Row walk: row at y {} is sector {} tile {}, computed {} / {}", pos->y, pos->sector, pos->tile,
                        clipped.sector, clipped.tile);
                }
                g_origTileRow(self, edx, device, pos, detail);
                return;
            }
        }
        // Ground is drawn for columns edgeLeft - 1 .. length - edgeRight + 1 (the rest are off-screen margins);
        // keep those columns where they were in the full row.
        auto& edgeLeft = *reinterpret_cast<int32_t*>(base + WorldView::rowEdgeLeft);
        auto& edgeRight = *reinterpret_cast<int32_t*>(base + WorldView::rowEdgeRight);
        const int32_t left0 = edgeLeft;
        const int32_t right0 = edgeRight;
        const int32_t n = last - first + 1;
        length = n;
        edgeLeft = left0 - first;
        edgeRight = right0 + n + first - n0;
        g_origTileRow(self, edx, device, &clipped, detail);
        length = n0;
        edgeLeft = left0;
        edgeRight = right0;
    }

    void __fastcall hookTileRow(void* self, void* edx, void* device, void* rowPosArg, int detail)
    {
        auto* base = static_cast<uint8_t*>(self);
        auto& layers = *reinterpret_cast<uint32_t*>(base + WorldView::layeredTileCount);
        auto& water = *reinterpret_cast<uint32_t*>(base + WorldView::waterTileCount);
        const bool waterList = self == g_water.view;
        if (waterList && water + kRowMargin > WorldView::waterTileCapacity)
        {
            const uint8_t* array = base + WorldView::waterTiles;
            g_water.entries.insert(g_water.entries.end(), array, array + water * WorldView::waterTileSize);
            water = 0;
        }
        if (layers + kRowMargin > WorldView::layeredTileCapacity || water + kRowMargin > WorldView::waterTileCapacity)
        {
            g_flushBatcher(base + WorldView::quadBatcher, nullptr, device);
            g_drawTileLayers(self, nullptr, device);
            layers = 0;
            if (water && !waterList)
            {
                g_drawWaterTiles(self, nullptr, device);
                water = 0;
            }
        }
        auto* pos = static_cast<RowPos*>(rowPosArg);
        if (self == g_walk.view && g_walk.mode != Walk::Off &&
            (pos == rowPos(self, WorldView::rowEven) || pos == rowPos(self, WorldView::rowOdd)))
        {
            const int odd = pos == rowPos(self, WorldView::rowOdd) ? 1 : 0;
            const int k = g_walk.rows[odd]++;
            if (g_walk.mode == Walk::Clip)
            {
                renderRowClipped(self, edx, device, pos, detail, odd, k);
            }
            return;
        }
        g_origTileRow(self, edx, device, rowPosArg, detail);
    }

    using TextureInitFn = uint32_t(__fastcall*)(void* self, void* edx, uint32_t budget);
    TextureInitFn g_origTextureInit = nullptr;

    uint32_t __fastcall hookTextureInit(void* self, void* edx, uint32_t budget)
    {
        const uint32_t wanted = g_config.textureBudgetMB > 0 ? static_cast<uint32_t>(g_config.textureBudgetMB) << 20
                                                             : std::max<uint32_t>(budget, 256u << 20);
        LOG("Texture budget: game {} MB, using {} MB", budget >> 20, wanted >> 20);
        return g_origTextureInit(self, edx, wanted);
    }

    using LoadingScreenFn = void(__fastcall*)(void* self, void* edx, uint32_t progress, const char* text);
    LoadingScreenFn g_origLoadingScreen = nullptr;

    // The loading screen draws with GDI into the back buffer and flips (only when its progress bar moved). With the
    // UI canvas the game draws it into a 1024x768 surface in place of the back buffer; Resolution::beforeFlip scales
    // that into the menus' canvas on the back buffer.
    IDirectDrawSurface7* g_loadingCanvas = nullptr;
    bool g_loadingCanvasFailed = false;
    thread_local IDirectDrawSurface7* t_loadingBack = nullptr;     // the back buffer while the canvas stands in

    IDirectDrawSurface7*& backBuffer(void* dxDriver)
    {
        return *reinterpret_cast<IDirectDrawSurface7**>(static_cast<uint8_t*>(dxDriver) + DxDriver::back);
    }

    IDirectDrawSurface7* loadingCanvas(void* dxDriver)
    {
        if (g_loadingCanvas || g_loadingCanvasFailed)
        {
            return g_loadingCanvas;
        }
        auto* ddraw = *reinterpret_cast<IDirectDraw7**>(static_cast<uint8_t*>(dxDriver) + DxDriver::ddraw);
        DDSURFACEDESC2 desc = {};
        desc.dwSize = sizeof(desc);
        desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
        desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
        desc.dwWidth = 1024;
        desc.dwHeight = 768;
        const HRESULT hr = ddraw ? ddraw->CreateSurface(&desc, &g_loadingCanvas, nullptr) : E_POINTER;
        if (FAILED(hr))
        {
            LOG("Loading screen: no 1024x768 surface ({:08x}), drawn centered", static_cast<uint32_t>(hr));
            g_loadingCanvas = nullptr;
            g_loadingCanvasFailed = true;
        }
        return g_loadingCanvas;
    }

    void __fastcall hookLoadingScreen(void* self, void* edx, uint32_t progress, const char* text)
    {
        IDirectDrawSurface7*& back = backBuffer(self);
        if (IDirectDrawSurface7* canvas = back && UiCanvas::enabled() ? loadingCanvas(self) : nullptr)
        {
            t_loadingBack = back;
            back = canvas;
            t_nativeGdi = true;
            g_origLoadingScreen(self, edx, progress, text);
            t_nativeGdi = false;
            if (t_loadingBack)
            {
                back = t_loadingBack;   // nothing new to show: no flip
                t_loadingBack = nullptr;
            }
            return;
        }
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
        // Depth: the camera sits 1341.6 units from its target (eye (0, 1200, 600)), so ground v world units above
        // the screen center is at depth 1341.6 + 2v. Scale the original range around that center with the height,
        // or the top rows of a taller view lie past the far plane: 3D models there lose their lower parts or vanish.
        const double scale = g_height / 768.0;
        const double eye = std::sqrt(1200.0 * 1200.0 + 600.0 * 600.0);
        g_near = eye - (eye + 600.0) * scale;
        g_far = eye + (2500.0 - eye) * scale;
        // Z-tested sprites compute their depth by hand: same range, center at H/2, bias kept in world units.
        g_depthHalfHF = static_cast<float>(g_projPosH);
        g_depthNegNearF = static_cast<float>(-g_near);
        g_depthScaleF = static_cast<float>(1.0 / (g_far - g_near));
        g_depthBiasF = static_cast<float>(0.002 / scale);
    }
}

int Resolution::width() { return g_width; }
int Resolution::height() { return g_height; }
bool Resolution::active() { return g_width != 1024 || g_height != 768; }

void Resolution::refresh()
{
    const float m11 = *reinterpret_cast<const float*>(Addr::g_unzoomedProjection);
    const float m22 = *reinterpret_cast<const float*>(Addr::g_unzoomedProjection + kUnzoomed22);
    if (m11 != 0.0f && m22 != 0.0f)
    {
        g_unzXF = m11 * 1024.0f / g_widthF;
        g_unzYF = m22 * 768.0f / g_heightF;
    }
}
void Resolution::beforeFlip(void* dxDriver)
{
    IDirectDrawSurface7* back = t_loadingBack;
    if (!back)
    {
        return;
    }
    t_loadingBack = nullptr;
    IDirectDrawSurface7*& current = backBuffer(dxDriver);
    IDirectDrawSurface7* canvas = current;
    current = back;
    DDBLTFX fx = {};
    fx.dwSize = sizeof(fx);
    back->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
    const UiCanvas::Bounds r = UiCanvas::menuCanvas();
    RECT dst = {std::lround(r.left), std::lround(r.top), std::lround(r.right), std::lround(r.bottom)};
    back->Blt(&dst, canvas, nullptr, DDBLT_WAIT, nullptr);
}

int Resolution::centerX() { return (g_width - 1024) / 2; }
int Resolution::centerY() { return (g_height - 768) / 2; }

void Resolution::install()
{
    chooseSize();
    LOG("Resolution: {}x{}, world depth range {:.0f}..{:.0f}", g_width, g_height, g_near, g_far);
    Patch::hook(g_origFindMode, Addr::cDxDevices_findMode, &hookFindMode, "cDxDevices::findMode");
    if (active() || g_config.ddrawD3D9)
    {
        g_origSetWindowPos = static_cast<SetWindowPosFn>(
            Patch::iat("USER32.dll", "SetWindowPos", reinterpret_cast<void*>(&hookSetWindowPos)));
    }
    if (!active())
    {
        return;
    }

    std::vector<uintptr_t> addrs(std::size(kResolutionSites));
    std::vector<Sig::Entry> entries;
    for (size_t i = 0; i < std::size(kResolutionSites); ++i)
    {
        const Site& s = kResolutionSites[i];
        entries.push_back({"resolution site", s.pattern, s.offset, Sig::Take::Match, &addrs[i]});
    }
    Sig::resolve(entries);
    int patched = 0;
    for (size_t i = 0; i < std::size(kResolutionSites); ++i)
    {
        const Site& s = kResolutionSites[i];
        if (!addrs[i])
        {
            continue;
        }
        if (!holdsExpected(s, addrs[i]))
        {
            LOG("Resolution: site {} ({:08x}) does not hold {:08x}", i, addrs[i], s.expected);
            continue;
        }
        patched += Patch::value(addrs[i], replacement(s.kind)) ? 1 : 0;
    }
    LOG("Resolution: patched {}/{} sites", patched, std::size(kResolutionSites));

    g_origBitBlt = static_cast<BitBltFn>(Patch::iat("GDI32.dll", "BitBlt", reinterpret_cast<void*>(&hookBitBlt)));
    g_origDrawTextA = static_cast<DrawTextAFn>(Patch::iat("USER32.dll", "DrawTextA", reinterpret_cast<void*>(&hookDrawTextA)));
    g_origCreateWindowExA = static_cast<CreateWindowExAFn>(
        Patch::iat("USER32.dll", "CreateWindowExA", reinterpret_cast<void*>(&hookCreateWindowExA)));

    g_flushBatcher = reinterpret_cast<DeviceFn>(Addr::cQuadBatcher_flush);
    g_drawTileLayers = reinterpret_cast<DeviceFn>(Addr::cWorldView_drawTileLayers);
    g_drawWaterTiles = reinterpret_cast<DeviceFn>(Addr::cWorldView_drawWaterTiles);
    Patch::hook(g_origLoadingScreen, Addr::dxDriver7_drawLoadingScreen, &hookLoadingScreen, "dxDriver7::drawLoadingScreen");
    Patch::hook(g_origTextureInit, Addr::cTextureManager_init, &hookTextureInit, "cTextureManager::init");
    Patch::hook(g_origTileRow, Addr::cWorldView_renderTileRow, &hookTileRow, "cWorldView::renderTileRow");
    Patch::hook(g_origInitRowWalk, Addr::cWorldView_initRowWalk, &hookInitRowWalk, "cWorldView::initRowWalk");
    const uintptr_t ambience = findAmbience();
    g_waterReady = ambience && Patch::hook(g_origAmbience, ambience, &hookAmbience, "water ambience") &&
        Patch::hook(g_origDrawWaterTiles, Addr::cWorldView_drawWaterTiles, &hookDrawWaterTiles, "cWorldView::drawWaterTiles");
    if (!g_waterReady)
    {
        LOG("Resolution: water ambience call not found, water tiles drawn early when their list fills up");
    }
    Patch::hook(g_origPixelsToWorld, Addr::pixelsToWorld, &hookPixelsToWorld, "pixelsToWorld");
    Patch::hook(g_origWorldToPixels, Addr::worldToPixels, &hookWorldToPixels, "worldToPixels");
}
