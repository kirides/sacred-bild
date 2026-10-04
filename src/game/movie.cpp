#include "game/movie.h"
#include "game/device_proxy.h"
#include "game/frame_hooks.h"
#include "game/sacred_addr.h"
#include "game/ui_canvas.h"
#include "config.h"
#include "log.h"
#include "patch.h"

#include <windows.h>
#include <amstream.h>
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
    // amstream's (DEFINE_GUIDs in amstream.h / mmstream.h, in no library this links against).
    constexpr GUID kClsidAMMultiMediaStream = {0x49c47ce5, 0x9ba4, 0x11d0, {0x82, 0x12, 0x00, 0xc0, 0x4f, 0xc3, 0x2c, 0x45}};
    constexpr GUID kMspidPrimaryVideo = {0xa35ff56a, 0x9fda, 0x11d0, {0x8f, 0xdf, 0x00, 0xc0, 0x4f, 0xd9, 0x18, 0x9d}};
    constexpr GUID kMspidPrimaryAudio = {0xa35ff56b, 0x9fda, 0x11d0, {0x8f, 0xdf, 0x00, 0xc0, 0x4f, 0xd9, 0x18, 0x9d}};
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

    void logOnce(const char* what, HRESULT hr)
    {
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            LOG("Movie: {} failed ({:08x})", what, static_cast<uint32_t>(hr));
        }
    }

    // Decodes a movie for the Player: 32-bit BGRX frames, and the sound on its own.
    class Source
    {
    public:
        virtual ~Source() = default;
        virtual bool finished() = 0;
        // False while the frame size isn't known yet.
        virtual bool videoSize(DWORD& w, DWORD& h) = 0;
        // Width / height of a pixel on screen.
        virtual float pixelAspect() { return 1.0f; }
        // Decodes the frame due now; false if there is no new one.
        virtual bool next() = 0;
        // Copies the frame next() decoded into `dst` (videoSize, 4 bytes per pixel).
        virtual bool copy(uint8_t* dst, LONG pitch) = 0;
    };

    // Media Foundation's Media Engine in frame server mode (no window of its own): frames are fetched with
    // TransferVideoFrame.
    class MediaEngineSource final : public Source
    {
    public:
        ~MediaEngineSource() override
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

        bool finished() override
        {
            if (m_failed)
            {
                return true;
            }
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

        bool videoSize(DWORD& w, DWORD& h) override
        {
            return SUCCEEDED(m_engine->GetNativeVideoSize(&w, &h)) && w && h;     // known once the metadata is loaded
        }

        float pixelAspect() override
        {
            DWORD ax = 0, ay = 0;
            if (SUCCEEDED(m_engine->GetVideoAspectRatio(&ax, &ay)) && ax && ay)
            {
                return static_cast<float>(ax) / static_cast<float>(ay);
            }
            return 1.0f;
        }

        bool next() override
        {
            if (!m_bitmap)
            {
                HRESULT hr = videoSize(m_w, m_h) ? S_OK : E_PENDING;
                if (SUCCEEDED(hr))
                {
                    hr = m_wic->CreateBitmap(m_w, m_h, kWicPixelFormat32bppBGRA, WICBitmapCacheOnLoad, &m_bitmap);
                }
                if (FAILED(hr))
                {
                    return false;
                }
            }
            LONGLONG pts = 0;
            if (m_engine->OnVideoStreamTick(&pts) != S_OK)
            {
                return false;
            }
            const RECT dst = {0, 0, static_cast<LONG>(m_w), static_cast<LONG>(m_h)};
            const MFARGB border = {0, 0, 0, 255};
            const HRESULT hr = m_engine->TransferVideoFrame(m_bitmap.Get(), nullptr, &dst, &border);
            if (FAILED(hr))
            {
                logOnce("TransferVideoFrame", hr);
                m_failed = !m_transferred;     // never worked (e.g. Wine): let the fallback play it
                return false;
            }
            m_transferred = true;
            return true;
        }

        bool copy(uint8_t* dst, LONG pitch) override
        {
            ComPtr<IWICBitmapLock> lock;
            const WICRect all = {0, 0, static_cast<INT>(m_w), static_cast<INT>(m_h)};
            HRESULT hr = m_bitmap->Lock(&all, WICBitmapLockRead, &lock);
            UINT stride = 0, size = 0;
            BYTE* pixels = nullptr;
            if (SUCCEEDED(hr))
            {
                lock->GetStride(&stride);
                hr = lock->GetDataPointer(&size, &pixels);
            }
            if (FAILED(hr))
            {
                logOnce("reading the frame", hr);
                return false;
            }
            for (UINT y = 0; y < m_h; ++y)
            {
                std::memcpy(dst + size_t(y) * pitch, pixels + size_t(y) * stride, size_t(m_w) * 4);
            }
            return true;
        }

    private:
        ComPtr<EngineEvents> m_events;
        ComPtr<IMFMediaEngine> m_engine;
        ComPtr<IWICImagingFactory> m_wic;
        ComPtr<IWICBitmap> m_bitmap;
        DWORD m_w = 0, m_h = 0;
        bool m_transferred = false;
        bool m_failed = false;
    };

    // DirectShow's multimedia streams (amstream, as the game uses them), decoding into a system memory surface of
    // Windows' own DirectDraw: amstream doesn't work on the Direct3D 9 backend's. It plays the sound itself. For
    // systems without the Media Engine (Windows 7, N editions) or where it can't play the file.
    class StreamSource final : public Source
    {
    public:
        ~StreamSource() override
        {
            if (m_stream)
            {
                m_stream->SetState(STREAMSTATE_STOP);
            }
            m_sample.Reset();
            m_video.Reset();
            m_stream.Reset();
            m_surface.Reset();
            m_ddraw.Reset();
        }

        // False if DirectShow can't play the file.
        bool open(const std::wstring& path)
        {
            HRESULT hr = createDirectDraw();
            if (SUCCEEDED(hr))
            {
                hr = CoCreateInstance(kClsidAMMultiMediaStream, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_stream));
            }
            if (SUCCEEDED(hr))
            {
                hr = m_stream->Initialize(STREAMTYPE_READ, 0, nullptr);
            }
            if (SUCCEEDED(hr))
            {
                hr = m_stream->AddMediaStream(m_ddraw.Get(), &kMspidPrimaryVideo, 0, nullptr);
            }
            if (SUCCEEDED(hr) && FAILED(m_stream->AddMediaStream(nullptr, &kMspidPrimaryAudio, AMMSF_ADDDEFAULTRENDERER, nullptr)))
            {
                LOG("Movie: no audio renderer, playing without sound");
            }
            if (SUCCEEDED(hr))
            {
                hr = m_stream->OpenFile(path.c_str(), 0);
            }
            ComPtr<IMediaStream> media;
            if (SUCCEEDED(hr))
            {
                hr = m_stream->GetMediaStream(kMspidPrimaryVideo, &media);
            }
            if (SUCCEEDED(hr))
            {
                hr = media.As(&m_video);
            }
            if (SUCCEEDED(hr))
            {
                hr = createSample();
            }
            if (SUCCEEDED(hr))
            {
                hr = m_stream->SetState(STREAMSTATE_RUN);
            }
            if (FAILED(hr))
            {
                LOG("Movie: DirectShow can't play it ({:08x})", static_cast<uint32_t>(hr));
                return false;
            }
            return true;
        }

        bool finished() override
        {
            return m_ended;
        }

        bool videoSize(DWORD& w, DWORD& h) override
        {
            w = m_w;
            h = m_h;
            return true;
        }

        // Waits for the next frame, as the game's own loop does.
        bool next() override
        {
            if (m_ended)
            {
                return false;
            }
            const HRESULT hr = m_sample->Update(0, nullptr, nullptr, 0);
            if (hr != S_OK)
            {
                if (FAILED(hr))
                {
                    LOG("Movie: DirectShow playback error {:08x}", static_cast<uint32_t>(hr));
                }
                m_ended = true;     // MS_S_ENDOFSTREAM, or an error
                return false;
            }
            return true;
        }

        bool copy(uint8_t* dst, LONG pitch) override
        {
            DDSURFACEDESC desc = {};
            desc.dwSize = sizeof(desc);
            const HRESULT hr = m_surface->Lock(nullptr, &desc, DDLOCK_WAIT | DDLOCK_READONLY, nullptr);
            if (FAILED(hr))
            {
                logOnce("reading the frame", hr);
                return false;
            }
            for (UINT y = 0; y < m_h; ++y)
            {
                std::memcpy(dst + size_t(y) * pitch, static_cast<const uint8_t*>(desc.lpSurface) + size_t(y) * desc.lPitch,
                    size_t(m_w) * 4);
            }
            m_surface->Unlock(nullptr);
            return true;
        }

    private:
        using DirectDrawCreateFn = HRESULT(WINAPI*)(GUID*, IDirectDraw**, IUnknown*);

        HRESULT createDirectDraw()
        {
            wchar_t dir[MAX_PATH] = {};
            GetSystemDirectoryW(dir, MAX_PATH);
            HMODULE module = LoadLibraryW((std::wstring(dir) + L"\\ddraw.dll").c_str());
            auto create = module ? reinterpret_cast<DirectDrawCreateFn>(GetProcAddress(module, "DirectDrawCreate")) : nullptr;
            if (!create)
            {
                return E_NOINTERFACE;
            }
            HRESULT hr = create(nullptr, &m_ddraw, nullptr);
            if (SUCCEEDED(hr))
            {
                hr = m_ddraw->SetCooperativeLevel(nullptr, DDSCL_NORMAL);   // no primary surface, no window
            }
            return hr;
        }

        // A 32-bit RGB system memory surface of the video's size, as the stream's sample.
        HRESULT createSample()
        {
            DDSURFACEDESC native = {};
            native.dwSize = sizeof(native);
            HRESULT hr = m_video->GetFormat(&native, nullptr, nullptr, nullptr);
            if (FAILED(hr))
            {
                return hr;
            }
            m_w = native.dwWidth;
            m_h = native.dwHeight;
            DDSURFACEDESC desc = {};
            desc.dwSize = sizeof(desc);
            desc.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
            desc.dwWidth = m_w;
            desc.dwHeight = m_h;
            desc.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
            desc.ddpfPixelFormat.dwFlags = DDPF_RGB;
            desc.ddpfPixelFormat.dwRGBBitCount = 32;
            desc.ddpfPixelFormat.dwRBitMask = 0xFF0000;
            desc.ddpfPixelFormat.dwGBitMask = 0xFF00;
            desc.ddpfPixelFormat.dwBBitMask = 0xFF;
            hr = m_video->SetFormat(&desc, nullptr);
            if (SUCCEEDED(hr))
            {
                desc.dwFlags |= DDSD_CAPS;
                desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
                hr = m_ddraw->CreateSurface(&desc, &m_surface, nullptr);
            }
            if (SUCCEEDED(hr))
            {
                hr = m_video->CreateSample(m_surface.Get(), nullptr, 0, &m_sample);
            }
            return hr;
        }

        ComPtr<IDirectDraw> m_ddraw;
        ComPtr<IAMMultiMediaStream> m_stream;
        ComPtr<IDirectDrawMediaStream> m_video;
        ComPtr<IDirectDrawSurface> m_surface;
        ComPtr<IDirectDrawStreamSample> m_sample;
        DWORD m_w = 0, m_h = 0;
        bool m_ended = false;
    };

    struct Outcome
    {
        bool skipped = false;
        bool quit = false;      // WM_QUIT arrived
        int frames = 0;
    };

    // Shows a Source's frames on the game's device until it ends or the player skips.
    class Player
    {
    public:
        Player(IDirect3DDevice7* device, IDirectDraw7* ddraw, IDirectDrawSurface7* primary, IDirectDrawSurface7* target,
            Source& source)
            : m_device(device), m_ddraw(ddraw), m_primary(primary), m_target(target), m_source(source)
        {
            DDSURFACEDESC2 desc = {};
            desc.dwSize = sizeof(desc);
            target->GetSurfaceDesc(&desc);
            m_screenW = desc.dwWidth;
            m_screenH = desc.dwHeight;
        }

        Outcome run(HWND window)
        {
            Outcome outcome;
            SavedState saved(m_device);
            setupStates();
            while (!m_source.finished())
            {
                if (skipPressed(window))
                {
                    outcome.skipped = true;
                    break;
                }
                if (!pumpMessages())
                {
                    outcome.quit = true;
                    break;
                }
                const bool fresh = update();
                outcome.frames += fresh ? 1 : 0;
                draw();
                if (!fresh && !(g_config.ddrawD3D9 && g_config.vsync))
                {
                    Sleep(1);   // presenting doesn't wait for the display here
                }
            }
            return outcome;
        }

    private:
        // Puts the frame due now into the texture, if there is a new one.
        bool update()
        {
            if (!m_frame && !createFrame())
            {
                return false;
            }
            if (!m_source.next())
            {
                return false;
            }
            DDSURFACEDESC2 desc = {};
            desc.dwSize = sizeof(desc);
            const HRESULT hr = m_frame->Lock(nullptr, &desc, DDLOCK_WAIT | DDLOCK_WRITEONLY, nullptr);
            if (FAILED(hr))
            {
                logOnce("copying the frame", hr);
                return false;
            }
            const bool copied = m_source.copy(static_cast<uint8_t*>(desc.lpSurface), desc.lPitch);
            m_frame->Unlock(nullptr);
            return copied;
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
                const float aspect = static_cast<float>(m_videoW) / static_cast<float>(m_videoH) * m_source.pixelAspect();
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

        bool createFrame()
        {
            DWORD w = 0, h = 0;
            if (!m_source.videoSize(w, h))
            {
                return false;
            }
            // A managed texture stays lockable on any DirectDraw; frames are BGRX, alpha unused.
            HRESULT hr = DDERR_INVALIDPIXELFORMAT;
            for (const DWORD alpha : {0u, 0xFF000000u})
            {
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
                hr = m_ddraw->CreateSurface(&desc, &m_frame, nullptr);
                if (SUCCEEDED(hr))
                {
                    break;
                }
                m_frame.Reset();
            }
            if (FAILED(hr))
            {
                logOnce("creating the frame texture", hr);
                return false;
            }
            m_videoW = w;
            m_videoH = h;
            LOG("Movie: {}x{} video", w, h);
            return true;
        }

        IDirect3DDevice7* m_device;
        IDirectDraw7* m_ddraw;
        IDirectDrawSurface7* m_primary;
        IDirectDrawSurface7* m_target;
        Source& m_source;
        DWORD m_screenW = 0, m_screenH = 0;
        ComPtr<IDirectDrawSurface7> m_frame;
        DWORD m_videoW = 0, m_videoH = 0;
    };

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

    // Plays `file` through Media Foundation, or through DirectShow if the Media Engine isn't there, can't open the
    // file or never delivers a frame ([Debug] MovieFallback: always); -1 if skipped, like the game's loop.
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
        Outcome outcome;
        bool done = false;
        if (!g_config.movieFallback && mediaEngineAvailable() && SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
        {
            {
                MediaEngineSource source;
                if (source.open(file.path()))
                {
                    LOG("Movie: {} through Media Foundation", file.name());
                    Player player(device, ddraw.Get(), primary.Get(), target.Get(), source);
                    outcome = player.run(window);
                    done = outcome.frames > 0 || outcome.skipped || outcome.quit;
                }
            }
            MFShutdown();
        }
        if (!done)
        {
            StreamSource source;
            if (source.open(file.path()))
            {
                LOG("Movie: {} through DirectShow (system memory surface)", file.name());
                Player player(device, ddraw.Get(), primary.Get(), target.Get(), source);
                outcome = player.run(window);
            }
        }
        if (SUCCEEDED(com))
        {
            CoUninitialize();
        }

        if (outcome.skipped)
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
        LOG("Movie: {} {}", file.name(), outcome.skipped ? "skipped" : "finished");
        return outcome.skipped ? -1 : 0;
    }

    HRESULT __cdecl hookOpenStream(const char* path, IUnknown* ddraw, IUnknown** stream)
    {
        if (!g_config.mediaFoundation || !path || !stream)
        {
            return g_origOpenStream(path, ddraw, stream);
        }
        if (!g_config.ddrawD3D9 && !g_config.movieFallback && !mediaEngineAvailable())
        {
            return g_origOpenStream(path, ddraw, stream);   // the game's own amstream player works on the chained ddraw
        }
        // playVideo plays the file itself (Media Foundation, or the DirectShow fallback).
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
