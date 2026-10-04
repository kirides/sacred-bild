#include "game/movie.h"
#include "game/device_proxy.h"
#include "game/frame_hooks.h"
#include "game/sacred_addr.h"
#include "game/ui_canvas.h"
#include "config.h"
#include "log.h"
#include "patch.h"

#include <windows.h>
#include <dxgiformat.h>
#include <mfapi.h>
#include <mfmediaengine.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <atomic>
#include <cstring>
#include <iterator>
#include <string>

using Microsoft::WRL::ComPtr;

namespace
{
    using namespace Sacred;

    using OpenStreamFn = HRESULT(__cdecl*)(const char* path, IUnknown* ddraw, IUnknown** stream);
    // thiscall on the movie player (+4 skip count, +8 window), hooked as fastcall.
    using PlayVideoFn = int(__fastcall*)(void* self, void* edx, IDirectDraw* ddraw, IDirectDrawSurface* primary,
        IUnknown* stream, int w, int h);

    OpenStreamFn g_origOpenStream = nullptr;
    PlayVideoFn g_origPlayVideo = nullptr;

    // Not exported by every SDK library this links against.
    constexpr GUID kClsidWicImagingFactory = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
    constexpr GUID kWicPixelFormat32bppBGRA = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0f}};
    // {9B1D2C64-3C1A-4E2B-8F11-7A0E5D6C4B21}: asks a stream object whether it is a MovieFile.
    constexpr GUID kMovieFileIid = {0x9b1d2c64, 0x3c1a, 0x4e2b, {0x8f, 0x11, 0x7a, 0x0e, 0x5d, 0x6c, 0x4b, 0x21}};

    // Takes the place of the game's multimedia stream: the file playVideo is going to play.
    class MovieFile final : public IUnknown
    {
    public:
        MovieFile(std::string name, std::wstring path) : m_name(std::move(name)), m_path(std::move(path)) {}
        const std::string& name() const { return m_name; }      // as the game named it
        const std::wstring& path() const { return m_path; }     // absolute

        STDMETHOD(QueryInterface)(REFIID riid, void** out) override
        {
            if (!out)
            {
                return E_POINTER;
            }
            if (riid == IID_IUnknown || riid == kMovieFileIid)
            {
                *out = this;
                AddRef();
                return S_OK;
            }
            *out = nullptr;
            return E_NOINTERFACE;
        }

        STDMETHOD_(ULONG, AddRef)() override
        {
            return static_cast<ULONG>(InterlockedIncrement(&m_refs));
        }

        STDMETHOD_(ULONG, Release)() override
        {
            const LONG refs = InterlockedDecrement(&m_refs);
            if (refs == 0)
            {
                delete this;
            }
            return static_cast<ULONG>(refs);
        }

    private:
        std::string m_name;
        std::wstring m_path;
        volatile LONG m_refs = 1;
    };

    MovieFile* asMovieFile(IUnknown* stream)
    {
        MovieFile* file = nullptr;
        if (stream && SUCCEEDED(stream->QueryInterface(kMovieFileIid, reinterpret_cast<void**>(&file))))
        {
            file->Release();    // the caller holds the stream
            return file;
        }
        return nullptr;
    }

    // Media Engine events (from Media Foundation's worker threads).
    class EngineEvents final : public IMFMediaEngineNotify
    {
    public:
        std::atomic<bool> ended{false};
        std::atomic<HRESULT> error{S_OK};

        STDMETHOD(QueryInterface)(REFIID riid, void** out) override
        {
            if (!out)
            {
                return E_POINTER;
            }
            if (riid == IID_IUnknown || riid == __uuidof(IMFMediaEngineNotify))
            {
                *out = static_cast<IMFMediaEngineNotify*>(this);
                AddRef();
                return S_OK;
            }
            *out = nullptr;
            return E_NOINTERFACE;
        }

        STDMETHOD_(ULONG, AddRef)() override
        {
            return static_cast<ULONG>(InterlockedIncrement(&m_refs));
        }

        STDMETHOD_(ULONG, Release)() override
        {
            const LONG refs = InterlockedDecrement(&m_refs);
            if (refs == 0)
            {
                delete this;
            }
            return static_cast<ULONG>(refs);
        }

        STDMETHOD(EventNotify)(DWORD event, DWORD_PTR, DWORD param2) override
        {
            if (event == MF_MEDIA_ENGINE_EVENT_ENDED)
            {
                ended = true;
            }
            else if (event == MF_MEDIA_ENGINE_EVENT_ERROR)
            {
                error = FAILED(static_cast<HRESULT>(param2)) ? static_cast<HRESULT>(param2) : E_FAIL;
            }
            return S_OK;
        }

    private:
        volatile LONG m_refs = 1;
    };

    // The keys and buttons the game's own loop skips movies with, while the game has the focus.
    bool skipPressed(HWND window)
    {
        if (window && GetForegroundWindow() != window)
        {
            return false;
        }
        for (int key : {VK_ESCAPE, VK_SPACE, VK_LBUTTON, VK_RBUTTON})
        {
            if (GetAsyncKeyState(key) & 0x8000)
            {
                return true;
            }
        }
        return false;
    }

    bool inputMessage(UINT message)
    {
        return (message >= WM_KEYFIRST && message <= WM_KEYLAST) ||
            (message >= WM_LBUTTONDOWN && message <= WM_MBUTTONDBLCLK) || message == WM_XBUTTONDOWN ||
            message == WM_XBUTTONUP || message == WM_XBUTTONDBLCLK;
    }

    // Keeps the window responsive. Keys and clicks are swallowed: they skip the movie and must not reach the menu
    // behind it. False on WM_QUIT (posted again for the game's own loop).
    bool pumpMessages()
    {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                PostQuitMessage(static_cast<int>(msg.wParam));
                return false;
            }
            if (inputMessage(msg.message))
            {
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        return true;
    }

    // Device state the movie quad changes, restored afterwards: the game's render state cache doesn't know about it.
    class SavedState
    {
    public:
        explicit SavedState(IDirect3DDevice7* device) : m_device(device)
        {
            for (size_t i = 0; i < std::size(kRenderStates); ++i)
            {
                device->GetRenderState(kRenderStates[i], &m_rs[i]);
            }
            for (size_t i = 0; i < std::size(kStageStates); ++i)
            {
                device->GetTextureStageState(kStageStates[i].stage, kStageStates[i].type, &m_tss[i]);
            }
            device->GetTexture(0, &m_texture);
            device->GetViewport(&m_viewport);
        }

        ~SavedState()
        {
            for (size_t i = 0; i < std::size(kRenderStates); ++i)
            {
                m_device->SetRenderState(kRenderStates[i], m_rs[i]);
            }
            for (size_t i = 0; i < std::size(kStageStates); ++i)
            {
                m_device->SetTextureStageState(kStageStates[i].stage, kStageStates[i].type, m_tss[i]);
            }
            m_device->SetTexture(0, m_texture);
            if (m_texture)
            {
                m_texture->Release();
            }
            m_device->SetViewport(&m_viewport);
        }

        SavedState(const SavedState&) = delete;
        SavedState& operator=(const SavedState&) = delete;

    private:
        struct StageState
        {
            DWORD stage;
            D3DTEXTURESTAGESTATETYPE type;
        };
        static constexpr D3DRENDERSTATETYPE kRenderStates[] = {
            D3DRENDERSTATE_ZENABLE, D3DRENDERSTATE_ZWRITEENABLE, D3DRENDERSTATE_ALPHABLENDENABLE,
            D3DRENDERSTATE_ALPHATESTENABLE, D3DRENDERSTATE_CULLMODE, D3DRENDERSTATE_FOGENABLE,
            D3DRENDERSTATE_STENCILENABLE, D3DRENDERSTATE_SPECULARENABLE,
        };
        static constexpr StageState kStageStates[] = {
            {0, D3DTSS_COLOROP}, {0, D3DTSS_COLORARG1}, {0, D3DTSS_ALPHAOP}, {0, D3DTSS_ALPHAARG1},
            {0, D3DTSS_TEXCOORDINDEX}, {0, D3DTSS_MAGFILTER}, {0, D3DTSS_MINFILTER}, {0, D3DTSS_MIPFILTER},
            {0, D3DTSS_ADDRESSU}, {0, D3DTSS_ADDRESSV}, {0, D3DTSS_TEXTURETRANSFORMFLAGS},
            {1, D3DTSS_COLOROP}, {1, D3DTSS_ALPHAOP},
        };

        IDirect3DDevice7* m_device;
        DWORD m_rs[std::size(kRenderStates)] = {};
        DWORD m_tss[std::size(kStageStates)] = {};
        IDirectDrawSurface7* m_texture = nullptr;
        D3DVIEWPORT7 m_viewport = {};
    };

    struct TlVertex
    {
        float x, y, z, rhw;
        float u, v;
    };
    constexpr DWORD kTlFvf = D3DFVF_XYZRHW | D3DFVF_TEX1;

    class Player
    {
    public:
        Player(IDirect3DDevice7* device, IDirectDraw7* ddraw, IDirectDrawSurface7* primary, IDirectDrawSurface7* target)
            : m_device(device), m_ddraw(ddraw), m_primary(primary), m_target(target)
        {
            DDSURFACEDESC2 desc = {};
            desc.dwSize = sizeof(desc);
            target->GetSurfaceDesc(&desc);
            m_screenW = desc.dwWidth;
            m_screenH = desc.dwHeight;
        }

        ~Player()
        {
            if (m_engine)
            {
                m_engine->Shutdown();
            }
        }

        // False if Media Foundation can't play the file.
        bool open(const std::wstring& path)
        {
            ComPtr<IMFMediaEngineClassFactory> factory;
            HRESULT hr = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
            ComPtr<IMFAttributes> attributes;
            if (SUCCEEDED(hr))
            {
                hr = MFCreateAttributes(&attributes, 2);
            }
            if (SUCCEEDED(hr))
            {
                m_events.Attach(new EngineEvents());
                attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, m_events.Get());
                // Frame server mode (no window of its own): frames are fetched with TransferVideoFrame.
                attributes->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);
                hr = factory->CreateInstance(0, attributes.Get(), &m_engine);
            }
            if (SUCCEEDED(hr))
            {
                BSTR url = SysAllocString(path.c_str());
                hr = m_engine->SetSource(url);
                SysFreeString(url);
            }
            if (SUCCEEDED(hr))
            {
                hr = m_engine->Play();
            }
            if (SUCCEEDED(hr))
            {
                hr = CoCreateInstance(kClsidWicImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_wic));
            }
            if (FAILED(hr))
            {
                LOG("Movie: Media Foundation can't play it ({:08x})", static_cast<uint32_t>(hr));
                return false;
            }
            return true;
        }

        bool finished() const
        {
            if (m_events->error.load() != S_OK)
            {
                static bool logged = false;
                if (!logged)
                {
                    logged = true;
                    LOG("Movie: playback error {:08x}", static_cast<uint32_t>(m_events->error.load()));
                }
                return true;
            }
            return m_events->ended.load();
        }

        // Fetches the frame due now, if there is a new one.
        bool update()
        {
            if (!m_frame && !createFrame())
            {
                return false;
            }
            LONGLONG pts = 0;
            if (m_engine->OnVideoStreamTick(&pts) != S_OK)
            {
                return false;
            }
            const RECT dst = {0, 0, static_cast<LONG>(m_videoW), static_cast<LONG>(m_videoH)};
            const MFARGB border = {0, 0, 0, 255};
            HRESULT hr = m_engine->TransferVideoFrame(m_bitmap.Get(), nullptr, &dst, &border);
            if (FAILED(hr))
            {
                logOnce("TransferVideoFrame", hr);
                return false;
            }
            ComPtr<IWICBitmapLock> lock;
            const WICRect all = {0, 0, static_cast<INT>(m_videoW), static_cast<INT>(m_videoH)};
            hr = m_bitmap->Lock(&all, WICBitmapLockRead, &lock);
            UINT stride = 0, size = 0;
            BYTE* pixels = nullptr;
            if (SUCCEEDED(hr))
            {
                lock->GetStride(&stride);
                hr = lock->GetDataPointer(&size, &pixels);
            }
            DDSURFACEDESC2 desc = {};
            desc.dwSize = sizeof(desc);
            if (SUCCEEDED(hr))
            {
                hr = m_frame->Lock(nullptr, &desc, DDLOCK_WAIT | DDLOCK_WRITEONLY, nullptr);
            }
            if (FAILED(hr))
            {
                logOnce("copying the frame", hr);
                return false;
            }
            for (UINT y = 0; y < m_videoH; ++y)
            {
                std::memcpy(static_cast<uint8_t*>(desc.lpSurface) + size_t(y) * desc.lPitch, pixels + size_t(y) * stride,
                    size_t(m_videoW) * 4);
            }
            m_frame->Unlock(nullptr);
            return true;
        }

        void draw()
        {
            const bool began = SUCCEEDED(m_device->BeginScene());
            D3DVIEWPORT7 vp = {0, 0, m_screenW, m_screenH, 0.0f, 1.0f};
            m_device->SetViewport(&vp);
            m_device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
            if (m_frame)
            {
                // Fit the picture (its display aspect ratio) into the screen, centered.
                DWORD ax = 0, ay = 0;
                float aspect = static_cast<float>(m_videoW) / static_cast<float>(m_videoH);
                if (SUCCEEDED(m_engine->GetVideoAspectRatio(&ax, &ay)) && ax && ay)
                {
                    aspect *= static_cast<float>(ax) / static_cast<float>(ay);
                }
                float w = static_cast<float>(m_screenW), h = w / aspect;
                if (h > static_cast<float>(m_screenH))
                {
                    h = static_cast<float>(m_screenH);
                    w = h * aspect;
                }
                const float x0 = (static_cast<float>(m_screenW) - w) * 0.5f, y0 = (static_cast<float>(m_screenH) - h) * 0.5f;
                TlVertex quad[4] = {
                    {x0, y0, 0.0f, 1.0f, 0.0f, 0.0f},
                    {x0 + w, y0, 0.0f, 1.0f, 1.0f, 0.0f},
                    {x0, y0 + h, 0.0f, 1.0f, 0.0f, 1.0f},
                    {x0 + w, y0 + h, 0.0f, 1.0f, 1.0f, 1.0f},
                };
                m_device->SetTexture(0, m_frame.Get());
                m_device->DrawPrimitive(D3DPT_TRIANGLESTRIP, kTlFvf, quad, 4, 0);
            }
            if (began)
            {
                m_device->EndScene();
            }
            // The game's own flip knows how to show the back buffer on this DirectDraw (windowed or flip chain).
            if (!FrameHooks::flip())
            {
                m_primary->Blt(nullptr, m_target, nullptr, DDBLT_WAIT, nullptr);
            }
        }

        void setupStates()
        {
            m_device->SetRenderState(D3DRENDERSTATE_ZENABLE, D3DZB_FALSE);
            m_device->SetRenderState(D3DRENDERSTATE_ZWRITEENABLE, FALSE);
            m_device->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, FALSE);
            m_device->SetRenderState(D3DRENDERSTATE_ALPHATESTENABLE, FALSE);
            m_device->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
            m_device->SetRenderState(D3DRENDERSTATE_FOGENABLE, FALSE);
            m_device->SetRenderState(D3DRENDERSTATE_STENCILENABLE, FALSE);
            m_device->SetRenderState(D3DRENDERSTATE_SPECULARENABLE, FALSE);
            m_device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            m_device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            m_device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            m_device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            m_device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
            m_device->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTFG_LINEAR);
            m_device->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTFN_LINEAR);
            m_device->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTFP_NONE);
            m_device->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
            m_device->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
            m_device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
            m_device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
            m_device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        }

    private:
        bool createFrame()
        {
            DWORD w = 0, h = 0;
            if (FAILED(m_engine->GetNativeVideoSize(&w, &h)) || !w || !h)
            {
                return false;   // not known before the metadata is loaded
            }
            HRESULT hr = m_wic->CreateBitmap(w, h, kWicPixelFormat32bppBGRA, WICBitmapCacheOnLoad, &m_bitmap);
            // A managed texture stays lockable on any DirectDraw; frames are BGRA, alpha unused.
            for (const DWORD alpha : {0u, 0xFF000000u})
            {
                if (FAILED(hr))
                {
                    break;
                }
                DDSURFACEDESC2 desc = {};
                desc.dwSize = sizeof(desc);
                desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
                desc.dwWidth = w;
                desc.dwHeight = h;
                desc.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
                desc.ddsCaps.dwCaps2 = DDSCAPS2_TEXTUREMANAGE;
                desc.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
                desc.ddpfPixelFormat.dwFlags = DDPF_RGB | (alpha ? DDPF_ALPHAPIXELS : 0);
                desc.ddpfPixelFormat.dwRGBBitCount = 32;
                desc.ddpfPixelFormat.dwRBitMask = 0xFF0000;
                desc.ddpfPixelFormat.dwGBitMask = 0xFF00;
                desc.ddpfPixelFormat.dwBBitMask = 0xFF;
                desc.ddpfPixelFormat.dwRGBAlphaBitMask = alpha;
                if (SUCCEEDED(m_ddraw->CreateSurface(&desc, &m_frame, nullptr)))
                {
                    break;
                }
                m_frame.Reset();
            }
            if (SUCCEEDED(hr) && !m_frame)
            {
                hr = DDERR_INVALIDPIXELFORMAT;
            }
            if (FAILED(hr))
            {
                logOnce("creating the frame texture", hr);
                m_frame.Reset();
                return false;
            }
            m_videoW = w;
            m_videoH = h;
            LOG("Movie: {}x{} video", w, h);
            return true;
        }

        static void logOnce(const char* what, HRESULT hr)
        {
            static bool logged = false;
            if (!logged)
            {
                logged = true;
                LOG("Movie: {} failed ({:08x})", what, static_cast<uint32_t>(hr));
            }
        }

        IDirect3DDevice7* m_device;
        IDirectDraw7* m_ddraw;
        IDirectDrawSurface7* m_primary;
        IDirectDrawSurface7* m_target;
        DWORD m_screenW = 0, m_screenH = 0;
        ComPtr<EngineEvents> m_events;
        ComPtr<IMFMediaEngine> m_engine;
        ComPtr<IWICImagingFactory> m_wic;
        ComPtr<IWICBitmap> m_bitmap;
        ComPtr<IDirectDrawSurface7> m_frame;
        DWORD m_videoW = 0, m_videoH = 0;
    };

    // Plays `file` through Media Foundation; -1 if skipped, like the game's loop.
    int play(void* self, IDirectDraw* ddraw1, IDirectDrawSurface* primary1, const MovieFile& file)
    {
        IDirect3DDevice7* device = DeviceProxy::instance();
        if (!device || !ddraw1 || !primary1)
        {
            return 0;
        }
        UiCanvas::Suspend physical;     // drawn in screen pixels, not into the 1024x768 UI canvas
        ComPtr<IDirectDraw7> ddraw;
        ComPtr<IDirectDrawSurface7> primary, target;
        if (FAILED(ddraw1->QueryInterface(IID_IDirectDraw7, &ddraw)) ||
            FAILED(primary1->QueryInterface(IID_IDirectDrawSurface7, &primary)) || FAILED(device->GetRenderTarget(&target)))
        {
            return 0;
        }
        HWND window = self ? *reinterpret_cast<HWND*>(static_cast<uint8_t*>(self) + 8) : nullptr;

        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
        bool skipped = false;
        {
            Player player(device, ddraw.Get(), primary.Get(), target.Get());
            if (mf && player.open(file.path()))
            {
                SavedState saved(device);
                player.setupStates();
                while (!player.finished())
                {
                    if (skipPressed(window))
                    {
                        skipped = true;
                        break;
                    }
                    if (!pumpMessages())
                    {
                        break;
                    }
                    const bool fresh = player.update();
                    player.draw();
                    if (!fresh && !(g_config.ddrawD3D9 && g_config.vsync))
                    {
                        Sleep(1);   // presenting doesn't wait for the display here
                    }
                }
            }
        }
        if (mf)
        {
            MFShutdown();
        }
        if (SUCCEEDED(com))
        {
            CoUninitialize();
        }

        if (skipped)
        {
            // As the game does: count the skip and wait for the key to be released, so it doesn't skip the next one.
            if (self)
            {
                ++*reinterpret_cast<int*>(static_cast<uint8_t*>(self) + 4);
            }
            while ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) || (GetAsyncKeyState(VK_SPACE) & 0x8000))
            {
                pumpMessages();
                Sleep(1);
            }
        }
        pumpMessages();
        LOG("Movie: {} {}", file.name(), skipped ? "skipped" : "finished");
        return skipped ? -1 : 0;
    }

    // The Media Engine exists from Windows 8 on (not on N editions without the Media Feature Pack).
    bool mediaEngineAvailable()
    {
        static const bool available = [] {
            const bool found = LoadLibraryExW(L"mfmediaengine.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) != nullptr;
            if (!found)
            {
                LOG("Movie: Media Foundation's Media Engine is not available");
            }
            return found;
        }();
        return available;
    }

    HRESULT __cdecl hookOpenStream(const char* path, IUnknown* ddraw, IUnknown** stream)
    {
        if (!g_config.mediaFoundation || !path || !stream)
        {
            return g_origOpenStream(path, ddraw, stream);
        }
        if (!mediaEngineAvailable())
        {
            if (!g_config.ddrawD3D9)
            {
                return g_origOpenStream(path, ddraw, stream);
            }
            *stream = nullptr;  // amstream can't decode into the Direct3D 9 backend's surfaces: no movie
            return E_FAIL;
        }
        // playVideo plays the file with Media Foundation instead of amstream.
        char full[MAX_PATH] = {};
        if (!GetFullPathNameA(path, MAX_PATH, full, nullptr))
        {
            *stream = nullptr;
            return E_FAIL;
        }
        wchar_t wide[MAX_PATH] = {};
        MultiByteToWideChar(CP_ACP, 0, full, -1, wide, MAX_PATH);
        *stream = new MovieFile(path, wide);
        return S_OK;
    }

    int __fastcall hookPlayVideo(void* self, void* edx, IDirectDraw* ddraw, IDirectDrawSurface* primary, IUnknown* stream,
        int w, int h)
    {
        if (MovieFile* file = asMovieFile(stream))
        {
            return play(self, ddraw, primary, *file);
        }
        UiCanvas::Scope ui;     // the game's 1024x768 quad goes into the UI canvas
        return g_origPlayVideo(self, edx, ddraw, primary, stream, w, h);
    }
}

void Movie::install()
{
    Patch::hook(g_origOpenStream, Addr::openMovieStream, &hookOpenStream, "openMovieStream");
    Patch::hook(g_origPlayVideo, Addr::playVideo, &hookPlayVideo, "playVideo");
}
