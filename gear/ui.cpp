#include "ui.h"
#include "text.h"
#include "automation.h"
#include "log.h"
#include "patch.h"
#include "game/gear_addr.h"

#include <windows.h>
#include <ddraw.h>
#include <d3d.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

// See ui.h. The game's UI pass, cUI_Manager_render (ENG 00758ED0, docs/UI_SYSTEM.md section 7), draws everything as
// pretransformed (XYZRHW) quads in its 1024x768 layout on the Direct3D 7 device it gets as its argument; the overlay
// draws the same way from three points inside it, so it layers with the game's windows and SacredBild maps it with
// the rest of the UI. SacredBild hooks the render function itself; the overlay uses other bytes
// (tools/check_gear_overlap.py).

namespace
{
    using Ui::Command;
    using Ui::Fit;
    using Ui::Kind;
    using Ui::Layer;

    constexpr size_t kLayers = static_cast<size_t>(Layer::Count);
    constexpr size_t kMaxCommands = 16384;      // per frame, all mods
    constexpr ULONGLONG kStaleMs = 500;

    std::mutex g_lock;
    std::array<std::vector<Command>, kLayers> g_recording;
    size_t g_recorded = 0;
    std::array<std::vector<Command>, kLayers> g_published;
    ULONGLONG g_publishedAt = 0;

    // The draws of one layer, copied out of the published frame (the Lua thread may publish the next one meanwhile).
    thread_local std::vector<Command> t_draw;

    // ---- Memory reads that may fault (the UI objects come and go with the game) ----

    template <class T>
    bool read(uintptr_t address, T& out)
    {
        __try
        {
            std::memcpy(&out, reinterpret_cast<const void*>(address), sizeof(T));
            return true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            return false;
        }
    }

    template <class T>
    T readOr(uintptr_t address, T fallback)
    {
        T v{};
        return read(address, v) ? v : fallback;
    }

    // ---- The UI manager (docs/UI_SYSTEM.md sections 2 and 3) ----

    constexpr int kWindowSlots = 23;            // +0x80 .. +0xD8
    constexpr uint32_t kManagerMenus = 0x01, kManagerInGame = 0x04, kManagerWindows = 0x08,
                       kManagerCinematic = 0x10, kManagerHelp = 0x200;
    // The windows the manager draws alone in game, first visible wins (savegame, options, character, megamap).
    constexpr int kExclusiveSlots[] = {11, 15, 20, 5};
    constexpr int kEscapeMenuSlot = 12;

    uintptr_t manager()
    {
        return Gear::Addr::g_pUiManager ? reinterpret_cast<uintptr_t>(*Gear::Addr::g_pUiManager) : 0;
    }

    uint32_t managerFlags(uintptr_t m) { return readOr<uint32_t>(m + 0x08, 0); }

    // The window in `slot`, 0 while the in-game windows do not exist (their pointers are stale then).
    uintptr_t windowAt(uintptr_t m, int slot)
    {
        if (!m || slot < 0 || slot >= kWindowSlots || !(managerFlags(m) & kManagerWindows))
        {
            return 0;
        }
        return readOr<uintptr_t>(m + 0x80 + slot * 4, 0);
    }

    bool visible(uintptr_t control) { return control && (readOr<uint32_t>(control + 0x10, 0) & 1); }

    // The window the manager draws alone in game this frame, -1 if none.
    int exclusiveSlot(uintptr_t m)
    {
        for (int slot : kExclusiveSlots)
        {
            if (visible(windowAt(m, slot)))
            {
                return slot;
            }
        }
        return -1;
    }

    std::string controlName(uintptr_t control)
    {
        char name[0x21] = {};
        if (!read(control + 0x30, *reinterpret_cast<char(*)[0x20]>(name)))
        {
            return {};
        }
        name[0x20] = 0;
        for (char& c : name)
        {
            if (c && (c < 0x20 || c > 0x7E))
            {
                return {};
            }
        }
        return name;
    }

    // The control names by slot (docs/UI_SYSTEM.md): what a window's name field holds is not reliable (the
    // statistics window's read empty in some runs, the net portraits' holds junk).
    static const char* const kSlotNames[kWindowSlots] = {
        "UI_WND_TASKBAR", "UI_WND_INVENTORY", "UI_WND_EQUIPMENT", "UI_WND_BLACKSMITH", "UI_WND_MERCHANT",
        "UI_WND_MEGAMAP", "UI_WND_OVERVIEWMAP", "UI_WND_MERC", "UI_WND_STATS_CHAR", "UI_WND_CONSOLE",
        "UI_WND_QUESTBOOK", "UI_WND_SAVEGAME", "UI_WND_ESCMENU", "UI_WND_PURCHASE", "UI_WND_MASTER",
        "UI_WND_OPTIONS", "UI_WND_CHEST", "UI_WND_HORSE", "UI_WND_NETINFO", "UI_WND_NETPORTRAIT", "UI_CHARACTER",
        "UI_WND_CUBE", "UI_WND_TRADE"};

    std::string windowName(int slot, uintptr_t w)
    {
        return slot >= 0 && slot < kWindowSlots ? kSlotNames[slot] : controlName(w);
    }

