#include "ui.h"
#include "itemicon.h"
#include "text.h"
#include "automation.h"
#include "lua_host.h"
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
#include <map>
#include <memory>
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
    using WheelFn = int(__cdecl*)(int delta);
    using SetWheelFn = void(__cdecl*)(WheelFn fn);
    using TakeScrollFn = int(__cdecl*)();
    SetWheelFn g_setWheel = nullptr;
    TakeScrollFn g_takeScroll = nullptr;
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
            g_setWheel = reinterpret_cast<SetWheelFn>(GetProcAddress(bild, "SacredBild_UiSetWheelHandler"));
            g_takeScroll = reinterpret_cast<TakeScrollFn>(GetProcAddress(bild, "SacredBild_UiTakeScroll"));
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

    // ---- Texture images: a texture of PAK\Texture.pak by its name (the game's own ability and skill icons are such textures).
    //      PAK\Texture.TMP lists them (0x118 bytes of header, then 0x50-byte records: name char[0x20], u16 width and height at
    //      +0x20 / +0x22); the record's index is the texture manager's handle, so the engine loads and decodes the texture. ----
    constexpr uint32_t kTextureImageBase = 0x20000000, kTextureImageMax = 0x10000;
    struct TextureEntry
    {
        uint16_t width = 0, height = 0;
    };
    std::map<std::string, uint32_t> g_textureIndex;     // upper case name -> handle
    std::vector<TextureEntry> g_textureEntries;
    std::once_flag g_textureOnce;

    void loadTextureIndex()
    {
        std::call_once(g_textureOnce, [] {
            wchar_t path[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, path, MAX_PATH);
            std::wstring p = path;
            const size_t slash = p.find_last_of(L"\\/");
            p = (slash == std::wstring::npos ? L"" : p.substr(0, slash + 1)) + L"PAK\\Texture.TMP";
            HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                LOG("UI: PAK\\Texture.TMP not found: no texture images");
                return;
            }
            const DWORD size = GetFileSize(h, nullptr);
            std::vector<uint8_t> data(size);
            DWORD got = 0;
            ReadFile(h, data.data(), size, &got, nullptr);
            CloseHandle(h);
            const uint32_t count = got > 0x118 ? (got - 0x118) / 0x50 : 0;
            g_textureEntries.resize(count);
            for (uint32_t i = 0; i < count && i < kTextureImageMax; ++i)
            {
                const uint8_t* r = data.data() + 0x118 + i * 0x50;
                char name[0x21] = {};
                std::memcpy(name, r, 0x20);
                if (!name[0])
                {
                    continue;
                }
                for (char& c : name)
                {
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                }
                g_textureIndex.emplace(name, i);
                std::memcpy(&g_textureEntries[i].width, r + 0x20, 2);
                std::memcpy(&g_textureEntries[i].height, r + 0x22, 2);
            }
            LOG("UI: {} textures in Texture.TMP", g_textureIndex.size());
        });
    }

    struct ImageRecord
    {
        uint32_t texture;
        float u0, v0, u1, v1;
        int width, height;
    };

    bool imageRecord(uint32_t id, ImageRecord& out)
    {
        if (id >= kTextureImageBase && id < kTextureImageBase + kTextureImageMax)
        {
            const uint32_t handle = id - kTextureImageBase;
            if (handle >= g_textureEntries.size() || !g_textureEntries[handle].width)
            {
                return false;
            }
            out = {handle, 0.0f, 0.0f, 1.0f, 1.0f, g_textureEntries[handle].width, g_textureEntries[handle].height};
            return true;
        }
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

    // ---- Pictures (Ui::pictureCreate): Gear's own images, a texture each, made on the render thread ----

    struct Picture
    {
        int width = 0, height = 0;
        std::vector<uint16_t> pixels;       // ARGB 4444, until the texture is made
        std::vector<uint32_t> pixels32;     // or ARGB 8888 (pictureCreate32)
        bool premultiplied = false;         // pixels32: the colors are scaled by alpha
        IDirectDrawSurface7* surface = nullptr;
        int texWidth = 0, texHeight = 0;
        bool failed = false;
    };
    std::mutex g_pictureLock;
    std::map<uint32_t, Picture> g_pictures;
    std::vector<IDirectDrawSurface7*> g_deadSurfaces;      // released by the next draw (the render thread)
    uint32_t g_nextPicture = Ui::kPictureBase;

    int powerOfTwo(int n)
    {
        int p = 1;
        while (p < n)
        {
            p <<= 1;
        }
        return p;
    }

    // The texture, made from the pixels (g_pictureLock held): 16-bit ARGB 4444 if the device has it, else 32-bit.
    bool makePictureTexture(IDirect3DDevice7* device, Picture& pic)
    {
        IDirect3D7* d3d = nullptr;
        if (FAILED(device->GetDirect3D(&d3d)) || !d3d)
        {
            return false;
        }
        IDirectDraw7* ddraw = nullptr;
        d3d->QueryInterface(IID_IDirectDraw7, reinterpret_cast<void**>(&ddraw));
        d3d->Release();
        if (!ddraw)
        {
            return false;
        }
        const int tw = powerOfTwo(pic.width), th = powerOfTwo(pic.height);
        bool wide = false;
        HRESULT hr = E_FAIL;
        for (const bool w : {false, true})
        {
            if (!w && !pic.pixels32.empty())
            {
                continue;       // 32-bit pixels need the 32-bit format
            }
            DDSURFACEDESC2 desc = {};
            desc.dwSize = sizeof(desc);
            desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
            desc.dwWidth = tw;
            desc.dwHeight = th;
            desc.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
            desc.ddsCaps.dwCaps2 = DDSCAPS2_TEXTUREMANAGE;
            desc.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
            desc.ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_ALPHAPIXELS;
            desc.ddpfPixelFormat.dwRGBBitCount = w ? 32 : 16;
            desc.ddpfPixelFormat.dwRBitMask = w ? 0xFF0000 : 0xF00;
            desc.ddpfPixelFormat.dwGBitMask = w ? 0xFF00 : 0xF0;
            desc.ddpfPixelFormat.dwBBitMask = w ? 0xFF : 0xF;
            desc.ddpfPixelFormat.dwRGBAlphaBitMask = w ? 0xFF000000 : 0xF000;
            hr = ddraw->CreateSurface(&desc, &pic.surface, nullptr);
            if (SUCCEEDED(hr))
            {
                wide = w;
                break;
            }
            pic.surface = nullptr;
        }
        ddraw->Release();
        if (FAILED(hr))
        {
            LOG("UI: no texture for a picture ({:08x})", static_cast<uint32_t>(hr));
            return false;
        }
        DDSURFACEDESC2 desc = {};
        desc.dwSize = sizeof(desc);
        if (FAILED(pic.surface->Lock(nullptr, &desc, DDLOCK_WAIT | DDLOCK_WRITEONLY, nullptr)))
        {
            pic.surface->Release();
            pic.surface = nullptr;
            return false;
        }
        for (int y = 0; y < th; ++y)
        {
            auto* row = static_cast<uint8_t*>(desc.lpSurface) + size_t(y) * desc.lPitch;
            // Beyond the picture: its last row and column repeated, so filtering at its edge takes nothing else.
            if (!pic.pixels32.empty())
            {
                const uint32_t* src32 = pic.pixels32.data() + size_t(std::min(y, pic.height - 1)) * pic.width;
                for (int x = 0; x < tw; ++x)
                {
                    reinterpret_cast<uint32_t*>(row)[x] = src32[std::min(x, pic.width - 1)];
                }
                continue;
            }
            const uint16_t* src = pic.pixels.data() + size_t(std::min(y, pic.height - 1)) * pic.width;
            for (int x = 0; x < tw; ++x)
            {
                const uint16_t p = src[std::min(x, pic.width - 1)];
                if (wide)
                {
                    const uint32_t a = (p >> 12) & 0xF, r = (p >> 8) & 0xF, g = (p >> 4) & 0xF, b = p & 0xF;
                    reinterpret_cast<uint32_t*>(row)[x] = (a * 17) << 24 | (r * 17) << 16 | (g * 17) << 8 | (b * 17);
                }
                else
                {
                    reinterpret_cast<uint16_t*>(row)[x] = p;
                }
            }
        }
        pic.surface->Unlock(nullptr);
        pic.texWidth = tw;
        pic.texHeight = th;
        pic.pixels.clear();
        pic.pixels.shrink_to_fit();
        pic.pixels32.clear();
        pic.pixels32.shrink_to_fit();
        return true;
    }

    // The picture's record (as an image's) and texture; render thread.
    bool pictureTexture(IDirect3DDevice7* device, uint32_t id, ImageRecord& r, IDirectDrawSurface7*& surface,
                        bool* premultiplied = nullptr)
    {
        std::scoped_lock lock(g_pictureLock);
        auto it = g_pictures.find(id);
        if (it == g_pictures.end() || it->second.failed)
        {
            return false;
        }
        Picture& pic = it->second;
        if (!pic.surface && !makePictureTexture(device, pic))
        {
            pic.failed = true;
            return false;
        }
        r.texture = 0;
        r.width = pic.width;
        r.height = pic.height;
        r.u0 = 0;
        r.v0 = 0;
        r.u1 = static_cast<float>(pic.width) / pic.texWidth;
        r.v1 = static_cast<float>(pic.height) / pic.texHeight;
        surface = pic.surface;
        if (premultiplied)
        {
            *premultiplied = pic.premultiplied;
        }
        return true;
    }

    // ---- Meshes (Ui::meshCreate): geometry of the RmlUi host, kept for the frames that draw it ----

    struct MeshData
    {
        std::vector<Ui::MeshVertex> vertices;
        std::vector<int> indices;
    };
    std::mutex g_meshLock;
    std::map<uint32_t, std::shared_ptr<const MeshData>> g_meshes;
    uint32_t g_nextMesh = 1;

    void releaseDeadPictures()
    {
        std::vector<IDirectDrawSurface7*> dead;
        {
            std::scoped_lock lock(g_pictureLock);
            dead.swap(g_deadSurfaces);
        }
        for (IDirectDrawSurface7* s : dead)
        {
            s->Release();
        }
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

    thread_local const float* t_clip = nullptr;    // the clip of the command being drawn (layout units)

    void quad(IDirect3DDevice7* device, float x0, float y0, float x1, float y1, uint32_t color,
              float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1)
    {
        if (t_clip && !Text::clipQuad(t_clip, x0, y0, x1, y1, u0, v0, u1, v1))
        {
            return;
        }
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
        IDirectDrawSurface7* surface = nullptr;
        if (!(c.color >> 24))
        {
            return;
        }
        if (c.image >= Ui::kPictureBase)
        {
            if (!pictureTexture(device, c.image, r, surface))
            {
                return;
            }
        }
        else if (!imageRecord(c.image, r) || !(surface = imageSurface(r.texture)))
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
        // A crop draws only that part of the image, where it would be in the box (a gauge: the lower part of a ring).
        if (c.crop[0] > 0 || c.crop[1] > 0 || c.crop[2] < 1 || c.crop[3] < 1)
        {
            const float l = std::clamp(c.crop[0], 0.0f, 1.0f), t = std::clamp(c.crop[1], 0.0f, 1.0f);
            const float rr = std::clamp(c.crop[2], l, 1.0f), b = std::clamp(c.crop[3], t, 1.0f);
            const float du = u1 - u0, dv = v1 - v0;
            x += w * l;
            y += h * t;
            w *= rr - l;
            h *= b - t;
            u1 = u0 + du * rr;
            u0 = u0 + du * l;
            v1 = v0 + dv * b;
            v0 = v0 + dv * t;
            if (w <= 0 || h <= 0)
            {
                return;
            }
        }
        if (c.flip & 1)
        {
            std::swap(u0, u1);
        }
        if (c.flip & 2)
        {
            std::swap(v0, v1);
        }
        state.enter();
        state.texture(surface);
        const Text::Placement p = placement();
        quad(device, Text::snapX(p, x), Text::snapY(p, y), Text::snapX(p, x + w), Text::snapY(p, y + h), c.color, u0, v0,
             u1, v1);
    }


    // Clips the convex polygon `poly` (n vertices) to the rectangle (x, y, w, h), texture coordinates and color
    // interpolated; returns the new count (at most n + 4).
    struct MeshPoint
    {
        float x, y, u, v;
        float a, r, g, b;
    };

    // The part of a polygon inside the texture cell [cu, cu + 1] x [cv, cv + 1] (Sutherland-Hodgman in u and v): a mesh whose
    // texture coordinates run past 1 (a repeated tile) is drawn cell by cell, because the UI images live in an atlas and
    // cannot wrap.
    size_t clipUvCell(const MeshPoint* in, size_t n, float cu, float cv, MeshPoint* out)
    {
        MeshPoint bufA[16], bufB[16];
        std::copy(in, in + n, bufA);
        MeshPoint* src = bufA;
        MeshPoint* dst = bufB;
        for (int edge = 0; edge < 4 && n > 0; ++edge)
        {
            const auto value = [&](const MeshPoint& p) {
                switch (edge)
                {
                case 0: return p.u - cu;
                case 1: return cu + 1.0f - p.u;
                case 2: return p.v - cv;
                default: return cv + 1.0f - p.v;
                }
            };
            size_t m = 0;
            for (size_t i = 0; i < n; ++i)
            {
                const MeshPoint& p = src[i];
                const MeshPoint& q = src[(i + 1) % n];
                const float vp = value(p), vq = value(q);
                if (vp >= 0)
                {
                    dst[m++] = p;
                }
                if ((vp >= 0) != (vq >= 0))
                {
                    const float k = vp / (vp - vq);
                    MeshPoint o;
                    o.x = p.x + (q.x - p.x) * k;
                    o.y = p.y + (q.y - p.y) * k;
                    o.u = p.u + (q.u - p.u) * k;
                    o.v = p.v + (q.v - p.v) * k;
                    o.a = p.a + (q.a - p.a) * k;
                    o.r = p.r + (q.r - p.r) * k;
                    o.g = p.g + (q.g - p.g) * k;
                    o.b = p.b + (q.b - p.b) * k;
                    dst[m++] = o;
                }
            }
            std::swap(src, dst);
            n = m;
        }
        std::copy(src, src + n, out);
        return n;
    }

    size_t clipPolygon(const MeshPoint* in, size_t n, const float* rect, MeshPoint* out)
    {
        MeshPoint bufA[16], bufB[16];
        std::copy(in, in + n, bufA);
        MeshPoint* src = bufA;
        MeshPoint* dst = bufB;
        const float l = rect[0], t = rect[1], r = rect[0] + rect[2], b = rect[1] + rect[3];
        for (int edge = 0; edge < 4 && n > 0; ++edge)
        {
            const auto inside = [&](const MeshPoint& p) {
                switch (edge)
                {
                case 0: return p.x >= l;
                case 1: return p.x <= r;
                case 2: return p.y >= t;
                default: return p.y <= b;
                }
            };
            const auto cut = [&](const MeshPoint& p, const MeshPoint& q) {
                float k;
                switch (edge)
                {
                case 0: k = (l - p.x) / (q.x - p.x); break;
                case 1: k = (r - p.x) / (q.x - p.x); break;
                case 2: k = (t - p.y) / (q.y - p.y); break;
                default: k = (b - p.y) / (q.y - p.y); break;
                }
                MeshPoint o;
                o.x = p.x + (q.x - p.x) * k;
                o.y = p.y + (q.y - p.y) * k;
                o.u = p.u + (q.u - p.u) * k;
                o.v = p.v + (q.v - p.v) * k;
                o.a = p.a + (q.a - p.a) * k;
                o.r = p.r + (q.r - p.r) * k;
                o.g = p.g + (q.g - p.g) * k;
                o.b = p.b + (q.b - p.b) * k;
                return o;
            };
            size_t m = 0;
            for (size_t i = 0; i < n; ++i)
            {
                const MeshPoint& cur = src[i];
                const MeshPoint& prev = src[(i + n - 1) % n];
                const bool ci = inside(cur), pi = inside(prev);
                if (ci)
                {
                    if (!pi)
                    {
                        dst[m++] = cut(prev, cur);
                    }
                    dst[m++] = cur;
                }
                else if (pi)
                {
                    dst[m++] = cut(prev, cur);
                }
            }
            std::swap(src, dst);
            n = m;
        }
        std::copy(src, src + n, out);
        return n;
    }

    uint32_t packColor(float a, float r, float g, float b)
    {
        const auto c = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f); };
        return c(a) << 24 | c(r) << 16 | c(g) << 8 | c(b);
    }

    void drawMesh(IDirect3DDevice7* device, StateGuard& state, const Command& c)
    {
        std::shared_ptr<const MeshData> mesh;
        {
            std::scoped_lock lock(g_meshLock);
            const auto it = g_meshes.find(c.region);
            if (it == g_meshes.end())
            {
                return;
            }
            mesh = it->second;
        }
        ImageRecord r{};
        IDirectDrawSurface7* surface = nullptr;
        bool premultiplied = true;      // the vertex colors: scaled by alpha (a game image's pixels are not)
        if (c.image >= Ui::kPictureBase)
        {
            if (!pictureTexture(device, c.image, r, surface, &premultiplied))
            {
                return;
            }
        }
        else if (c.image)
        {
            if (!imageRecord(c.image, r) || !(surface = imageSurface(r.texture)))
            {
                return;
            }
            premultiplied = false;
        }
        const float inv = c.w > 0 ? 1.0f / c.w : 1.0f;
        const float tu0 = r.u0, tv0 = r.v0, tdu = r.u1 - r.u0, tdv = r.v1 - r.v0;
        const bool flipU = (c.flip & 1) != 0, flipV = (c.flip & 2) != 0;
        const std::vector<Ui::MeshVertex>& vs = mesh->vertices;

        const auto point = [&](int index) {
            const Ui::MeshVertex& v = vs[static_cast<size_t>(index)];
            MeshPoint p;
            float px = v.x + c.x, py = v.y + c.y;
            if (c.matrix)
            {
                const auto& m = *c.matrix;
                const float tx = m[0] * px + m[4] * py + m[12];
                const float ty = m[1] * px + m[5] * py + m[13];
                const float tw = m[3] * px + m[7] * py + m[15];
                const float k = tw != 0.0f ? 1.0f / tw : 1.0f;
                px = tx * k;
                py = ty * k;
            }
            p.x = px * inv;
            p.y = py * inv;
            const float uu = flipU ? 1.0f - v.u : v.u, vv = flipV ? 1.0f - v.v : v.v;
            p.u = uu;       // mapped into the atlas rectangle when the vertex is emitted (a repeated tile runs past 1)
            p.v = vv;
            p.a = static_cast<float>(v.color >> 24);
            p.r = static_cast<float>((v.color >> 16) & 0xFF);
            p.g = static_cast<float>((v.color >> 8) & 0xFF);
            p.b = static_cast<float>(v.color & 0xFF);
            return p;
        };
        const auto toVertex = [&](const MeshPoint& p, float cellU = 0.0f, float cellV = 0.0f) {
            float rr = p.r, gg = p.g, bb = p.b;
            if (!premultiplied && p.a > 0.0f)
            {
                // Straight-alpha blending: the color without the alpha scaled in.
                const float k = 255.0f / p.a;
                rr = std::min(255.0f, rr * k);
                gg = std::min(255.0f, gg * k);
                bb = std::min(255.0f, bb * k);
            }
            return TLVertex{p.x, p.y, 0.0f, 1.0f, packColor(p.a, rr, gg, bb), 0,
                            surface ? tu0 + (p.u - cellU) * tdu : 0.0f, surface ? tv0 + (p.v - cellV) * tdv : 0.0f};
        };

        thread_local std::vector<TLVertex> out;
        out.clear();
        const size_t triangles = mesh->indices.size() / 3;
        for (size_t t = 0; t < triangles; ++t)
        {
            const int i0 = mesh->indices[t * 3], i1 = mesh->indices[t * 3 + 1], i2 = mesh->indices[t * 3 + 2];
            const int count = static_cast<int>(vs.size());
            if (i0 < 0 || i1 < 0 || i2 < 0 || i0 >= count || i1 >= count || i2 >= count)
            {
                continue;
            }
            const MeshPoint tri[3] = {point(i0), point(i1), point(i2)};
            const float umin = std::min({tri[0].u, tri[1].u, tri[2].u}), umax = std::max({tri[0].u, tri[1].u, tri[2].u});
            const float vmin = std::min({tri[0].v, tri[1].v, tri[2].v}), vmax = std::max({tri[0].v, tri[1].v, tri[2].v});
            if (surface && !(c.image >= Ui::kPictureBase) && (umin < -0.001f || umax > 1.001f || vmin < -0.001f || vmax > 1.001f))
            {
                // A repeated tile: one piece per texture cell, each mapped into the image's rectangle.
                const int u0 = static_cast<int>(std::floor(umin + 0.0005f)), u1 = static_cast<int>(std::ceil(umax - 0.0005f));
                const int v0 = static_cast<int>(std::floor(vmin + 0.0005f)), v1 = static_cast<int>(std::ceil(vmax - 0.0005f));
                if ((u1 - u0) * (v1 - v0) > 256)
                {
                    continue;
                }
                for (int iv = v0; iv < std::max(v1, v0 + 1); ++iv)
                {
                    for (int iu = u0; iu < std::max(u1, u0 + 1); ++iu)
                    {
                        MeshPoint cell[16];
                        const size_t cn = clipUvCell(tri, 3, static_cast<float>(iu), static_cast<float>(iv), cell);
                        if (cn < 3)
                        {
                            continue;
                        }
                        MeshPoint poly[16];
                        size_t n = cn;
                        std::copy(cell, cell + cn, poly);
                        if (c.clipped)
                        {
                            MeshPoint cut[16];
                            n = clipPolygon(poly, cn, c.clip, cut);
                            std::copy(cut, cut + n, poly);
                        }
                        for (size_t k = 1; k + 1 < n; ++k)
                        {
                            out.push_back(toVertex(poly[0], static_cast<float>(iu), static_cast<float>(iv)));
                            out.push_back(toVertex(poly[k], static_cast<float>(iu), static_cast<float>(iv)));
                            out.push_back(toVertex(poly[k + 1], static_cast<float>(iu), static_cast<float>(iv)));
                        }
                    }
                }
                continue;
            }
            if (c.clipped)
            {
                MeshPoint poly[8];
                const size_t n = clipPolygon(tri, 3, c.clip, poly);
                for (size_t k = 1; k + 1 < n; ++k)
                {
                    out.push_back(toVertex(poly[0]));
                    out.push_back(toVertex(poly[k]));
                    out.push_back(toVertex(poly[k + 1]));
                }
            }
            else
            {
                out.push_back(toVertex(tri[0]));
                out.push_back(toVertex(tri[1]));
                out.push_back(toVertex(tri[2]));
            }
        }
        if (out.empty())
        {
            return;
        }
        state.enter();
        state.texture(surface);
        device->SetRenderState(D3DRENDERSTATE_SRCBLEND, premultiplied ? D3DBLEND_ONE : D3DBLEND_SRCALPHA);
        constexpr size_t kChunk = 60000;        // vertices per call (a multiple of 3, below the device's limit)
        for (size_t first = 0; first < out.size(); first += kChunk)
        {
            const size_t n = std::min(kChunk, out.size() - first);
            device->DrawPrimitive(D3DPT_TRIANGLELIST, kFvf, out.data() + first, static_cast<DWORD>(n), 0);
        }
        device->SetRenderState(D3DRENDERSTATE_SRCBLEND, D3DBLEND_SRCALPHA);
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
        t_clip = c.clipped ? c.clip : nullptr;
        Text::setClip(t_clip);
        if (t_clip && (c.kind == Kind::ItemIcon || (c.kind == Kind::Text && !c.vector)))
        {
            // Drawn by the game: only when the whole box lies inside (the game's text: its rect, measured if unset).
            float w = c.w, h = c.h;
            if (c.kind == Kind::Text && (w <= 0 || h <= 0))
            {
                int tw = 0, th = 0;
                void* font = fontById(c.font);
                if (font && measure(font, c.text.c_str(), tw, th))
                {
                    w = w > 0 ? w : static_cast<float>(tw);
                    h = h > 0 ? h : static_cast<float>(th);
                }
            }
            if (c.x < t_clip[0] || c.y < t_clip[1] || c.x + w > t_clip[0] + t_clip[2] || c.y + h > t_clip[1] + t_clip[3])
            {
                return true;
            }
        }
        __try
        {
            switch (c.kind)
            {
            case Kind::Rect: drawRect(device, state, c); break;
            case Kind::Image: drawImage(device, state, c); break;
            case Kind::Mesh: drawMesh(device, state, c); break;
            case Kind::Text: c.vector ? drawVectorText(device, state, c) : drawText(device, state, c); break;
            case Kind::Region: break;
            case Kind::ItemIcon:
                state.restore();     // the model renderer sets what it needs through the game's own state cache
                ItemIcon::draw(device, {c.region, c.image, c.x, c.y, c.w, c.h});
                break;
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
        if (layer == Layer::Hud)
        {
            const uintptr_t m = manager();
            if (m && (managerFlags(m) & (kManagerMenus | kManagerCinematic)) == kManagerMenus)
            {
                LuaHost::menuFrame();
            }
        }
        if (!device)
        {
            return;
        }
        releaseDeadPictures();
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
        ItemIcon::endPass();
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

    // ---- The mouse wheel ----

    std::atomic<int> g_wheel{0};        // over mod UI, since the last takeWheel
    HHOOK g_wheelHook = nullptr;        // Gear's own, when SacredBild does not hand the wheel over
    std::once_flag g_wheelOnce;

    // The wheel turned by `delta`: Gear's when the cursor is over a mod's region (the game then does not get it).
    int __cdecl wheelHandler(int delta)
    {
        const uint8_t* mouse = Gear::Addr::cMouse_instance ? Gear::Addr::cMouse_instance() : nullptr;
        if (!mouse || !delta)
        {
            return 0;
        }
        const int x = readOr<int32_t>(reinterpret_cast<uintptr_t>(mouse) + 4, 0);
        const int y = readOr<int32_t>(reinterpret_cast<uintptr_t>(mouse) + 8, 0);
        if (!cursorOverModUi(x, y))
        {
            return 0;
        }
        g_wheel += delta;
        return 1;
    }

    // Without SacredBild: WM_MOUSEWHEEL as the game's thread takes it from its queue (a taken one becomes WM_NULL).
    LRESULT CALLBACK wheelHook(int code, WPARAM wParam, LPARAM lParam)
    {
        auto* msg = reinterpret_cast<MSG*>(lParam);
        if (code == HC_ACTION && wParam == PM_REMOVE && msg && msg->message == WM_MOUSEWHEEL &&
            wheelHandler(GET_WHEEL_DELTA_WPARAM(msg->wParam)))
        {
            msg->message = WM_NULL;
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    BOOL CALLBACK findGameWindow(HWND window, LPARAM out)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        wchar_t cls[16] = {};
        if (pid == GetCurrentProcessId() && GetClassNameW(window, cls, 16) && wcscmp(cls, L"Sacred") == 0)
        {
            *reinterpret_cast<HWND*>(out) = window;
            return FALSE;
        }
        return TRUE;
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

namespace
{
    // The clip the Lua thread records commands with (Ui::setClip).
    bool g_clipOn = false;
    float g_clipRect[4] = {};
}

void Ui::setClip(bool on, float x, float y, float w, float h)
{
    std::scoped_lock lock(g_lock);
    g_clipOn = on;
    g_clipRect[0] = x;
    g_clipRect[1] = y;
    g_clipRect[2] = std::max(0.0f, w);
    g_clipRect[3] = std::max(0.0f, h);
}

int Ui::takeWheel()
{
    findBild();
    if (!g_setWheel && !g_wheelHook)
    {
        // Without SacredBild: a message hook on the game window's thread, once the window exists.
        HWND window = nullptr;
        EnumWindows(&findGameWindow, reinterpret_cast<LPARAM>(&window));
        if (window)
        {
            std::call_once(g_wheelOnce, [window] {
                HMODULE self = nullptr;
                GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(&wheelHook), &self);
                g_wheelHook = SetWindowsHookExW(WH_GETMESSAGE, &wheelHook, self, GetWindowThreadProcessId(window, nullptr));
                LOG("UI: the mouse wheel over mod UI through Gear's message hook: {}", g_wheelHook ? "on" : "failed");
            });
        }
    }
    return g_wheel.exchange(0);
}

int Ui::takePadScroll()
{
    findBild();
    return g_takeScroll ? g_takeScroll() : 0;
}

bool Ui::add(Command&& command)
{
    if (command.layer >= Layer::Count)
    {
        return false;
    }
    std::scoped_lock lock(g_lock);
    if (g_clipOn && command.window < 0)
    {
        if (command.kind == Kind::Region)
        {
            const float x0 = std::max(command.x, g_clipRect[0]), y0 = std::max(command.y, g_clipRect[1]);
            const float x1 = std::min(command.x + command.w, g_clipRect[0] + g_clipRect[2]);
            const float y1 = std::min(command.y + command.h, g_clipRect[1] + g_clipRect[3]);
            if (x1 <= x0 || y1 <= y0)
            {
                return true;    // hidden: it takes no mouse
            }
            command.x = x0;
            command.y = y0;
            command.w = x1 - x0;
            command.h = y1 - y0;
        }
        else if (command.kind == Kind::Mesh && command.clipped)
        {
            // A mesh with a clip of its own (the RmlUi host's scissor): the intersection with the scroll area's.
            const float x0 = std::max(command.clip[0], g_clipRect[0]), y0 = std::max(command.clip[1], g_clipRect[1]);
            const float x1 = std::min(command.clip[0] + command.clip[2], g_clipRect[0] + g_clipRect[2]);
            const float y1 = std::min(command.clip[1] + command.clip[3], g_clipRect[1] + g_clipRect[3]);
            command.clip[0] = x0;
            command.clip[1] = y0;
            command.clip[2] = std::max(0.0f, x1 - x0);
            command.clip[3] = std::max(0.0f, y1 - y0);
        }
        else
        {
            command.clipped = true;
            std::copy(std::begin(g_clipRect), std::end(g_clipRect), std::begin(command.clip));
        }
    }
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
    if (g_setWheel)
    {
        g_setWheel(&wheelHandler);
        LOG("UI: the mouse wheel over mod UI comes from SacredBild");
    }
    return ok;
}

uint32_t Ui::textureImage(const std::string& name)
{
    loadTextureIndex();
    std::string key = name;
    for (char& c : key)
    {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    const auto it = g_textureIndex.find(key);
    return it == g_textureIndex.end() ? 0 : kTextureImageBase + it->second;
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

namespace
{
    bool callShow(uintptr_t window, bool shown)
    {
        __try
        {
            // The visible bit (1) of the control's flags, set and cleared as the engine does (00732550 / 007325C0), which
            // also tell the window (vtable +0x24).
            if (!Gear::Addr::cUI_Popup_show || !Gear::Addr::cUI_Control_clearFlags)
            {
                return false;
            }
            if (shown)
            {
                Gear::Addr::cUI_Popup_show(reinterpret_cast<void*>(window), 1);
            }
            else
            {
                Gear::Addr::cUI_Control_clearFlags(reinterpret_cast<void*>(window), 1);
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}

bool Ui::setWindowShown(const std::string& name, bool shown)
{
    WindowInfo info;
    const uintptr_t m = manager();
    if (!window(name, info))
    {
        return false;
    }
    const uintptr_t w = windowAt(m, info.slot);
    return w && callShow(w, shown);
}

namespace
{
    std::atomic<uintptr_t> g_engine{0};

    // cWorldView_drawOverheadLabels (ENG 00627180) sets bit 0x4000 of the world view's flags (+0x10) each frame
    // from GetAsyncKeyState(VK_MENU) and draws the item and object labels while it is set: the game's "show text".
    // The view is views[viewIndex] of the engine (+0x08, 8 pointers; index uint16 at +0x48).
    bool showText()
    {
        const uintptr_t engine = g_engine.load(std::memory_order_relaxed);
        if (!engine)
        {
            return false;
        }
        const uint16_t index = readOr<uint16_t>(engine + 0x48, 0xFFFF);
        const uintptr_t view = index < 8 ? readOr<uintptr_t>(engine + 0x08 + 4 * index, 0) : 0;
        return view && (readOr<uint32_t>(view + 0x10, 0) & 0x4000) != 0;
    }
}

void Ui::setEngine(void* engine)
{
    g_engine.store(reinterpret_cast<uintptr_t>(engine), std::memory_order_relaxed);
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
    s.showText = std::strcmp(s.mode, "game") == 0 && showText();
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

uint32_t Ui::pictureCreate(int width, int height, std::vector<uint16_t> pixels)
{
    if (width <= 0 || height <= 0 || width > 2048 || height > 2048 || pixels.size() < size_t(width) * height)
    {
        return 0;
    }
    std::scoped_lock lock(g_pictureLock);
    const uint32_t id = g_nextPicture++;
    Picture& pic = g_pictures[id];
    pic.width = width;
    pic.height = height;
    pic.pixels = std::move(pixels);
    return id;
}

void Ui::pictureRelease(uint32_t id)
{
    std::scoped_lock lock(g_pictureLock);
    auto it = g_pictures.find(id);
    if (it == g_pictures.end())
    {
        return;
    }
    if (it->second.surface)
    {
        g_deadSurfaces.push_back(it->second.surface);
    }
    g_pictures.erase(it);
}

uint32_t Ui::pictureCreate32(int width, int height, std::vector<uint32_t> pixels, bool premultiplied)
{
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096 || pixels.size() < size_t(width) * height)
    {
        return 0;
    }
    std::scoped_lock lock(g_pictureLock);
    const uint32_t id = g_nextPicture++;
    Picture& pic = g_pictures[id];
    pic.width = width;
    pic.height = height;
    pic.pixels32 = std::move(pixels);
    pic.premultiplied = premultiplied;
    return id;
}

uint32_t Ui::meshCreate(std::vector<MeshVertex> vertices, std::vector<int> indices)
{
    if (vertices.empty() || indices.size() < 3 || vertices.size() > 1000000 || indices.size() > 3000000)
    {
        return 0;
    }
    auto mesh = std::make_shared<MeshData>();
    mesh->vertices = std::move(vertices);
    mesh->indices = std::move(indices);
    std::scoped_lock lock(g_meshLock);
    const uint32_t id = g_nextMesh++;
    g_meshes.emplace(id, std::move(mesh));
    return id;
}

void Ui::meshRelease(uint32_t id)
{
    std::scoped_lock lock(g_meshLock);
    g_meshes.erase(id);
}

float Ui::canvasScale()
{
    return placement().scale;
}
