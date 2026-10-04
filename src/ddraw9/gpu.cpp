#include "ddraw9/gpu.h"
#include "ddraw9/format.h"
#include "config.h"
#include "log.h"

#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace DDraw9::Gpu
{
    namespace
    {
        using CreateFn = HRESULT(WINAPI*)(UINT, d9::IDirect3D9Ex**);

        RecursiveSpinLock g_lock;
        bool g_loaded = false;
        d9::IDirect3D9Ex* g_d3d = nullptr;
        UINT g_adapter = D3DADAPTER_DEFAULT;
        d9::D3DCAPS9 g_caps = {};
        HWND g_window = nullptr;
        bool g_fpuPreserve = false;
        d9::IDirect3DDevice9Ex* g_device = nullptr;
        d9::D3DPRESENT_PARAMETERS g_params = {};
        bool g_backBufferFrozen = false;
        bool g_presentFailed = false;

        UINT adapterOf(HWND window)
        {
            const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
            const UINT count = g_d3d->GetAdapterCount();
            for (UINT i = 0; i < count; ++i)
            {
                if (g_d3d->GetAdapterMonitor(i) == monitor)
                {
                    return i;
                }
            }
            return D3DADAPTER_DEFAULT;
        }

        const char* swapEffectName(d9::D3DSWAPEFFECT effect)
        {
            return effect == d9::D3DSWAPEFFECT_FLIPEX ? "flip model" : "blit model";
        }

        std::string narrow(const std::wstring& s)
        {
            std::string out;
            for (wchar_t c : s)
            {
                out += c < 0x80 ? static_cast<char>(c) : '?';
            }
            return out;
        }

        std::wstring exeDirectory()
        {
            wchar_t path[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, path, MAX_PATH);
            std::wstring dir = path;
            return dir.substr(0, dir.find_last_of(L"\\/"));
        }

        // Where d3d9.dll may come from, in order: [DDraw] D3D9 (relative to the game folder unless absolute), a
        // d3d9.dll next to the exe (e.g. DXVK), Windows' own.
        std::vector<std::wstring> d3d9Candidates()
        {
            std::vector<std::wstring> paths;
            const std::wstring gameDir = exeDirectory();
            if (!g_config.d3d9Path.empty())
            {
                const std::wstring& p = g_config.d3d9Path;
                const bool absolute = (p.size() > 1 && p[1] == L':') || p.starts_with(L"\\\\");
                paths.push_back(absolute ? p : gameDir + L"\\" + p);
            }
            const std::wstring local = gameDir + L"\\d3d9.dll";
            if (GetFileAttributesW(local.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                paths.push_back(local);
            }
            wchar_t system[MAX_PATH] = {};
            GetSystemDirectoryW(system, MAX_PATH);
            paths.push_back(std::wstring(system) + L"\\d3d9.dll");
            return paths;
        }

        // Loads `path` and creates the Direct3D 9Ex object from it.
        d9::IDirect3D9Ex* createFrom(const std::wstring& path)
        {
            HMODULE module = LoadLibraryW(path.c_str());
            if (!module)
            {
                LOG("Direct3D 9: {} not loaded ({})", narrow(path), GetLastError());
                return nullptr;
            }
            auto create = reinterpret_cast<CreateFn>(GetProcAddress(module, "Direct3DCreate9Ex"));
            d9::IDirect3D9Ex* d3d = nullptr;
            const HRESULT hr = create ? create(D3D_SDK_VERSION, &d3d) : E_NOINTERFACE;
            if (FAILED(hr) || !d3d)
            {
                LOG("Direct3D 9: {}: {} ({:08x})", narrow(path), create ? "Direct3DCreate9Ex failed" : "no Direct3DCreate9Ex",
                    static_cast<uint32_t>(hr));
                FreeLibrary(module);
                return nullptr;
            }
            LOG("Direct3D 9: using {}", narrow(path));
            return d3d;
        }
    }

    bool available()
    {
        std::scoped_lock lock(g_lock);
        if (g_loaded)
        {
            return g_d3d != nullptr;
        }
        g_loaded = true;
        for (const std::wstring& path : d3d9Candidates())
        {
            if ((g_d3d = createFrom(path)) != nullptr)
            {
                break;
            }
        }
        if (!g_d3d)
        {
            return false;
        }
        g_d3d->GetDeviceCaps(g_adapter, d9::D3DDEVTYPE_HAL, &g_caps);
        d9::D3DADAPTER_IDENTIFIER9 id = {};
        g_d3d->GetAdapterIdentifier(g_adapter, 0, &id);
        LOG("Direct3D 9: {} (driver {}), max texture {}x{}", id.Description, id.Driver, g_caps.MaxTextureWidth,
            g_caps.MaxTextureHeight);
        return true;
    }

    d9::IDirect3D9Ex* d3d()
    {
        return g_d3d;
    }

    UINT adapter()
    {
        return g_adapter;
    }

    const d9::D3DCAPS9& caps()
    {
        return g_caps;
    }

    void setWindow(HWND window, bool fpuPreserve)
    {
        std::scoped_lock lock(g_lock);
        if (window && window != g_window)
        {
            g_window = window;
            if (!g_device && g_d3d)
            {
                g_adapter = adapterOf(window);
                g_d3d->GetDeviceCaps(g_adapter, d9::D3DDEVTYPE_HAL, &g_caps);
            }
        }
        g_fpuPreserve = fpuPreserve;
    }

    d9::IDirect3DDevice9Ex* existingDevice()
    {
        return g_device;
    }

    d9::IDirect3DDevice9Ex* device(UINT width, UINT height)
    {
        if (g_device)
        {
            return g_device;
        }
        std::scoped_lock lock(g_lock);
        if (g_device || !available())
        {
            return g_device;
        }
        HWND window = g_window ? g_window : GetForegroundWindow();
        if (!width || !height)
        {
            RECT client = {};
            if (window && GetClientRect(window, &client))
            {
                width = client.right - client.left;
                height = client.bottom - client.top;
            }
            if (!width || !height)
            {
                width = 1024;
                height = 768;
            }
        }

        g_params = {};
        g_params.BackBufferWidth = width;
        g_params.BackBufferHeight = height;
        g_params.BackBufferFormat = d9::D3DFMT_X8R8G8B8;
        g_params.BackBufferCount = 2;
        g_params.SwapEffect = d9::D3DSWAPEFFECT_FLIPEX;
        g_params.hDeviceWindow = window;
        g_params.Windowed = TRUE;
        g_params.PresentationInterval = g_config.vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;

        // The game calls the device from two threads and loads textures while drawing, so Direct3D serializes.
        // A pure device first: the Direct3D 7 device keeps its own copy of all state and never reads it back.
        const DWORD common = D3DCREATE_MULTITHREADED | D3DCREATE_NOWINDOWCHANGES | (g_fpuPreserve ? D3DCREATE_FPU_PRESERVE : 0);
        const DWORD attempts[] = {
            common | D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_PUREDEVICE,
            common | D3DCREATE_HARDWARE_VERTEXPROCESSING,
            common | D3DCREATE_SOFTWARE_VERTEXPROCESSING,
        };
        HRESULT hr = E_FAIL;
        for (int swap = 0; swap < 2 && !g_device; ++swap)
        {
            if (swap == 1)
            {
                // Flip model needs Windows 7+ and a plain top-level window; fall back to the blit model.
                g_params.SwapEffect = d9::D3DSWAPEFFECT_DISCARD;
                g_params.BackBufferCount = 1;
            }
            for (DWORD flags : attempts)
            {
                hr = g_d3d->CreateDeviceEx(g_adapter, d9::D3DDEVTYPE_HAL, window, flags, &g_params, nullptr, &g_device);
                if (SUCCEEDED(hr))
                {
                    LOG("Direct3D 9: device {}x{}, {}, {}, {}vertex processing{}, vsync {}, frame latency {}", width, height,
                        swapEffectName(g_params.SwapEffect), g_params.BackBufferCount == 1 ? "1 back buffer" : "2 back buffers",
                        (flags & D3DCREATE_SOFTWARE_VERTEXPROCESSING) ? "software " : "hardware ",
                        (flags & D3DCREATE_PUREDEVICE) ? " (pure)" : "", g_config.vsync ? "on" : "off", g_config.maxFrameLatency);
                    break;
                }
                g_device = nullptr;
            }
        }
        if (!g_device)
        {
            LOG("Direct3D 9: CreateDeviceEx failed ({:08x})", static_cast<uint32_t>(hr));
            return nullptr;
        }
        if (g_config.maxFrameLatency > 0)
        {
            g_device->SetMaximumFrameLatency(static_cast<UINT>(g_config.maxFrameLatency));
        }
        return g_device;
    }

    void matchBackBuffer(UINT width, UINT height)
    {
        std::scoped_lock lock(g_lock);
        if (!g_device || g_backBufferFrozen || (g_params.BackBufferWidth == width && g_params.BackBufferHeight == height))
        {
            return;
        }
        d9::D3DPRESENT_PARAMETERS params = g_params;
        params.BackBufferWidth = width;
        params.BackBufferHeight = height;
        const HRESULT hr = g_device->ResetEx(&params, nullptr);
        LOG("Direct3D 9: back buffer {}x{} -> {}x{} ({:08x})", g_params.BackBufferWidth, g_params.BackBufferHeight, width,
            height, static_cast<uint32_t>(hr));
        if (SUCCEEDED(hr))
        {
            g_params = params;
            if (g_config.maxFrameLatency > 0)
            {
                g_device->SetMaximumFrameLatency(static_cast<UINT>(g_config.maxFrameLatency));
            }
        }
    }

    void freezeBackBuffer()
    {
        std::scoped_lock lock(g_lock);
        g_backBufferFrozen = true;
    }

    HRESULT present(d9::IDirect3DSurface9* source, const RECT* sourceRect)
    {
        d9::IDirect3DDevice9Ex* dev = g_device;
        if (!dev || !source)
        {
            return DDERR_GENERIC;
        }
        d9::IDirect3DSurface9* backBuffer = nullptr;
        HRESULT hr = dev->GetBackBuffer(0, 0, d9::D3DBACKBUFFER_TYPE_MONO, &backBuffer);
        if (SUCCEEDED(hr))
        {
            d9::D3DSURFACE_DESC src = {};
            source->GetDesc(&src);
            const UINT w = sourceRect ? sourceRect->right - sourceRect->left : src.Width;
            const UINT h = sourceRect ? sourceRect->bottom - sourceRect->top : src.Height;
            const bool scaled = w != g_params.BackBufferWidth || h != g_params.BackBufferHeight;
            hr = dev->StretchRect(source, sourceRect, backBuffer, nullptr, scaled ? d9::D3DTEXF_LINEAR : d9::D3DTEXF_POINT);
            backBuffer->Release();
        }
        if (SUCCEEDED(hr))
        {
            hr = dev->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
        }
        if (FAILED(hr) && !g_presentFailed)
        {
            g_presentFailed = true;
            LOG("Direct3D 9: presenting failed ({:08x}){}", static_cast<uint32_t>(hr),
                hr == D3DERR_DEVICEREMOVED || hr == D3DERR_DEVICEHUNG ? ": the GPU device was removed or hung" : "");
        }
        // Occluded (minimized) windows and mode changes are not errors for the game.
        return SUCCEEDED(hr) || hr == D3DERR_DEVICELOST ? DD_OK : DDERR_GENERIC;
    }

    d9::D3DDISPLAYMODE displayMode()
    {
        d9::D3DDISPLAYMODE mode = {};
        if (g_d3d && SUCCEEDED(g_d3d->GetAdapterDisplayMode(g_adapter, &mode)))
        {
            return mode;
        }
        mode.Width = GetSystemMetrics(SM_CXSCREEN);
        mode.Height = GetSystemMetrics(SM_CYSCREEN);
        mode.RefreshRate = 60;
        mode.Format = d9::D3DFMT_X8R8G8B8;
        return mode;
    }

    namespace
    {
        bool check(DWORD usage, d9::D3DRESOURCETYPE type, d9::D3DFORMAT format)
        {
            return g_d3d && SUCCEEDED(g_d3d->CheckDeviceFormat(g_adapter, d9::D3DDEVTYPE_HAL, d9::D3DFMT_X8R8G8B8, usage, type, format));
        }
    }

    bool textureFormat(d9::D3DFORMAT format)
    {
        return check(0, d9::D3DRTYPE_TEXTURE, format);
    }

    bool depthFormat(d9::D3DFORMAT format)
    {
        return check(D3DUSAGE_DEPTHSTENCIL, d9::D3DRTYPE_SURFACE, format);
    }
}

namespace DDraw9
{
    void unsupported(const char* what)
    {
        static std::mutex mutex;
        static std::set<std::string> seen;
        std::scoped_lock lock(mutex);
        if (seen.size() < 256 && seen.insert(what).second)
        {
            LOG("Direct3D 9 backend: not supported: {}", what);
        }
    }
}