    Ui::WindowInfo windowInfo(uintptr_t m, int slot, uintptr_t w, int exclusive)
    {
        Ui::WindowInfo info;
        info.slot = slot;
        info.name = windowName(slot, w);
        info.x = readOr<int32_t>(w + 0x24, 0);
        info.y = readOr<int32_t>(w + 0x28, 0);
        info.width = readOr<int16_t>(w + 0x2C, 0);
        info.height = readOr<int16_t>(w + 0x2E, 0);
        info.visible = visible(w);
        const bool inGame = (managerFlags(m) & (kManagerInGame | kManagerMenus | kManagerCinematic)) == kManagerInGame;
        info.drawn = info.visible && inGame && (exclusive < 0 || exclusive == slot || slot == kEscapeMenuSlot);
        return info;
    }

    // ---- SacredBild (optional): its UI frames, so overlays drawn for a window move with it ----

    using PushFrameFn = int(__cdecl*)(void* window, int confine);
    using PopFrameFn = void(__cdecl*)();
    using BoundsFn = int(__cdecl*)(float* bounds);
    using SetWindowFrameFn = int(__cdecl*)(int slot, float x, float y, int set);
    using GetWindowFrameFn = int(__cdecl*)(int slot, float* x, float* y);
    using WindowBoundsFn = int(__cdecl*)(int slot, float* rect);
    using CursorFilterFn = int(__cdecl*)(int x, int y);
    using SetCursorFilterFn = int(__cdecl*)(CursorFilterFn filter);
    using PlacementFn = int(__cdecl*)(float* out);
    PlacementFn g_placement = nullptr;
    SetWindowFrameFn g_setWindowFrame = nullptr;
    WindowBoundsFn g_windowBounds = nullptr;
    SetCursorFilterFn g_setCursorFilter = nullptr;
    GetWindowFrameFn g_getWindowFrame = nullptr;
    PushFrameFn g_pushFrame = nullptr;
    PopFrameFn g_popFrame = nullptr;
    BoundsFn g_screenBounds = nullptr;
    std::once_flag g_bildOnce;

    void findBild()
    {
        std::call_once(g_bildOnce, [] {
            HMODULE bild = GetModuleHandleW(L"ddraw.dll");
            if (!bild)
            {
                return;
            }
            auto push = reinterpret_cast<PushFrameFn>(GetProcAddress(bild, "SacredBild_UiPushFrame"));
            auto pop = reinterpret_cast<PopFrameFn>(GetProcAddress(bild, "SacredBild_UiPopFrame"));
            if (push && pop)
            {
                g_pushFrame = push;
                g_popFrame = pop;
                LOG("UI: SacredBild's UI frames found: overlays follow its HUD layout");
            }
            g_screenBounds = reinterpret_cast<BoundsFn>(GetProcAddress(bild, "SacredBild_UiScreenBounds"));
            g_setWindowFrame = reinterpret_cast<SetWindowFrameFn>(GetProcAddress(bild, "SacredBild_UiSetWindowFrame"));
            g_getWindowFrame = reinterpret_cast<GetWindowFrameFn>(GetProcAddress(bild, "SacredBild_UiGetWindowFrame"));
            g_windowBounds = reinterpret_cast<WindowBoundsFn>(GetProcAddress(bild, "SacredBild_UiGetWindowBounds"));
            g_setCursorFilter = reinterpret_cast<SetCursorFilterFn>(GetProcAddress(bild, "SacredBild_UiSetCursorFilter"));
            g_placement = reinterpret_cast<PlacementFn>(GetProcAddress(bild, "SacredBild_UiPlacement"));
        });
    }

    // Where the calling thread's frame lands on the screen (SacredBild's canvas); 1, 0, 0 without it.
    Text::Placement placement()
    {
        findBild();
        float p[3];
        if (g_placement && g_placement(p) && p[0] > 0.0f)
        {
            return {p[0], p[1], p[2]};
        }
        return {};
    }

    // ---- Fonts (cFontManager, ENG 00CDCA70: a vector of cFont* by id) ----

    // cFont vtable: +0x18 draw(device, cRect*, const wchar_t* text, ARGB color, flags), +0x28 lineHeight(ch) (low
    // 16 bits), +0x2C textWidth(text) (the widest line; only cFontTTF2 measures, the others return 0).
    struct cRect
    {
        int32_t x, y;
        int16_t w, h;
    };
    using DrawTextFn = int(__fastcall*)(void* font, void* edx, IDirect3DDevice7* device, cRect* rect,
                                        const wchar_t* text, uint32_t color, uint32_t flags);
    using LineHeightFn = uint32_t(__fastcall*)(void* font, void* edx, uint32_t ch);
    using TextWidthFn = uint16_t(__fastcall*)(void* font, void* edx, const wchar_t* text);

    void* fontById(uint32_t id)
    {
        const uintptr_t fm = Gear::Addr::g_pFontManager ? reinterpret_cast<uintptr_t>(*Gear::Addr::g_pFontManager) : 0;
        if (!fm)
        {
            return nullptr;
        }
        const uintptr_t begin = readOr<uintptr_t>(fm, 0), end = readOr<uintptr_t>(fm + 4, 0);
        if (!begin || end < begin || id >= (end - begin) / 4)
        {
            return nullptr;
        }
        return readOr<void*>(begin + id * 4, nullptr);
    }

    template <class Fn>
    Fn fontSlot(void* font, size_t offset)
    {
        const uintptr_t vtable = readOr<uintptr_t>(reinterpret_cast<uintptr_t>(font), 0);
        return vtable ? reinterpret_cast<Fn>(readOr<uintptr_t>(vtable + offset, 0)) : nullptr;
    }

