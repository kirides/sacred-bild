// Sacred Gear UI sandbox (docs/Sandbox.md): Gear's own Lua host, kernel, RmlUi host and UI replay, run outside the game. The game's UI
// images come straight from the install's files (game_data.h), the drawing goes through SacredBild's DirectDraw / Direct3D 7 emulation on
// Direct3D 9Ex, the same path the game's UI takes with SacredBild. A window shows it, or nothing does (--hidden): frames are rendered
// off-screen and can be saved as PNG. A driver script (--script) steps frames, moves the mouse, clicks and takes snapshots, and gets the
// RmlUi natives and command capture for differential tests (tools/sandbox/fuzz).
#include "game_data.h"

#include "automation.h"
#include "clock.h"
#include "log.h"
#include "lua_host.h"
#include "ui/rml.h"
#include "ui/ui.h"

#include "ddraw9/backend.h"

#include <windows.h>
#include <ddraw.h>
#include <d3d.h>
#include <lua.hpp>

#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    // ---- Options ----

    struct Options
    {
        std::wstring gameDir = L"B:\\Spiele\\GOG Games\\sacred gold GOG";
        std::wstring modsDir;               // default: <sandbox>\mods
        std::wstring script;                // driver script
        std::vector<std::string> scriptArgs;
        std::wstring snapshot;              // without a script: save the last frame here
        std::wstring background;            // an image file drawn under the UI (a game screenshot)
        std::wstring log;                   // default: SacredGearSandbox.log next to the exe
        std::string enable;                 // mods switched on before the start: "all" or "a,b,c"
        int width = 1024, height = 768;
        int frames = 0;                     // without a script: stop after this many frames (0: run until the window closes)
        bool hidden = false;
        bool mods = true;                   // run the kernel and the mods
        bool simulated = false;             // simulated clock (fixed dt per frame); scripts always have it
        // Frame time of the simulated clock: 1/64 s, a power of two, so that every simulated time and every difference of two is exact in a float
        // too. RmlUi counts the caret's blink (0.5 s) and other timers down in floats; with 1/60 the rounding decided whether a blink fell in one
        // frame or the next, depending on how often the document had been updated before. With 1/64 a 0.5 s timer ends exactly on frame 32.
        double dt = 1.0 / 64.0;
        std::string mode = "game";          // gear.ui.state().mode
        uint32_t clear = 0xFF202830;
    };
    Options g_opt;

    // ---- Clock ----

    double g_simTime = 0.0;
    double simClock() { return g_simTime; }

    // ---- Window and input ----

    HWND g_window = nullptr;
    bool g_quit = false;
    int g_mouseX = -1000, g_mouseY = -1000;

    LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_CLOSE:
            g_quit = true;
            return 0;
        case WM_MOUSEMOVE:
            g_mouseX = static_cast<short>(LOWORD(lParam));
            g_mouseY = static_cast<short>(HIWORD(lParam));
            return 0;
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        {
            const int x = static_cast<short>(LOWORD(lParam)), y = static_cast<short>(HIWORD(lParam));
            const bool down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN;
            const int button = msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP ? 0 : 1;
            if (down)
            {
                SetCapture(hwnd);
            }
            else
            {
                ReleaseCapture();
            }
            Ui::hostMouse(down, button, x, y);
            return 0;
        }
        case WM_MOUSEWHEEL:
        {
            POINT p{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
            ScreenToClient(hwnd, &p);
            Ui::hostWheel(GET_WHEEL_DELTA_WPARAM(wParam), p.x, p.y);
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }

    bool createWindow()
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = windowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"SacredGearSandbox";
        RegisterClassW(&wc);
        RECT r{0, 0, g_opt.width, g_opt.height};
        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        AdjustWindowRect(&r, style, FALSE);
        g_window = CreateWindowW(wc.lpszClassName, L"Sacred Gear UI sandbox", style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                                 r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
        if (g_window && !g_opt.hidden)
        {
            ShowWindow(g_window, SW_SHOW);
        }
        return g_window != nullptr;
    }

    void pumpMessages()
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    // ---- Direct3D 7 on SacredBild's Direct3D 9Ex backend ----

    IDirectDraw7* g_ddraw = nullptr;
    IDirect3D7* g_d3d = nullptr;
    IDirectDrawSurface7* g_primary = nullptr;
    IDirectDrawSurface7* g_target = nullptr;
    IDirect3DDevice7* g_device = nullptr;
    IDirectDrawSurface7* g_backgroundSurface = nullptr;

    IDirectDrawSurface7* makeTexture(int w, int h, const uint32_t* argb)
    {
        DDSURFACEDESC2 desc{};
        desc.dwSize = sizeof(desc);
        desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        desc.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
        desc.ddsCaps.dwCaps2 = DDSCAPS2_TEXTUREMANAGE;
        desc.dwWidth = static_cast<DWORD>(w);
        desc.dwHeight = static_cast<DWORD>(h);
        desc.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
        desc.ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_ALPHAPIXELS;
        desc.ddpfPixelFormat.dwRGBBitCount = 32;
        desc.ddpfPixelFormat.dwRBitMask = 0x00FF0000;
        desc.ddpfPixelFormat.dwGBitMask = 0x0000FF00;
        desc.ddpfPixelFormat.dwBBitMask = 0x000000FF;
        desc.ddpfPixelFormat.dwRGBAlphaBitMask = 0xFF000000;
        IDirectDrawSurface7* s = nullptr;
        if (FAILED(g_ddraw->CreateSurface(&desc, &s, nullptr)))
        {
            return nullptr;
        }
        DDSURFACEDESC2 lock{};
        lock.dwSize = sizeof(lock);
        if (SUCCEEDED(s->Lock(nullptr, &lock, DDLOCK_WAIT | DDLOCK_WRITEONLY, nullptr)))
        {
            for (int y = 0; y < h; ++y)
            {
                std::memcpy(static_cast<uint8_t*>(lock.lpSurface) + static_cast<size_t>(y) * lock.lPitch, argb + static_cast<size_t>(y) * w,
                            static_cast<size_t>(w) * 4);
            }
            s->Unlock(nullptr);
        }
        return s;
    }

    bool createDevice(std::string& error)
    {
        using CreateEx = HRESULT(WINAPI*)(GUID*, LPVOID*, REFIID, IUnknown*);
        CreateEx create = nullptr;
        for (const DDraw9::Export& e : DDraw9::exports())
        {
            if (std::strcmp(e.name, "DirectDrawCreateEx") == 0)
            {
                create = reinterpret_cast<CreateEx>(e.proc);
            }
        }
        if (!create || FAILED(create(nullptr, reinterpret_cast<LPVOID*>(&g_ddraw), IID_IDirectDraw7, nullptr)) || !g_ddraw)
        {
            error = "Direct3D 9Ex is not available";
            return false;
        }
        g_ddraw->SetCooperativeLevel(g_window, DDSCL_NORMAL | DDSCL_FPUPRESERVE);
        if (FAILED(g_ddraw->QueryInterface(IID_IDirect3D7, reinterpret_cast<void**>(&g_d3d))))
        {
            error = "no IDirect3D7";
            return false;
        }
        DDSURFACEDESC2 desc{};
        desc.dwSize = sizeof(desc);
        desc.dwFlags = DDSD_CAPS;
        desc.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
        if (FAILED(g_ddraw->CreateSurface(&desc, &g_primary, nullptr)))
        {
            error = "cannot create the primary surface";
            return false;
        }
        desc = {};
        desc.dwSize = sizeof(desc);
        desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY;
        desc.dwWidth = static_cast<DWORD>(g_opt.width);
        desc.dwHeight = static_cast<DWORD>(g_opt.height);
        desc.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
        desc.ddpfPixelFormat.dwFlags = DDPF_RGB;
        desc.ddpfPixelFormat.dwRGBBitCount = 32;
        desc.ddpfPixelFormat.dwRBitMask = 0x00FF0000;
        desc.ddpfPixelFormat.dwGBitMask = 0x0000FF00;
        desc.ddpfPixelFormat.dwBBitMask = 0x000000FF;
        if (FAILED(g_ddraw->CreateSurface(&desc, &g_target, nullptr)))
        {
            error = "cannot create the render target";
            return false;
        }
        if (FAILED(g_d3d->CreateDevice(IID_IDirect3DHALDevice, g_target, &g_device)))
        {
            error = "cannot create the Direct3D 7 device";
            return false;
        }
        D3DVIEWPORT7 vp{0, 0, static_cast<DWORD>(g_opt.width), static_cast<DWORD>(g_opt.height), 0.0f, 1.0f};
        g_device->SetViewport(&vp);
        return true;
    }

    // ---- The host's game images (Ui::Host) ----

    constexpr uint32_t kTextureImageBase = 0x20000000;
    std::map<uint32_t, IDirectDrawSurface7*> g_surfaces;    // host texture handle (Texture.TMP index + 1) -> surface

    IDirectDrawSurface7* hostSurface(uint32_t texture)
    {
        auto it = g_surfaces.find(texture);
        if (it != g_surfaces.end())
        {
            return it->second;
        }
        GameData::Pixels px;
        std::string error;
        IDirectDrawSurface7* s = nullptr;
        if (GameData::loadTexture(static_cast<int>(texture) - 1, px, error))
        {
            s = makeTexture(px.width, px.height, px.argb.data());
        }
        else
        {
            LOG("Sandbox: texture {} ({}): {}", texture - 1, GameData::textureName(static_cast<int>(texture) - 1), error);
        }
        g_surfaces.emplace(texture, s);
        return s;
    }

    uint32_t hostImageCount() { return static_cast<uint32_t>(GameData::uiImages().size()); }

    bool hostImage(uint32_t id, Ui::HostImage& out)
    {
        if (id >= kTextureImageBase)
        {
            const int index = static_cast<int>(id - kTextureImageBase);
            int w = 0, h = 0;
            if (!GameData::textureSize(index, w, h))
            {
                return false;
            }
            out.texture = static_cast<uint32_t>(index) + 1;
            out.u0 = out.v0 = 0.0f;
            out.u1 = out.v1 = 1.0f;
            out.width = w;
            out.height = h;
            out.sheet = GameData::textureName(index);
            return true;
        }
        const auto& images = GameData::uiImages();
        if (id == 0 || id >= images.size() || images[id].width <= 0)
        {
            return false;
        }
        const GameData::UiImage& u = images[id];
        const int sheet = GameData::textureIndex(u.sheet);
        int sw = 256, sh = 256;
        if (sheet < 0 || !GameData::textureSize(sheet, sw, sh))
        {
            return false;
        }
        out.texture = static_cast<uint32_t>(sheet) + 1;
        out.u0 = static_cast<float>(u.left) / sw;
        out.v0 = static_cast<float>(u.top) / sh;
        out.u1 = static_cast<float>(u.right + 1) / sw;
        out.v1 = static_cast<float>(u.bottom + 1) / sh;
        out.width = u.width;
        out.height = u.height;
        out.sheet = u.sheet;
        return true;
    }

    uint32_t hostTextureImage(const std::string& name)
    {
        const int index = GameData::textureIndex(name);
        return index < 0 ? 0 : kTextureImageBase + static_cast<uint32_t>(index);
    }

    void hostState(Ui::State& s)
    {
        s.available = true;
        s.mode = g_opt.mode.c_str();
        s.mouseX = g_mouseX;
        s.mouseY = g_mouseY;
    }

    int __cdecl hostScreenBounds(float* bounds)
    {
        bounds[0] = 0.0f;
        bounds[1] = 0.0f;
        bounds[2] = static_cast<float>(std::max(g_opt.width, 1024));
        bounds[3] = static_cast<float>(std::max(g_opt.height, 768));
        return 1;
    }

    int __cdecl hostPlacement(float* out)
    {
        out[0] = 1.0f;     // layout units are canvas pixels: the canvas is the screen, its corner the layout's
        out[1] = 0.0f;
        out[2] = 0.0f;
        return 1;
    }

    const Ui::Host g_host = [] {
        Ui::Host h;
        h.state = hostState;
        h.imageCount = hostImageCount;
        h.image = hostImage;
        h.textureImage = hostTextureImage;
        h.surface = hostSurface;
        h.screenBounds = hostScreenBounds;
        h.placement = hostPlacement;
        return h;
    }();

    // ---- Frames ----

    // Background: an image file (stb_image) as a texture, drawn as one quad.
    bool loadBackground()
    {
        if (g_opt.background.empty())
        {
            return true;
        }
        FILE* f = _wfopen(g_opt.background.c_str(), L"rb");
        if (!f)
        {
            return false;
        }
        std::vector<unsigned char> bytes;
        unsigned char chunk[65536];
        for (size_t n; (n = fread(chunk, 1, sizeof(chunk), f)) > 0;)
        {
            bytes.insert(bytes.end(), chunk, chunk + n);
        }
        fclose(f);
        int w = 0, h = 0, n = 0;
        unsigned char* data = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &n, 4);
        if (!data)
        {
            return false;
        }
        std::vector<uint32_t> argb(static_cast<size_t>(w) * h);
        for (size_t i = 0; i < argb.size(); ++i)
        {
            const unsigned char* p = data + i * 4;
            argb[i] = static_cast<uint32_t>(p[3]) << 24 | static_cast<uint32_t>(p[0]) << 16 | static_cast<uint32_t>(p[1]) << 8 | p[2];
        }
        stbi_image_free(data);
        g_backgroundSurface = makeTexture(w, h, argb.data());
        return g_backgroundSurface != nullptr;
    }

    void drawBackground()
    {
        if (!g_backgroundSurface)
        {
            return;
        }
        struct V
        {
            float x, y, z, rhw;
            DWORD color;
            float u, v;
        };
        const float w = static_cast<float>(g_opt.width), h = static_cast<float>(g_opt.height);
        V v[4] = {{-0.5f, -0.5f, 0, 1, 0xFFFFFFFF, 0, 0}, {w - 0.5f, -0.5f, 0, 1, 0xFFFFFFFF, 1, 0},
                  {-0.5f, h - 0.5f, 0, 1, 0xFFFFFFFF, 0, 1}, {w - 0.5f, h - 0.5f, 0, 1, 0xFFFFFFFF, 1, 1}};
        g_device->SetTexture(0, g_backgroundSurface);
        g_device->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, FALSE);
        g_device->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1, v, 4, 0);
        g_device->SetTexture(0, nullptr);
    }

    void beginCanvas()
    {
        g_device->BeginScene();
        g_device->Clear(0, nullptr, D3DCLEAR_TARGET, g_opt.clear, 1.0f, 0);
        drawBackground();
    }

    void present()
    {
        g_device->EndScene();
        if (!g_opt.hidden)
        {
            RECT client{};
            GetClientRect(g_window, &client);
            POINT origin{0, 0};
            ClientToScreen(g_window, &origin);
            RECT dst{origin.x, origin.y, origin.x + g_opt.width, origin.y + g_opt.height};
            g_primary->Blt(&dst, g_target, nullptr, DDBLT_WAIT, nullptr);
        }
    }

    // Lua tick (the mods record), then the three layers drawn as the game's UI pass would.
    int g_frame = 0;

    // One frame: the clock moves on by dt (simulated), the mods tick, and (unless `draw` is false) the three layers are drawn.
    void frame(bool draw = true)
    {
        pumpMessages();
        if (g_opt.simulated)
        {
            g_simTime += g_opt.dt;
        }
        if (g_opt.mods)
        {
            LuaHost::frame(static_cast<float>(g_opt.dt));
        }
        if (!draw)
        {
            ++g_frame;
            return;
        }
        beginCanvas();
        Ui::hostDraw(Ui::Layer::World, g_device);
        Ui::hostDraw(Ui::Layer::Hud, g_device);
        Ui::hostDraw(Ui::Layer::Top, g_device);
        present();
        ++g_frame;
    }

    // The render target's pixels, 0xAARRGGBB (alpha forced to 0xFF).
    bool readPixels(std::vector<uint32_t>& out)
    {
        DDSURFACEDESC2 lock{};
        lock.dwSize = sizeof(lock);
        if (FAILED(g_target->Lock(nullptr, &lock, DDLOCK_WAIT | DDLOCK_READONLY, nullptr)))
        {
            return false;
        }
        out.resize(static_cast<size_t>(g_opt.width) * g_opt.height);
        for (int y = 0; y < g_opt.height; ++y)
        {
            const auto* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(lock.lpSurface) + static_cast<size_t>(y) * lock.lPitch);
            for (int x = 0; x < g_opt.width; ++x)
            {
                out[static_cast<size_t>(y) * g_opt.width + x] = row[x] | 0xFF000000u;
            }
        }
        g_target->Unlock(nullptr);
        return true;
    }

    bool writePng(const std::wstring& path, const std::vector<uint32_t>& argb, int w, int h)
    {
        std::vector<unsigned char> rgba(argb.size() * 4);
        for (size_t i = 0; i < argb.size(); ++i)
        {
            rgba[i * 4 + 0] = static_cast<unsigned char>(argb[i] >> 16);
            rgba[i * 4 + 1] = static_cast<unsigned char>(argb[i] >> 8);
            rgba[i * 4 + 2] = static_cast<unsigned char>(argb[i]);
            rgba[i * 4 + 3] = static_cast<unsigned char>(argb[i] >> 24);
        }
        std::error_code ec;
        fs::create_directories(fs::path(path).parent_path(), ec);
        int len = 0;
        unsigned char* png = stbi_write_png_to_mem(rgba.data(), w * 4, w, h, 4, &len);
        if (!png)
        {
            return false;
        }
        FILE* f = _wfopen(path.c_str(), L"wb");
        const bool ok = f && fwrite(png, 1, static_cast<size_t>(len), f) == static_cast<size_t>(len);
        if (f)
        {
            fclose(f);
        }
        STBIW_FREE(png);
        return ok;
    }

    bool snapshot(const std::wstring& path)
    {
        std::vector<uint32_t> px;
        return readPixels(px) && writePng(path, px, g_opt.width, g_opt.height);
    }

    std::wstring widen(const std::string& s)
    {
        if (s.empty())
        {
            return {};
        }
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
        return w;
    }

    std::string narrow(const std::wstring& w)
    {
        if (w.empty())
        {
            return {};
        }
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
        return s;
    }

    // ---- The driver script's natives (sbx.*) ----

    // Captured command lists (sbx.capture_begin / capture_end), compared by sbx.compare and sbx.render_compare.
    std::vector<std::vector<Ui::Command>> g_captures;
    std::vector<Ui::Command> g_capturing;
    bool g_captureOn = false;

    // Expands retained lists into their commands (marked by `retained` in the flat list's second vector).
    void flatten(const std::vector<Ui::Command>& in, std::vector<const Ui::Command*>& out)
    {
        for (const Ui::Command& c : in)
        {
            if (c.kind == Ui::Kind::Retained)
            {
                if (c.retained)
                {
                    for (const Ui::Command& r : Ui::hostRetained(*c.retained))
                    {
                        out.push_back(&r);
                    }
                }
            }
            else
            {
                out.push_back(&c);
            }
        }
    }

    std::string describe(const Ui::Command& c)
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "kind %d layer %d window %d at %.3f,%.3f size %.3f,%.3f color %08x image %u region %u clip %d (%.2f %.2f %.2f %.2f)",
                      static_cast<int>(c.kind), static_cast<int>(c.layer), c.window, c.x, c.y, c.w, c.h, c.color, c.image, c.region, c.clipped ? 1 : 0,
                      c.clip[0], c.clip[1], c.clip[2], c.clip[3]);
        return buf;
    }

    // Layout floats of two documents laid out along different update paths may differ in the last bits.
    bool closeTo(float a, float b) { return std::fabs(a - b) <= 1e-3f; }
    bool closeTo4(const float* a, const float* b) { return closeTo(a[0], b[0]) && closeTo(a[1], b[1]) && closeTo(a[2], b[2]) && closeTo(a[3], b[3]); }

    // Two commands draw the same: everything but the mesh id, and the meshes' vertices and indices.
    bool sameCommand(const Ui::Command& a, const Ui::Command& b, std::string& why)
    {
        auto fail = [&](const char* what) {
            why = what;
            return false;
        };
        if (a.kind != b.kind) return fail("kind");
        if (a.layer != b.layer || a.window != b.window) return fail("layer / window");
        if (!closeTo(a.x, b.x) || !closeTo(a.y, b.y) || !closeTo(a.w, b.w) || !closeTo(a.h, b.h)) return fail("position / size");
        if (a.color != b.color) return fail("color");
        if (a.image != b.image) return fail("image");
        if (a.flip != b.flip || a.point != b.point || a.fit != b.fit) return fail("flip / point / fit");
        if (std::memcmp(a.crop, b.crop, sizeof(a.crop)) != 0) return fail("crop");
        if (a.clipped != b.clipped || (a.clipped && !closeTo4(a.clip, b.clip))) return fail("clip");
        if (!a.matrix != !b.matrix || (a.matrix && *a.matrix != *b.matrix)) return fail("transform");
        if (a.kind == Ui::Kind::Text && (a.text != b.text || a.font != b.font || a.flags != b.flags)) return fail("text");
        if (a.kind == Ui::Kind::Mesh)
        {
            std::vector<Ui::MeshVertex> va, vb;
            std::vector<int> ia, ib;
            const bool ha = Ui::hostMesh(a.region, va, ia), hb = Ui::hostMesh(b.region, vb, ib);
            if (ha != hb) return fail("mesh exists");
            if (ia != ib) return fail("mesh indices");
            if (va.size() != vb.size())
            {
                why = "mesh vertex count " + std::to_string(va.size()) + " vs " + std::to_string(vb.size());
                return false;
            }
            for (size_t i = 0; i < va.size(); ++i)
            {
                if (!closeTo(va[i].x, vb[i].x) || !closeTo(va[i].y, vb[i].y) || va[i].color != vb[i].color || !closeTo(va[i].u, vb[i].u) || !closeTo(va[i].v, vb[i].v))
                {
                    char buf[200];
                    std::snprintf(buf, sizeof(buf), "mesh vertex %u of %u: (%.3f %.3f %08x %.4f %.4f) vs (%.3f %.3f %08x %.4f %.4f)",
                                  static_cast<unsigned>(i), static_cast<unsigned>(va.size()), va[i].x, va[i].y, va[i].color, va[i].u, va[i].v,
                                  vb[i].x, vb[i].y, vb[i].color, vb[i].u, vb[i].v);
                    why = buf;
                    return false;
                }
            }
        }
        if (a.kind == Ui::Kind::Region && a.region != b.region) return fail("region handle");
        return true;
    }

    int sbxCaptureBegin(lua_State*)
    {
        g_capturing.clear();
        g_captureOn = true;
        Ui::hostCapture(&g_capturing);
        return 0;
    }

    int sbxCaptureEnd(lua_State* L)
    {
        Ui::hostCapture(nullptr);
        g_captureOn = false;
        g_captures.push_back(std::move(g_capturing));
        g_capturing.clear();
        lua_pushnumber(L, static_cast<lua_Number>(g_captures.size()));
        return 1;
    }

    const std::vector<Ui::Command>& captureArg(lua_State* L, int arg)
    {
        const int i = static_cast<int>(luaL_checknumber(L, arg));
        if (i < 1 || i > static_cast<int>(g_captures.size()))
        {
            luaL_error(L, "no capture %d", i);
        }
        return g_captures[static_cast<size_t>(i - 1)];
    }

    // sbx.capture_info(h) -> commands (flattened), retained commands in it
    int sbxCaptureInfo(lua_State* L)
    {
        const auto& list = captureArg(L, 1);
        std::vector<const Ui::Command*> flat;
        flatten(list, flat);
        int retained = 0;
        for (const Ui::Command& c : list)
        {
            retained += c.kind == Ui::Kind::Retained;
        }
        lua_pushnumber(L, static_cast<lua_Number>(flat.size()));
        lua_pushnumber(L, retained);
        return 2;
    }

    // sbx.compare(a, b) -> true | false, message: the two captures draw the same commands (retained lists expanded, meshes by content).
    int sbxCompare(lua_State* L)
    {
        std::vector<const Ui::Command*> a, b;
        flatten(captureArg(L, 1), a);
        flatten(captureArg(L, 2), b);
        const size_t n = std::min(a.size(), b.size());
        for (size_t i = 0; i < n; ++i)
        {
            std::string why;
            if (!sameCommand(*a[i], *b[i], why))
            {
                lua_pushboolean(L, 0);
                const std::string msg = "command " + std::to_string(i + 1) + ": " + why + "\n  a: " + describe(*a[i]) + "\n  b: " + describe(*b[i]);
                lua_pushstring(L, msg.c_str());
                return 2;
            }
        }
        if (a.size() != b.size())
        {
            lua_pushboolean(L, 0);
            const std::string msg = "a has " + std::to_string(a.size()) + " commands, b " + std::to_string(b.size()) +
                (a.size() > n ? "\n  first extra a: " + describe(*a[n]) : std::string()) +
                (b.size() > n ? "\n  first extra b: " + describe(*b[n]) : std::string());
            lua_pushstring(L, msg.c_str());
            return 2;
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    bool renderCapture(const std::vector<Ui::Command>& list, std::vector<uint32_t>& px)
    {
        beginCanvas();
        Ui::hostDrawList(list, g_device);
        g_device->EndScene();
        return readPixels(px);
    }

    // sbx.render_compare(a, b, prefix?) -> differing pixels, largest channel difference: draws each capture alone; with `prefix`, a mismatch is
    // saved as prefix_a.png and prefix_b.png.
    int sbxRenderCompare(lua_State* L)
    {
        std::vector<uint32_t> pa, pb;
        const bool ok = renderCapture(captureArg(L, 1), pa) && renderCapture(captureArg(L, 2), pb);
        if (!ok)
        {
            return luaL_error(L, "cannot read the render target");
        }
        int count = 0, largest = 0;
        for (size_t i = 0; i < pa.size(); ++i)
        {
            if (pa[i] != pb[i])
            {
                ++count;
                for (int s = 0; s < 24; s += 8)
                {
                    largest = std::max(largest, std::abs(static_cast<int>((pa[i] >> s) & 255) - static_cast<int>((pb[i] >> s) & 255)));
                }
            }
        }
        if (count && lua_isstring(L, 3))
        {
            const std::wstring prefix = widen(lua_tostring(L, 3));
            writePng(prefix + L"_a.png", pa, g_opt.width, g_opt.height);
            writePng(prefix + L"_b.png", pb, g_opt.width, g_opt.height);
        }
        lua_pushnumber(L, count);
        lua_pushnumber(L, largest);
        return 2;
    }

    int sbxCaptureClear(lua_State*)
    {
        g_captures.clear();
        return 0;
    }

    // sbx.step(n [, dt]): n ticks without drawing (fast-forward: timers, animations and mods run, nothing is rendered).
    int sbxStep(lua_State* L)
    {
        const int n = static_cast<int>(luaL_optnumber(L, 1, 1));
        const double saved = g_opt.dt;
        if (lua_isnumber(L, 2))
        {
            g_opt.dt = lua_tonumber(L, 2);
        }
        for (int i = 0; i < n && !g_quit; ++i)
        {
            frame(false);
        }
        g_opt.dt = saved;
        lua_pushboolean(L, !g_quit);
        return 1;
    }

    // sbx.set_time(seconds): the simulated clock jumps to that time; the next frame sees it plus dt (sbx.frame(1, 0) shows exactly it).
    // Forward jumps are what RmlUi expects: an animation started earlier is then shown at that point of its timeline. Going back is allowed
    // but RmlUi's timers do not run backwards (an animation stays where it is until the time passes it again).
    int sbxSetTime(lua_State* L)
    {
        g_simTime = luaL_checknumber(L, 1);
        return 0;
    }

    // sbx.seek(seconds [, fn]): moves the simulated clock forward to that time in steps of at most 1/16 s, a tick without drawing after each
    // (the mods' timers and documents run), and fn() after each too (a script that updates its documents itself). RmlUi advances an
    // animation by at most 0.1 s per update, so a jump of the clock alone would show an earlier frame of it; this shows the frame at
    // `seconds`. The steps are powers of two: the times stay exact in floats.
    int sbxSeek(lua_State* L)
    {
        const double target = luaL_checknumber(L, 1);
        const bool callback = lua_isfunction(L, 2);
        const double saved = g_opt.dt;
        while (g_simTime < target && !g_quit)
        {
            g_opt.dt = std::min(1.0 / 16.0, target - g_simTime);
            frame(false);
            if (callback)
            {
                lua_pushvalue(L, 2);
                lua_call(L, 0, 0);
            }
        }
        g_opt.dt = saved;
        return 0;
    }

    // sbx.dt([seconds]) -> the frame time of the simulated clock (sets it with an argument; default 1/64)
    int sbxDt(lua_State* L)
    {
        if (lua_isnumber(L, 1))
        {
            g_opt.dt = lua_tonumber(L, 1);
        }
        lua_pushnumber(L, g_opt.dt);
        return 1;
    }

    // sbx.frame([n [, dt]]): n frames (dt: their frame time instead of sbx.dt(); 0 draws the present time again).
    int sbxFrame(lua_State* L)
    {
        const int n = static_cast<int>(luaL_optnumber(L, 1, 1));
        const double saved = g_opt.dt;
        if (lua_isnumber(L, 2))
        {
            g_opt.dt = lua_tonumber(L, 2);
        }
        for (int i = 0; i < n && !g_quit; ++i)
        {
            frame();
        }
        g_opt.dt = saved;
        lua_pushboolean(L, !g_quit);
        return 1;
    }

    // sbx.advance(seconds): moves the simulated clock without a frame (the next native call sees the new time).
    int sbxAdvance(lua_State* L)
    {
        g_simTime += luaL_checknumber(L, 1);
        return 0;
    }

    int sbxTime(lua_State* L)
    {
        lua_pushnumber(L, GearClock::seconds());
        return 1;
    }

    int sbxMouse(lua_State* L)
    {
        g_mouseX = static_cast<int>(luaL_checknumber(L, 1));
        g_mouseY = static_cast<int>(luaL_checknumber(L, 2));
        return 0;
    }

    int sbxButton(lua_State* L)
    {
        const bool down = lua_toboolean(L, 1) != 0;
        const int button = static_cast<int>(luaL_optnumber(L, 2, 0));
        lua_pushboolean(L, Ui::hostMouse(down, button, g_mouseX, g_mouseY));
        return 1;
    }

    // sbx.key(vk, down): a key held or let go as mods see it (gear.input / key bindings ask Ui::keyDown, which counts automation's keys).
    int sbxKey(lua_State* L)
    {
        Automation::key(static_cast<int>(luaL_checknumber(L, 1)), lua_toboolean(L, 2) != 0);
        return 0;
    }

    int sbxWheel(lua_State* L)
    {
        lua_pushboolean(L, Ui::hostWheel(static_cast<int>(luaL_checknumber(L, 1)), g_mouseX, g_mouseY));
        return 1;
    }

    // sbx.pixel(x, y) -> 0xRRGGBB of the last drawn frame at (x, y) (canvas pixels), nil outside
    int sbxPixel(lua_State* L)
    {
        const int x = static_cast<int>(luaL_checknumber(L, 1)), y = static_cast<int>(luaL_checknumber(L, 2));
        std::vector<uint32_t> px;
        if (x < 0 || y < 0 || x >= g_opt.width || y >= g_opt.height || !readPixels(px))
        {
            return 0;
        }
        lua_pushnumber(L, px[static_cast<size_t>(y) * g_opt.width + x] & 0xFFFFFF);
        return 1;
    }

    int sbxSnapshot(lua_State* L)
    {
        lua_pushboolean(L, snapshot(widen(luaL_checkstring(L, 1))));
        return 1;
    }

    int sbxDraw(lua_State*)
    {
        // Draws what is published now without a tick (after the script recorded commands itself and called sbx.commit).
        beginCanvas();
        Ui::hostDraw(Ui::Layer::World, g_device);
        Ui::hostDraw(Ui::Layer::Hud, g_device);
        Ui::hostDraw(Ui::Layer::Top, g_device);
        present();
        return 0;
    }

    int sbxCommit(lua_State*)
    {
        Ui::commit();
        return 0;
    }

    // sbx.clip(on [, x, y, w, h]): the clip of the commands recorded from now on (Ui::setClip, as gear.ui.scroll_begin sets it).
    int sbxClip(lua_State* L)
    {
        Ui::setClip(lua_toboolean(L, 1) != 0, static_cast<float>(luaL_optnumber(L, 2, 0)), static_cast<float>(luaL_optnumber(L, 3, 0)),
                    static_cast<float>(luaL_optnumber(L, 4, 0)), static_cast<float>(luaL_optnumber(L, 5, 0)));
        return 0;
    }

    int sbxLog(lua_State* L)
    {
        const char* s = luaL_checkstring(L, 1);
        LOG("script: {}", s);
        std::printf("%s\n", s);
        std::fflush(stdout);
        return 0;
    }

    int sbxSize(lua_State* L)
    {
        lua_pushnumber(L, g_opt.width);
        lua_pushnumber(L, g_opt.height);
        return 2;
    }

    int sbxImage(lua_State* L)
    {
        Ui::HostImage h;
        if (!hostImage(static_cast<uint32_t>(luaL_checknumber(L, 1)), h))
        {
            return 0;
        }
        lua_pushstring(L, h.sheet.c_str());
        lua_pushnumber(L, h.width);
        lua_pushnumber(L, h.height);
        return 3;
    }

    // Runs the driver script: `sbx` (above) and `rml` (Gear's RmlUi natives, for tests that drive documents directly) are globals; `arg` the
    // script's arguments. Returns the process exit code: 0, or what the script returned (a number), 1 on an error.
    int runScript()
    {
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        static constexpr luaL_Reg kSbx[] = {
            {"frame", sbxFrame}, {"step", sbxStep}, {"set_time", sbxSetTime}, {"seek", sbxSeek}, {"dt", sbxDt}, {"advance", sbxAdvance}, {"key", sbxKey}, {"time", sbxTime}, {"mouse", sbxMouse}, {"button", sbxButton},
            {"wheel", sbxWheel}, {"snapshot", sbxSnapshot}, {"pixel", sbxPixel}, {"draw", sbxDraw}, {"commit", sbxCommit}, {"log", sbxLog},
            {"size", sbxSize}, {"image", sbxImage}, {"capture_begin", sbxCaptureBegin}, {"capture_end", sbxCaptureEnd},
            {"capture_info", sbxCaptureInfo}, {"capture_clear", sbxCaptureClear}, {"compare", sbxCompare},
            {"render_compare", sbxRenderCompare}, {"clip", sbxClip}, {nullptr, nullptr},
        };
        lua_newtable(L);
        for (const luaL_Reg* r = kSbx; r->name; ++r)
        {
            lua_pushcfunction(L, r->func);
            lua_setfield(L, -2, r->name);
        }
        lua_pushstring(L, narrow(g_opt.modsDir).c_str());
        lua_setfield(L, -2, "mods_dir");
        lua_setglobal(L, "sbx");
        lua_newtable(L);
        RmlHost::registerNatives(L);
        lua_setglobal(L, "rml");
        lua_newtable(L);
        for (size_t i = 0; i < g_opt.scriptArgs.size(); ++i)
        {
            lua_pushstring(L, g_opt.scriptArgs[i].c_str());
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        lua_setglobal(L, "arg");
        const std::string path = narrow(g_opt.script);
        int code = 0;
        if (luaL_loadfile(L, path.c_str()) != 0 || lua_pcall(L, 0, 1, 0) != 0)
        {
            const char* msg = lua_tostring(L, -1);
            std::fprintf(stderr, "script error: %s\n", msg ? msg : "?");
            LOG("Sandbox: script error: {}", msg ? msg : "?");
            code = 1;
        }
        else if (lua_isnumber(L, -1))
        {
            code = static_cast<int>(lua_tonumber(L, -1));
        }
        lua_close(L);
        return code;
    }

    // --enable: gear_mods.json with these mods on and every other one off (a fresh file: fine for a sandbox folder).
    bool enableMods()
    {
        std::vector<std::string> on;
        std::error_code ec;
        const std::string list = "," + g_opt.enable + ",";
        for (const auto& e : fs::directory_iterator(g_opt.modsDir, ec))
        {
            if (e.is_directory() && fs::exists(e.path() / L"mod.lua"))
            {
                const std::string id = narrow(e.path().filename().wstring());
                if (g_opt.enable == "all" || list.find("," + id + ",") != std::string::npos)
                {
                    on.push_back(id);
                }
            }
        }
        std::string json = "{\"mods\":{";
        for (size_t i = 0; i < on.size(); ++i)
        {
            json += (i ? ",\"" : "\"") + on[i] + "\":{\"enabled\":true}";
        }
        json += "},\"version\":1}";
        FILE* f = _wfopen((fs::path(g_opt.modsDir) / L"gear_mods.json").c_str(), L"wb");
        if (!f)
        {
            return false;
        }
        fwrite(json.data(), 1, json.size(), f);
        fclose(f);
        LOG("Sandbox: mods switched on: {}", on.size());
        return true;
    }

    void usage()
    {
        std::printf(
            "gear_sandbox [options]\n"
            "  --game <dir>         Sacred Gold install (read only): Sacred.exe, PAK, font (default B:\\Spiele\\GOG Games\\sacred gold GOG)\n"
            "  --mods <dir>         mods folder (default: mods next to the sandbox); gear_mods.json and the layout are written there\n"
            "  --no-mods            no kernel, no mods (scripts drive the RmlUi natives themselves)\n"
            "  --size WxH           canvas (default 1024x768; layout units are its pixels)\n"
            "  --mode game|menus    gear.ui.state().mode (default game)\n"
            "  --hidden             no visible window: render off-screen\n"
            "  --frames N           stop after N frames\n"
            "  --snapshot file.png  save the last frame\n"
            "  --background file    an image drawn under the UI (PNG, JPG, TGA)\n"
            "  --enable all|a,b,c   switch these mods on (gear_mods.json in the mods folder; all: every folder with a mod.lua)\n"
            "  --log file           the log (default SacredGearSandbox.log next to the sandbox)\n"
            "  --sim [dt]           simulated clock, dt seconds per frame (default 1/64, exact in floats); scripts always run on it\n"
            "  --script file.lua [args...]  run a driver script (the rest of the command line is its arg table)\n");
    }

    bool parse(int argc, wchar_t** argv)
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::wstring a = argv[i];
            auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : std::wstring(); };
            if (a == L"--game") g_opt.gameDir = next();
            else if (a == L"--mods") g_opt.modsDir = next();
            else if (a == L"--no-mods") g_opt.mods = false;
            else if (a == L"--hidden") g_opt.hidden = true;
            else if (a == L"--frames") g_opt.frames = _wtoi(next().c_str());
            else if (a == L"--snapshot") g_opt.snapshot = next();
            else if (a == L"--background") g_opt.background = next();
            else if (a == L"--log") g_opt.log = next();
            else if (a == L"--enable") g_opt.enable = narrow(next());
            else if (a == L"--mode") g_opt.mode = narrow(next());
            else if (a == L"--size")
            {
                const std::wstring s = next();
                if (swscanf_s(s.c_str(), L"%dx%d", &g_opt.width, &g_opt.height) != 2 || g_opt.width < 64 || g_opt.height < 64)
                {
                    return false;
                }
            }
            else if (a == L"--sim")
            {
                g_opt.simulated = true;
                if (i + 1 < argc && argv[i + 1][0] != L'-')
                {
                    g_opt.dt = _wtof(argv[++i]);
                }
            }
            else if (a == L"--script")
            {
                g_opt.script = next();
                g_opt.simulated = true;
                while (i + 1 < argc)
                {
                    g_opt.scriptArgs.push_back(narrow(argv[++i]));
                }
            }
            else
            {
                return false;
            }
        }
        return true;
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (!parse(argc, argv))
    {
        usage();
        return 2;
    }
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const fs::path exeDir = fs::path(exe).parent_path();
    if (g_opt.modsDir.empty())
    {
        g_opt.modsDir = (exeDir / L"mods").wstring();
    }
    g_opt.modsDir = fs::absolute(g_opt.modsDir).wstring();
    std::error_code ec;
    fs::create_directories(g_opt.modsDir, ec);
    Log::init(g_opt.log.empty() ? (exeDir / L"SacredGearSandbox.log").c_str() : g_opt.log.c_str());
    if (!g_opt.enable.empty() && !enableMods())
    {
        std::fprintf(stderr, "cannot write gear_mods.json in %s\n", narrow(g_opt.modsDir).c_str());
    }
    LOG("Sandbox: game {}, mods {}, {}x{}{}", narrow(g_opt.gameDir), narrow(g_opt.modsDir), g_opt.width, g_opt.height,
        g_opt.hidden ? ", hidden" : "");

    std::string error;
    if (!GameData::open(g_opt.gameDir, error))
    {
        std::fprintf(stderr, "game files: %s\n", error.c_str());
        return 3;
    }
    LOG("Sandbox: {} UI images, {} textures", GameData::uiImages().size(), GameData::textureCount());
    if (g_opt.simulated)
    {
        GearClock::g_override = simClock;
    }
    Ui::setHost(&g_host);
    if (!createWindow() || !createDevice(error))
    {
        std::fprintf(stderr, "Direct3D: %s\n", error.c_str());
        return 4;
    }
    if (!loadBackground())
    {
        std::fprintf(stderr, "cannot load the background %s\n", narrow(g_opt.background).c_str());
    }
    LuaHost::configureFolders(GetModuleHandleW(nullptr), g_opt.gameDir, g_opt.modsDir);
    RmlHost::setFontDir((fs::path(g_opt.gameDir) / L"font").wstring());

    int code = 0;
    if (!g_opt.script.empty())
    {
        code = runScript();
    }
    else
    {
        LARGE_INTEGER freq, last, now;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&last);
        while (!g_quit && (g_opt.frames <= 0 || g_frame < g_opt.frames))
        {
            if (!g_opt.simulated)
            {
                QueryPerformanceCounter(&now);
                g_opt.dt = std::clamp(static_cast<double>(now.QuadPart - last.QuadPart) / freq.QuadPart, 0.001, 0.1);
                last = now;
            }
            frame();
            if (!g_opt.hidden && !g_opt.simulated)
            {
                Sleep(1);
            }
        }
        if (!g_opt.snapshot.empty() && !snapshot(g_opt.snapshot))
        {
            std::fprintf(stderr, "cannot save %s\n", narrow(g_opt.snapshot).c_str());
            code = 5;
        }
    }
    LOG("Sandbox: done after {} frames, exit {}", g_frame, code);
    // The process ends here: Gear's statics (RmlUi, Lua, textures) are left to the OS, as the game leaves them.
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
    return code;
}