    // The text's size; false if the font does not measure. Under a fault guard.
    bool measure(void* font, const wchar_t* text, int& width, int& height)
    {
        auto widthFn = fontSlot<TextWidthFn>(font, 0x2C);
        auto heightFn = fontSlot<LineHeightFn>(font, 0x28);
        if (!widthFn || !heightFn)
        {
            return false;
        }
        int lines = 1;
        for (const wchar_t* p = text; *p; ++p)
        {
            lines += *p == L'\n';
        }
        __try
        {
            width = widthFn(font, nullptr, text);
            height = static_cast<int16_t>(heightFn(font, nullptr, L'A') & 0xFFFF) * lines;
            return width > 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // ---- UI images: 0x54-byte records (+0x08 TGA name, +0x28 texture handle, +0x2C..+0x38 u0 v0 u1 v1,
    //      +0x4C / +0x50 width / height) ----

    constexpr uint32_t kImageStride = 0x54;
    constexpr uint32_t kMaxImages = 4096;

    uintptr_t imageTable()
    {
        return Gear::Addr::uiImageTextures ? static_cast<uintptr_t>(Gear::Addr::uiImageTextures) - 0x28 : 0;
    }

    uint32_t g_imageCount = 0;
    std::once_flag g_imageOnce;

    uint32_t countImages()
    {
        std::call_once(g_imageOnce, [] {
            const uintptr_t t = imageTable();
            for (uint32_t i = 1; t && i < kMaxImages; ++i)
            {
                char name[12] = {};
                if (!read(t + i * kImageStride + 8, name) || std::memcmp(name, "INVALID_MAX", 11) == 0)
                {
                    g_imageCount = i;
                    break;
                }
            }
            LOG("UI: {} UI images", g_imageCount);
        });
        return g_imageCount;
    }

    struct ImageRecord
    {
        uint32_t texture;
        float u0, v0, u1, v1;
        int width, height;
    };

    bool imageRecord(uint32_t id, ImageRecord& out)
    {
        const uintptr_t t = imageTable();
        if (!t || id == 0 || id >= countImages())
        {
            return false;
        }
        const uintptr_t r = t + id * kImageStride;
        float uv[4] = {};
        if (!read(r + 0x28, out.texture) || !read(r + 0x2C, uv) || !read(r + 0x4C, out.width) || !read(r + 0x50, out.height))
        {
            return false;
        }
        // The game turns the pixel rects into texture coordinates when the UI starts (0x761580); before that they
        // are pixels of the 256x256 sheet.
        const bool pixels = uv[2] > 1.5f || uv[3] > 1.5f;
        const float s = pixels ? 1.0f / 256.0f : 1.0f;
        out.u0 = uv[0] * s;
        out.v0 = uv[1] * s;
        out.u1 = uv[2] * s;
        out.v1 = uv[3] * s;
        if (out.width <= 0 || out.height <= 0)
        {
            out.width = static_cast<int>(std::lround(std::fabs(out.u1 - out.u0) * 256.0f));
            out.height = static_cast<int>(std::lround(std::fabs(out.v1 - out.v0) * 256.0f));
        }
        return true;
    }

    IDirectDrawSurface7* imageSurface(uint32_t handle)
    {
        void* tm = Gear::Addr::g_pTextureManager ? *Gear::Addr::g_pTextureManager : nullptr;
        if (!tm || !handle || !Gear::Addr::cTextureManager_get)
        {
            return nullptr;
        }
        uint8_t* texture = Gear::Addr::cTextureManager_get(tm, handle, 0);
        return texture ? readOr<IDirectDrawSurface7*>(reinterpret_cast<uintptr_t>(texture) + 0x14, nullptr) : nullptr;
    }

    // ---- Device state: what the overlay's quads need, read before and set back after ----

    struct TLVertex
    {
        float x, y, z, rhw;
        DWORD diffuse, specular;
        float u, v;
    };
    constexpr DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1;   // 0x1C4, as the game's UI

    constexpr D3DRENDERSTATETYPE kRenderStates[] = {
        D3DRENDERSTATE_ZENABLE, D3DRENDERSTATE_ZWRITEENABLE, D3DRENDERSTATE_ALPHABLENDENABLE, D3DRENDERSTATE_SRCBLEND,
        D3DRENDERSTATE_DESTBLEND, D3DRENDERSTATE_ALPHATESTENABLE, D3DRENDERSTATE_CULLMODE, D3DRENDERSTATE_FOGENABLE,
        D3DRENDERSTATE_SPECULARENABLE, D3DRENDERSTATE_STENCILENABLE, D3DRENDERSTATE_COLORKEYENABLE,
        D3DRENDERSTATE_SHADEMODE,
    };
    struct StageState
    {
        DWORD stage;
        D3DTEXTURESTAGESTATETYPE type;
    };
    constexpr StageState kStageStates[] = {
        {0, D3DTSS_COLOROP}, {0, D3DTSS_COLORARG1}, {0, D3DTSS_COLORARG2}, {0, D3DTSS_ALPHAOP},
        {0, D3DTSS_ALPHAARG1}, {0, D3DTSS_ALPHAARG2}, {0, D3DTSS_MAGFILTER}, {0, D3DTSS_MINFILTER},
        {0, D3DTSS_MIPFILTER}, {0, D3DTSS_ADDRESSU}, {0, D3DTSS_ADDRESSV}, {0, D3DTSS_TEXCOORDINDEX},
        {0, D3DTSS_TEXTURETRANSFORMFLAGS}, {1, D3DTSS_COLOROP}, {1, D3DTSS_ALPHAOP},
    };

    class StateGuard
    {
    public:
        explicit StateGuard(IDirect3DDevice7* device) : m_device(device) {}
        ~StateGuard() { restore(); }

        // Before an overlay quad: saves the game's states once, then sets the overlay's.
        void enter()
        {
            if (m_saved)
            {
                return;
            }
            for (size_t i = 0; i < std::size(kRenderStates); ++i)
            {
                m_rs[i] = 0;
                m_device->GetRenderState(kRenderStates[i], &m_rs[i]);
            }
            for (size_t i = 0; i < std::size(kStageStates); ++i)
            {
                m_tss[i] = 0;
                m_device->GetTextureStageState(kStageStates[i].stage, kStageStates[i].type, &m_tss[i]);
            }
            m_texture = nullptr;
            m_device->GetTexture(0, &m_texture);
            m_saved = true;
            m_currentTexture = reinterpret_cast<IDirectDrawSurface7*>(-1);

            m_device->SetRenderState(D3DRENDERSTATE_ZENABLE, D3DZB_FALSE);
            m_device->SetRenderState(D3DRENDERSTATE_ZWRITEENABLE, FALSE);
            m_device->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, TRUE);
            m_device->SetRenderState(D3DRENDERSTATE_SRCBLEND, D3DBLEND_SRCALPHA);
            m_device->SetRenderState(D3DRENDERSTATE_DESTBLEND, D3DBLEND_INVSRCALPHA);
            m_device->SetRenderState(D3DRENDERSTATE_ALPHATESTENABLE, FALSE);
            m_device->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
            m_device->SetRenderState(D3DRENDERSTATE_FOGENABLE, FALSE);
            m_device->SetRenderState(D3DRENDERSTATE_SPECULARENABLE, FALSE);
            m_device->SetRenderState(D3DRENDERSTATE_STENCILENABLE, FALSE);
            m_device->SetRenderState(D3DRENDERSTATE_COLORKEYENABLE, FALSE);
            m_device->SetRenderState(D3DRENDERSTATE_SHADEMODE, D3DSHADE_GOURAUD);
            m_device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            m_device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            m_device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            m_device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
            m_device->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTFG_LINEAR);
            m_device->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTFN_LINEAR);
            m_device->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTFP_NONE);
            m_device->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
            m_device->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
            m_device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
            m_device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
            m_device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
            m_device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        }

        void texture(IDirectDrawSurface7* surface)
        {
            if (surface == m_currentTexture)
            {
                return;
            }
            m_currentTexture = surface;
            m_device->SetTexture(0, surface);
            const DWORD op = surface ? D3DTOP_MODULATE : D3DTOP_SELECTARG2;
            m_device->SetTextureStageState(0, D3DTSS_COLOROP, op);
            m_device->SetTextureStageState(0, D3DTSS_ALPHAOP, op);
        }

        // The game's states back (before a game function draws, and at the end).
        void restore()
        {
            if (!m_saved)
            {
                return;
            }
            m_saved = false;
            m_device->SetTexture(0, m_texture);
            if (m_texture)
            {
                m_texture->Release();
                m_texture = nullptr;
            }
            for (size_t i = 0; i < std::size(kStageStates); ++i)
            {
                m_device->SetTextureStageState(kStageStates[i].stage, kStageStates[i].type, m_tss[i]);
            }
            for (size_t i = 0; i < std::size(kRenderStates); ++i)
            {
                m_device->SetRenderState(kRenderStates[i], m_rs[i]);
            }
        }

    private:
        IDirect3DDevice7* m_device;
        bool m_saved = false;
        DWORD m_rs[std::size(kRenderStates)] = {};
        DWORD m_tss[std::size(kStageStates)] = {};
        IDirectDrawSurface7* m_texture = nullptr;
        IDirectDrawSurface7* m_currentTexture = nullptr;
    };

    void quad(IDirect3DDevice7* device, float x0, float y0, float x1, float y1, uint32_t color,
              float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1)
    {
        TLVertex v[4] = {
            {x0, y0, 0.0f, 1.0f, color, 0, u0, v0},
            {x1, y0, 0.0f, 1.0f, color, 0, u1, v0},
            {x0, y1, 0.0f, 1.0f, color, 0, u0, v1},
            {x1, y1, 0.0f, 1.0f, color, 0, u1, v1},
        };
        device->DrawPrimitive(D3DPT_TRIANGLESTRIP, kFvf, v, 4, 0);
    }

    // ---- Drawing one layer ----

    void drawRect(IDirect3DDevice7* device, StateGuard& state, const Command& c)
    {
        if (c.w <= 0 || c.h <= 0 || !(c.color >> 24))
        {
            return;
        }
        state.enter();
        state.texture(nullptr);
        // Edges on the screen's pixel grid: a 1-unit line is as wide everywhere, never smeared over two pixels.
        const Text::Placement p = placement();
        float x0 = Text::snapX(p, c.x), y0 = Text::snapY(p, c.y);
        float x1 = Text::snapX(p, c.x + c.w), y1 = Text::snapY(p, c.y + c.h);
        if (x1 <= x0)
        {
            x1 = x0 + 1.0f / p.scale;
        }
        if (y1 <= y0)
        {
            y1 = y0 + 1.0f / p.scale;
        }
        quad(device, x0, y0, x1, y1, c.color);
    }

    void setTextureThunk(void* ctx, void* surface)
    {
        static_cast<StateGuard*>(ctx)->texture(static_cast<IDirectDrawSurface7*>(surface));
    }

    void drawVectorText(IDirect3DDevice7* device, StateGuard& state, const Command& c)
    {
        const Text::Placement p = placement();
        const int align = (c.flags & 1) ? 1 : (c.flags & 4) ? 2 : 0;
        if (c.background >> 24)
        {
            float tw = 0, th = 0;
            if (Text::measure(c.style, c.text, p.scale, tw, th))
            {
                float bx = c.x;
                if (c.w > 0 && align)
                {
                    bx += align == 1 ? (c.w - tw) * 0.5f : c.w - tw;
                }
                Command r;
                r.x = bx - c.padding;
                r.y = c.y - c.padding;
                r.w = tw + 2 * c.padding;
                r.h = (c.h > 0 ? c.h : th) + 2 * c.padding;
                r.color = c.background;
                drawRect(device, state, r);
            }
        }
        state.enter();
        const float dark = static_cast<float>(c.color >> 24) * 0.85f;
        const uint32_t shade = static_cast<uint32_t>(dark) << 24;
        const float px = 1.0f / p.scale;
        if (c.style.flags & Text::Outline)
        {
            for (const auto& o : {std::pair{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}})
            {
                Text::draw(device, p, c.style, c.text, c.x + o.first * px, c.y + o.second * px, c.w, c.h, align,
                           c.valign, shade, setTextureThunk, &state);
            }
        }
        else if (c.style.flags & Text::Shadow)
        {
            Text::draw(device, p, c.style, c.text, c.x + px, c.y + px, c.w, c.h, align, c.valign, shade, setTextureThunk,
                       &state);
        }
        Text::draw(device, p, c.style, c.text, c.x, c.y, c.w, c.h, align, c.valign, c.color, setTextureThunk, &state);
    }

    void drawImage(IDirect3DDevice7* device, StateGuard& state, const Command& c)
    {
        ImageRecord r{};
        if (!imageRecord(c.image, r) || !(c.color >> 24))
        {
            return;
        }
        IDirectDrawSurface7* surface = imageSurface(r.texture);
        if (!surface)
        {
            return;
        }
        float w = c.w > 0 ? c.w : static_cast<float>(r.width);
        float h = c.h > 0 ? c.h : static_cast<float>(r.height);
        float x = c.x, y = c.y;
        float u0 = r.u0, v0 = r.v0, u1 = r.u1, v1 = r.v1;
        if (c.fit != Fit::Stretch && r.width > 0 && r.height > 0 && w > 0 && h > 0)
        {
            const float sx = w / r.width, sy = h / r.height;
            if (c.fit == Fit::Contain)
            {
                // The whole image, as large as fits, centered in the box.
                const float s = std::min(sx, sy);
                x += (w - r.width * s) * 0.5f;
                y += (h - r.height * s) * 0.5f;
                w = r.width * s;
                h = r.height * s;
            }
            else
            {
                // The box covered, the image's overhang cut off evenly.
                const float s = std::max(sx, sy);
                const float cropU = (1.0f - w / (r.width * s)) * 0.5f * (u1 - u0);
                const float cropV = (1.0f - h / (r.height * s)) * 0.5f * (v1 - v0);
                u0 += cropU;
                u1 -= cropU;
                v0 += cropV;
                v1 -= cropV;
            }
        }
        if (w <= 0 || h <= 0)
        {
            return;
        }
        state.enter();
        state.texture(surface);
        const Text::Placement p = placement();
        quad(device, Text::snapX(p, x), Text::snapY(p, y), Text::snapX(p, x + w), Text::snapY(p, y + h), c.color, u0, v0,
             u1, v1);
    }

    void drawText(IDirect3DDevice7* device, StateGuard& state, const Command& c)
    {
        void* font = fontById(c.font);
        auto draw = font ? fontSlot<DrawTextFn>(font, 0x18) : nullptr;
        if (!draw || c.text.empty())
        {
            return;
        }
        float w = c.w, h = c.h;
        if (c.background >> 24 || w <= 0 || h <= 0)
        {
            int tw = 0, th = 0;
            if (measure(font, c.text.c_str(), tw, th))
            {
                if (w <= 0)
                {
                    w = static_cast<float>(tw);
                }
                if (h <= 0)
                {
                    h = static_cast<float>(th);
                }
            }
        }
        float tx = c.x;     // where the font's rect starts
        if (c.background >> 24)
        {
            float bx = c.x;
            if (c.w > 0 && (c.flags & 5))
            {
                // A centered or right-aligned text's background hugs the text, not the box.
                int tw = 0, th = 0;
                if (measure(font, c.text.c_str(), tw, th))
                {
                    bx += (c.flags & 1) ? (c.w - tw) * 0.5f : c.w - tw;
                    w = static_cast<float>(tw);
                }
            }
            state.enter();
            state.texture(nullptr);
            quad(device, bx - c.padding, c.y - c.padding, bx + w + c.padding, c.y + h + c.padding, c.background);
            tx = bx;
        }
        // The font sets its own states through the game's state cache: give it the game's.
        state.restore();
        float ty = c.y;
        if (c.h > 0 && (c.valign == 1 || c.valign == 2))
        {
            int tw = 0, th = 0;
            if (measure(font, c.text.c_str(), tw, th) && th > 0)
            {
                ty += c.valign == 1 ? (c.h - th) * 0.5f : c.h - th;
                h = static_cast<float>(th);
            }
        }
        cRect rect{static_cast<int32_t>(std::lround(tx)), static_cast<int32_t>(std::lround(ty)),
                   static_cast<int16_t>(std::clamp(w > 0 ? w : 1024.0f, 1.0f, 4096.0f)),
                   static_cast<int16_t>(std::clamp(h > 0 ? h : 32.0f, 1.0f, 4096.0f))};
        draw(font, nullptr, device, &rect, c.text.c_str(), c.color, c.flags);
    }

    // A command that faults (a font or texture the game freed meanwhile) is skipped; the rest still draw.
    bool drawGuarded(IDirect3DDevice7* device, StateGuard& state, const Command& c)
    {
        __try
        {
            switch (c.kind)
            {
            case Kind::Rect: drawRect(device, state, c); break;
            case Kind::Image: drawImage(device, state, c); break;
            case Kind::Text: c.vector ? drawVectorText(device, state, c) : drawText(device, state, c); break;
            case Kind::Region: break;
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    std::atomic<uint32_t> g_faults{0};

    void drawLayer(Layer layer, IDirect3DDevice7* device)
    {
        if (!device)
        {
            return;
        }
        t_draw.clear();
        {
            std::scoped_lock lock(g_lock);
            if (!g_publishedAt || GetTickCount64() - g_publishedAt > kStaleMs)
            {
                return;
            }
            const auto& list = g_published[static_cast<size_t>(layer)];
            if (list.empty())
            {
                return;
            }
            t_draw = list;
        }
        findBild();
        const uintptr_t m = manager();
        StateGuard state(device);
        // The frame the commands run in: the window's (confined to its 1024x768 rect, like the window), or the
        // canvas unconfined, so overlays may reach beyond the canvas to the screen's edges.
        int frameSlot = -2;     // -2: no frame chosen yet
        bool pushed = false;
        for (const Command& c : t_draw)
        {
            const uintptr_t w = c.window >= 0 ? windowAt(m, c.window) : 0;
            if (c.window >= 0 && !w)
            {
                continue;   // the window is gone (left the game)
            }
            if (g_pushFrame && c.window != frameSlot)
            {
                state.restore();
                if (pushed)
                {
                    g_popFrame();
                }
                pushed = g_pushFrame(reinterpret_cast<void*>(w), w ? 1 : 0) != 0;
                frameSlot = c.window;
            }
            if (!drawGuarded(device, state, c) && g_faults.fetch_add(1) < 5)
            {
                LOG("UI: a draw command faulted (kind {}, layer {}); skipped", static_cast<int>(c.kind),
                    static_cast<int>(layer));
            }
        }
        state.restore();
        if (pushed)
        {
            g_popFrame();
        }
    }

    // ---- The hooks ----

    decltype(Gear::Addr::cUI_Manager_renderPopups)::Ptr g_origPopups = nullptr;
    using PartyArrowsFn = void(__cdecl*)(void* device);
    PartyArrowsFn g_origWorldCall = nullptr;
    uintptr_t g_origTopCall = 0;

    void __fastcall hookPopups(void* manager, void* edx, void* device, int flag)
    {
        drawLayer(Layer::Hud, static_cast<IDirect3DDevice7*>(device));
        g_origPopups(manager, edx, device, flag);
    }

    void __cdecl hookWorldCall(void* device)
    {
        drawLayer(Layer::World, static_cast<IDirect3DDevice7*>(device));
        g_origWorldCall(device);
    }

    void __cdecl drawTop(void* device)
    {
        drawLayer(Layer::Top, static_cast<IDirect3DDevice7*>(device));
    }

    // At `call 0x649BE0` (no arguments; the next call's argument is already on the stack): EBX is the render's
    // device. drawTop keeps EBX, ESI, EDI and EBP; EAX, ECX and EDX are free at a call.
    __declspec(naked) void hookTopCall()
    {
        __asm
        {
            push ebx
            call drawTop
            add esp, 4
            jmp dword ptr [g_origTopCall]
        }
    }

    uintptr_t callTarget(uintptr_t site)
    {
        return site + 5 + *reinterpret_cast<const int32_t*>(site + 1);
    }

    // ---- Input ----

    struct HitRegion
    {
        Layer layer;
        float x, y, w, h;
        uint32_t handle;
    };
    std::vector<HitRegion> g_regions;           // of the published frame, bottom to top (g_lock)
    std::vector<Ui::InputEvent> g_events;       // until the next tick (g_lock)
    uint32_t g_captured = 0;                    // the region a captured press went to, 0: none (g_lock)
    constexpr size_t kMaxEvents = 256;

    decltype(Gear::Addr::cUI_Manager_receiveEvent)::Ptr g_origReceiveEvent = nullptr;

    struct MouseEvent
    {
        uintptr_t vtable;
        int32_t button;
        int32_t x, y;
    };

    // The region under (x, y), 0 if none: the topmost; world and hud regions are covered by the escape menu and
    // message boxes.
    uint32_t regionAt(uintptr_t m, int x, int y)
    {
        if (!g_publishedAt || GetTickCount64() - g_publishedAt > kStaleMs)
        {
            return 0;
        }
        const bool covered = visible(windowAt(m, kEscapeMenuSlot)) || visible(readOr<uintptr_t>(m + 0x144, 0));
        for (auto it = g_regions.rbegin(); it != g_regions.rend(); ++it)
        {
            if (covered && it->layer != Layer::Top)
            {
                continue;
            }
            if (x >= it->x && y >= it->y && x < it->x + it->w && y < it->y + it->h)
            {
                return it->handle;
            }
        }
        return 0;
    }

    uint8_t __fastcall hookReceiveEvent(void* manager, void* edx, void* event)
    {
        MouseEvent e{};
        const uintptr_t down = Gear::Addr::cEventMouseDown_vtable, up = Gear::Addr::cEventMouseUp_vtable;
        if (event && down && up && read(reinterpret_cast<uintptr_t>(event), e) && (e.vtable == down || e.vtable == up))
        {
            std::scoped_lock lock(g_lock);
            uint32_t region = 0;
            if (e.vtable == down)
            {
                region = regionAt(reinterpret_cast<uintptr_t>(manager), e.x, e.y);
                if (region)
                {
                    g_captured = region;
                }
            }
            else if (g_captured)
            {
                region = g_captured;
                g_captured = 0;
            }
            if (region)
            {
                if (g_events.size() < kMaxEvents)
                {
                    g_events.push_back({e.vtable == down, e.button, e.x, e.y, region});
                }
                return 1;   // handled: the game's windows and the world do not see it
            }
        }
        return g_origReceiveEvent(manager, edx, event);
    }

    // SacredBild asks it once per UI frame and for the world's pick: while the cursor is over a mod's region, the game's
    // windows see the cursor off the screen (no hover, no tooltips under mod UI) and the world picks nothing there.
    int __cdecl cursorOverModUi(int x, int y)
    {
        const uintptr_t m = manager();
        std::scoped_lock lock(g_lock);
        return regionAt(m, x, y) != 0;
    }

    // Whether the foreground window belongs to the game.
    bool gameHasFocus()
    {
        HWND w = GetForegroundWindow();
        DWORD pid = 0;
        if (w)
        {
            GetWindowThreadProcessId(w, &pid);
        }
        return pid == GetCurrentProcessId();
    }
}

std::vector<Ui::InputEvent> Ui::takeEvents()
{
    std::scoped_lock lock(g_lock);
    std::vector<InputEvent> out;
    out.swap(g_events);
    return out;
}

bool Ui::keyDown(int vk)
{
    // Automation's keys count like the game's own key queries see them (Gear's GetAsyncKeyState is not hooked).
    if (vk <= 0 || vk >= 256)
    {
        return false;
    }
    if (Automation::keyHeld(vk))
    {
        return true;
    }
    return !Automation::isolatedInput() && gameHasFocus() && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

bool Ui::add(Command&& command)
{
    if (command.layer >= Layer::Count)
    {
        return false;
    }
    std::scoped_lock lock(g_lock);
    if (g_recorded >= kMaxCommands)
    {
        return false;
    }
    ++g_recorded;
    g_recording[static_cast<size_t>(command.layer)].push_back(std::move(command));
    return true;
}

void Ui::commit()
{
    std::scoped_lock lock(g_lock);
    for (size_t i = 0; i < kLayers; ++i)
    {
        g_published[i].swap(g_recording[i]);
        g_recording[i].clear();
    }
    g_recorded = 0;
    g_regions.clear();
    for (size_t i = 0; i < kLayers; ++i)
    {
        for (const Command& c : g_published[i])
        {
            if (c.kind == Kind::Region && c.w > 0 && c.h > 0)
            {
                g_regions.push_back({c.layer, c.x, c.y, c.w, c.h, c.region});
            }
        }
    }
    g_publishedAt = GetTickCount64();
}

bool Ui::install()
{
    namespace A = Gear::Addr;
    if (!A::cUI_Manager_renderPopups || !A::uiRenderWorldCall || !A::uiRenderTopCall || !A::g_pUiManager)
    {
        LOG("UI: the UI pass was not found: mods cannot draw");
        return false;
    }
    bool ok = Patch::hook(g_origPopups, Gear::Addr::cUI_Manager_renderPopups, hookPopups, "cUI_Manager_renderPopups");
    if (*reinterpret_cast<const uint8_t*>(A::uiRenderWorldCall) == 0xE8 &&
        *reinterpret_cast<const uint8_t*>(A::uiRenderTopCall) == 0xE8)
    {
        g_origWorldCall = reinterpret_cast<PartyArrowsFn>(callTarget(A::uiRenderWorldCall));
        g_origTopCall = callTarget(A::uiRenderTopCall);
        ok = Patch::redirectCall(Gear::Addr::uiRenderWorldCall, reinterpret_cast<const void*>(&hookWorldCall)) && ok;
        ok = Patch::redirectCall(Gear::Addr::uiRenderTopCall, reinterpret_cast<const void*>(&hookTopCall)) && ok;
    }
    else
    {
        LOG("UI: the world / top draw calls are not calls: those layers stay empty");
    }
    if (A::cUI_Manager_receiveEvent && A::cEventMouseDown_vtable && A::cEventMouseUp_vtable)
    {
        ok = Patch::hook(g_origReceiveEvent, Gear::Addr::cUI_Manager_receiveEvent, hookReceiveEvent,
                         "cUI_Manager_receiveEvent") && ok;
    }
    else
    {
        LOG("UI: the UI's event handler was not found: mod UI takes no mouse input");
    }
    findBild();
    if (g_setCursorFilter && g_setCursorFilter(&cursorOverModUi))
    {
        LOG("UI: the game's windows and the world do not see the cursor over mod UI");
    }
    return ok;
}

uint32_t Ui::imageCount() { return countImages(); }

bool Ui::imageInfo(uint32_t id, ImageInfo& out)
{
    ImageRecord r{};
    if (!imageRecord(id, r))
    {
        return false;
    }
    char name[0x21] = {};
    read(imageTable() + id * kImageStride + 8, *reinterpret_cast<char(*)[0x20]>(name));
    out.sheet = name;
    out.width = r.width;
    out.height = r.height;
    out.loaded = r.texture != 0;
    return true;
}

uint32_t Ui::fontCount()
{
    const uintptr_t fm = Gear::Addr::g_pFontManager ? reinterpret_cast<uintptr_t>(*Gear::Addr::g_pFontManager) : 0;
    const uintptr_t begin = fm ? readOr<uintptr_t>(fm, 0) : 0, end = fm ? readOr<uintptr_t>(fm + 4, 0) : 0;
    return begin && end > begin ? static_cast<uint32_t>((end - begin) / 4) : 0;
}

bool Ui::textSize(const Text::Style& style, const std::wstring& text, float& width, float& height)
{
    return Text::measure(style, text, placement().scale, width, height);
}

bool Ui::textSize(uint16_t font, const std::wstring& text, int& width, int& height)
{
    void* f = fontById(font);
    return f && !text.empty() && measure(f, text.c_str(), width, height);
}

std::vector<Ui::WindowInfo> Ui::windows()
{
    std::vector<WindowInfo> out;
    const uintptr_t m = manager();
    const int exclusive = exclusiveSlot(m);
    for (int slot = 0; slot < kWindowSlots; ++slot)
    {
        if (const uintptr_t w = windowAt(m, slot))
        {
            WindowInfo info = windowInfo(m, slot, w, exclusive);
            findBild();
            float fx = 0, fy = 0;
            if (g_getWindowFrame && g_getWindowFrame(slot, &fx, &fy))
            {
                info.frameX = fx;
                info.frameY = fy;
                // Not the windows the game draws alone (world map, savegame, options, character export).
                info.movable = g_setWindowFrame && slot != 5 && slot != 11 && slot != 15 && slot != 20;
            }
            float rect[4];
            if (g_windowBounds && g_windowBounds(slot, rect))
            {
                info.hasBounds = true;
                info.boundsX = rect[0] + info.frameX;   // its own layout -> the canvas, where it is now
                info.boundsY = rect[1] + info.frameY;
                info.boundsW = rect[2];
                info.boundsH = rect[3];
            }
            out.push_back(std::move(info));
        }
    }
    return out;
}

bool Ui::window(const std::string& name, WindowInfo& out)
{
    for (WindowInfo& w : windows())
    {
        if (_stricmp(w.name.c_str(), name.c_str()) == 0)
        {
            out = std::move(w);
            return true;
        }
    }
    return false;
}

Ui::State Ui::state()
{
    State s;
    const uintptr_t m = manager();
    findBild();
    float bounds[4];
    if (g_screenBounds && g_screenBounds(bounds))
    {
        s.screenLeft = bounds[0];
        s.screenTop = bounds[1];
        s.screenRight = bounds[2];
        s.screenBottom = bounds[3];
    }
    if (!m)
    {
        return s;
    }
    s.available = true;
    const uint32_t flags = managerFlags(m);
    s.mode = flags & kManagerCinematic ? "cinematic" : flags & kManagerMenus ? "menus" : flags & kManagerInGame ? "game" : "none";
    s.help = flags & kManagerHelp;
    const int exclusive = std::strcmp(s.mode, "game") == 0 ? exclusiveSlot(m) : -1;
    if (exclusive >= 0)
    {
        s.exclusive = windowName(exclusive, windowAt(m, exclusive));
    }
    s.escapeMenu = visible(windowAt(m, kEscapeMenuSlot));
    s.messageBox = visible(readOr<uintptr_t>(m + 0x144, 0));
    if (Gear::Addr::cMouse_instance)
    {
        if (const uint8_t* mouse = Gear::Addr::cMouse_instance())
        {
            s.mouseX = readOr<int32_t>(reinterpret_cast<uintptr_t>(mouse) + 4, 0);
            s.mouseY = readOr<int32_t>(reinterpret_cast<uintptr_t>(mouse) + 8, 0);
        }
    }
    return s;
}

bool Ui::canMoveWindows()
{
    findBild();
    float x = 0, y = 0;
    return g_setWindowFrame && g_getWindowFrame && g_getWindowFrame(0, &x, &y) != 0;
}

bool Ui::moveWindow(int slot, float x, float y, bool set)
{
    findBild();
    return g_setWindowFrame && slot >= 0 && slot < kWindowSlots && g_setWindowFrame(slot, x, y, set ? 1 : 0) != 0;
}