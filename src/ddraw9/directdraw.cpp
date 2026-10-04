#include "ddraw9/directdraw.h"
#include "ddraw9/backend.h"
#include "ddraw9/device.h"
#include "ddraw9/format.h"
#include "ddraw9/gpu.h"
#include "ddraw9/surface.h"
#include "ddraw9/vertex_buffer.h"
#include "log.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <tuple>
#include <vector>

namespace DDraw9
{
    static_assert(offsetof(DDSURFACEDESC, ddsCaps) == offsetof(DDSURFACEDESC2, ddsCaps));
    static_assert(offsetof(DDSURFACEDESC, ddpfPixelFormat) == offsetof(DDSURFACEDESC2, ddpfPixelFormat));

    void toDesc2(const DDSURFACEDESC& in, DDSURFACEDESC2& out)
    {
        out = {};
        std::memcpy(&out, &in, offsetof(DDSURFACEDESC, ddsCaps));
        out.dwSize = sizeof(DDSURFACEDESC2);
        out.ddsCaps.dwCaps = in.ddsCaps.dwCaps;
    }

    void toDesc1(const DDSURFACEDESC2& in, DDSURFACEDESC& out)
    {
        out = {};
        std::memcpy(&out, &in, offsetof(DDSURFACEDESC, ddsCaps));
        out.dwSize = sizeof(DDSURFACEDESC);
        out.ddsCaps.dwCaps = in.ddsCaps.dwCaps;
    }

    namespace
    {
        struct Mode
        {
            UINT width, height, bpp, refresh;
            auto key() const { return std::tie(bpp, width, height, refresh); }
            bool operator<(const Mode& o) const { return key() < o.key(); }
            bool operator==(const Mode& o) const { return key() == o.key(); }
        };

        void describeMode(const Mode& mode, DDSURFACEDESC2& desc)
        {
            desc = {};
            desc.dwSize = sizeof(desc);
            desc.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
            desc.dwWidth = mode.width;
            desc.dwHeight = mode.height;
            desc.lPitch = static_cast<LONG>(mode.width * mode.bpp / 8);
            desc.dwRefreshRate = mode.refresh;
            desc.ddpfPixelFormat = Format::toPixelFormat(mode.bpp == 16 ? d9::D3DFMT_R5G6B5 : d9::D3DFMT_X8R8G8B8);
        }

        void fillCaps(DDCAPS& caps)
        {
            caps = {};
            caps.dwSize = sizeof(caps);
            caps.dwCaps = DDCAPS_3D | DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_BLTDEPTHFILL | DDCAPS_BLTSTRETCH |
                DDCAPS_CANBLTSYSMEM | DDCAPS_CANCLIP | DDCAPS_CANCLIPSTRETCHED | DDCAPS_COLORKEY | DDCAPS_GDI;
            caps.dwCaps2 = DDCAPS2_CANRENDERWINDOWED | DDCAPS2_WIDESURFACES | DDCAPS2_NOPAGELOCKREQUIRED |
                DDCAPS2_FLIPNOVSYNC | DDCAPS2_FLIPINTERVAL | DDCAPS2_CANMANAGETEXTURE;
            caps.dwCKeyCaps = DDCKEYCAPS_SRCBLT;
            caps.dwZBufferBitDepths = DDBD_16 | DDBD_24 | DDBD_32;
            caps.ddsCaps.dwCaps = DDSCAPS_3DDEVICE | DDSCAPS_BACKBUFFER | DDSCAPS_COMPLEX | DDSCAPS_FLIP |
                DDSCAPS_FRONTBUFFER | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_PRIMARYSURFACE | DDSCAPS_SYSTEMMEMORY |
                DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM | DDSCAPS_ZBUFFER;
            caps.ddsOldCaps.dwCaps = caps.ddsCaps.dwCaps;
            DWORD memory = 0xFFF00000;
            if (d9::IDirect3DDevice9Ex* dev = Gpu::existingDevice())
            {
                memory = dev->GetAvailableTextureMem();
            }
            caps.dwVidMemTotal = memory;
            caps.dwVidMemFree = memory;
        }

        // Display modes for 16 and 32 bits per pixel; the desktop mode and 1024x768 are always included.
        std::vector<Mode> displayModes(bool refreshRates)
        {
            std::vector<Mode> modes;
            const d9::D3DDISPLAYMODE desktop = Gpu::displayMode();
            if (d9::IDirect3D9Ex* d3d = Gpu::d3d())
            {
                for (auto [format, bpp] : {std::pair{d9::D3DFMT_R5G6B5, 16u}, std::pair{d9::D3DFMT_X8R8G8B8, 32u}})
                {
                    const UINT count = d3d->GetAdapterModeCount(Gpu::adapter(), format);
                    for (UINT i = 0; i < count; ++i)
                    {
                        d9::D3DDISPLAYMODE mode = {};
                        if (SUCCEEDED(d3d->EnumAdapterModes(Gpu::adapter(), format, i, &mode)))
                        {
                            modes.push_back({mode.Width, mode.Height, bpp, refreshRates ? mode.RefreshRate : 0});
                        }
                    }
                }
            }
            for (UINT bpp : {16u, 32u})
            {
                modes.push_back({desktop.Width, desktop.Height, bpp, refreshRates ? desktop.RefreshRate : 0});
                modes.push_back({1024, 768, bpp, refreshRates ? 60u : 0});
            }
            std::sort(modes.begin(), modes.end());
            modes.erase(std::unique(modes.begin(), modes.end()), modes.end());
            return modes;
        }

        bool matches(const Mode& mode, const DDSURFACEDESC2* filter)
        {
            if (!filter)
            {
                return true;
            }
            if ((filter->dwFlags & DDSD_WIDTH) && filter->dwWidth != mode.width)
            {
                return false;
            }
            if ((filter->dwFlags & DDSD_HEIGHT) && filter->dwHeight != mode.height)
            {
                return false;
            }
            if ((filter->dwFlags & DDSD_PIXELFORMAT) && filter->ddpfPixelFormat.dwRGBBitCount != mode.bpp)
            {
                return false;
            }
            if ((filter->dwFlags & DDSD_REFRESHRATE) && filter->dwRefreshRate && filter->dwRefreshRate != mode.refresh)
            {
                return false;
            }
            return true;
        }

        // Version 1 callbacks on top of the version 7 enumeration.
        struct ModeCallback1
        {
            LPDDENUMMODESCALLBACK callback;
            LPVOID context;

            static HRESULT CALLBACK thunk(LPDDSURFACEDESC2 desc, LPVOID self)
            {
                auto* me = static_cast<ModeCallback1*>(self);
                DDSURFACEDESC desc1;
                toDesc1(*desc, desc1);
                return me->callback(&desc1, me->context);
            }
        };
    }

    // ------------------------------------------------------------------------------------------------------------
    // DirectDraw

    DirectDraw* DirectDraw::create()
    {
        return new DirectDraw();
    }

    void DirectDraw::displayMode(DDSURFACEDESC2& desc) const
    {
        const d9::D3DDISPLAYMODE desktop = Gpu::displayMode();
        Mode mode = {desktop.Width, desktop.Height, 32, desktop.RefreshRate};
        if (m_modeWidth && m_modeHeight)
        {
            mode = {m_modeWidth, m_modeHeight, m_modeBpp ? m_modeBpp : 32, desktop.RefreshRate};
        }
        describeMode(mode, desc);
    }

    HRESULT DirectDraw::QueryInterface(REFIID riid, LPVOID* out)
    {
        if (!out)
        {
            return E_POINTER;
        }
        *out = nullptr;
        if (riid == IID_IUnknown || riid == IID_IDirectDraw7)
        {
            *out = static_cast<IDirectDraw7*>(this);
        }
        else if (riid == IID_IDirect3D7)
        {
            *out = static_cast<IDirect3D7*>(this);
        }
        else if (riid == IID_IDirectDraw)
        {
            *out = &m_v1;
        }
        else
        {
            unsupported("IDirectDraw7::QueryInterface for an interface other than IDirectDraw7, IDirect3D7, IDirectDraw");
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    ULONG DirectDraw::AddRef()
    {
        return static_cast<ULONG>(InterlockedIncrement(&m_refs));
    }

    ULONG DirectDraw::Release()
    {
        const LONG refs = InterlockedDecrement(&m_refs);
        if (refs == 0)
        {
            delete this;
        }
        return static_cast<ULONG>(refs);
    }

    HRESULT DirectDraw::Compact()
    {
        return DD_OK;
    }

    HRESULT DirectDraw::CreateClipper(DWORD, LPDIRECTDRAWCLIPPER* out, IUnknown* outer)
    {
        if (!out)
        {
            return DDERR_INVALIDPARAMS;
        }
        if (outer)
        {
            return CLASS_E_NOAGGREGATION;
        }
        *out = Clipper::create();
        return DD_OK;
    }

    HRESULT DirectDraw::CreatePalette(DWORD, LPPALETTEENTRY, LPDIRECTDRAWPALETTE*, IUnknown*)
    {
        unsupported("IDirectDraw7::CreatePalette");
        return DDERR_UNSUPPORTED;
    }

    HRESULT DirectDraw::CreateSurface(LPDDSURFACEDESC2 desc, LPDIRECTDRAWSURFACE7* out, IUnknown* outer)
    {
        if (!desc || !out || desc->dwSize != sizeof(DDSURFACEDESC2))
        {
            return DDERR_INVALIDPARAMS;
        }
        if (outer)
        {
            return CLASS_E_NOAGGREGATION;
        }
        *out = nullptr;
        Surface* surface = nullptr;
        const HRESULT hr = Surface::create(this, *desc, &surface);
        if (SUCCEEDED(hr))
        {
            *out = surface;
        }
        return hr;
    }

    HRESULT DirectDraw::DuplicateSurface(LPDIRECTDRAWSURFACE7, LPDIRECTDRAWSURFACE7*)
    {
        unsupported("IDirectDraw7::DuplicateSurface");
        return DDERR_UNSUPPORTED;
    }

    HRESULT DirectDraw::EnumDisplayModes(DWORD flags, LPDDSURFACEDESC2 filter, LPVOID context, LPDDENUMMODESCALLBACK2 callback)
    {
        if (!callback)
        {
            return DDERR_INVALIDPARAMS;
        }
        for (const Mode& mode : displayModes((flags & DDEDM_REFRESHRATES) != 0))
        {
            if (!matches(mode, filter))
            {
                continue;
            }
            DDSURFACEDESC2 desc;
            describeMode(mode, desc);
            if (callback(&desc, context) == DDENUMRET_CANCEL)
            {
                break;
            }
        }
        return DD_OK;
    }

    HRESULT DirectDraw::EnumSurfaces(DWORD, LPDDSURFACEDESC2, LPVOID, LPDDENUMSURFACESCALLBACK7)
    {
        unsupported("IDirectDraw7::EnumSurfaces");
        return DDERR_UNSUPPORTED;
    }

    HRESULT DirectDraw::FlipToGDISurface()
    {
        return DD_OK;
    }

    HRESULT DirectDraw::GetCaps(LPDDCAPS driverCaps, LPDDCAPS helCaps)
    {
        if (!driverCaps && !helCaps)
        {
            return DDERR_INVALIDPARAMS;
        }
        DDCAPS caps;
        fillCaps(caps);
        for (LPDDCAPS out : {driverCaps, helCaps})
        {
            if (out)
            {
                const DWORD size = std::min<DWORD>(out->dwSize ? out->dwSize : sizeof(caps), sizeof(caps));
                std::memcpy(out, &caps, size);
                out->dwSize = size;
            }
        }
        return DD_OK;
    }

    HRESULT DirectDraw::GetDisplayMode(LPDDSURFACEDESC2 desc)
    {
        if (!desc)
        {
            return DDERR_INVALIDPARAMS;
        }
        displayMode(*desc);
        return DD_OK;
    }

    HRESULT DirectDraw::GetFourCCCodes(LPDWORD count, LPDWORD)
    {
        if (!count)
        {
            return DDERR_INVALIDPARAMS;
        }
        *count = 0;
        return DD_OK;
    }

    HRESULT DirectDraw::GetGDISurface(LPDIRECTDRAWSURFACE7*)
    {
        unsupported("IDirectDraw7::GetGDISurface");
        return DDERR_NOTFOUND;
    }

    HRESULT DirectDraw::GetMonitorFrequency(LPDWORD frequency)
    {
        if (!frequency)
        {
            return DDERR_INVALIDPARAMS;
        }
        *frequency = Gpu::displayMode().RefreshRate;
        return DD_OK;
    }

    HRESULT DirectDraw::GetScanLine(LPDWORD line)
    {
        if (!line)
        {
            return DDERR_INVALIDPARAMS;
        }
        *line = 0;
        return DD_OK;
    }

    HRESULT DirectDraw::GetVerticalBlankStatus(LPBOOL inVerticalBlank)
    {
        if (!inVerticalBlank)
        {
            return DDERR_INVALIDPARAMS;
        }
        *inVerticalBlank = FALSE;
        return DD_OK;
    }

    HRESULT DirectDraw::Initialize(GUID*)
    {
        return DDERR_ALREADYINITIALIZED;
    }

    HRESULT DirectDraw::RestoreDisplayMode()
    {
        m_modeWidth = m_modeHeight = m_modeBpp = 0;
        return DD_OK;
    }

    HRESULT DirectDraw::SetCooperativeLevel(HWND window, DWORD flags)
    {
        m_window = window;
        m_cooperativeFlags = flags;
        Gpu::setWindow(window, (flags & (DDSCL_FPUSETUP | DDSCL_FPUPRESERVE)) != 0);
        return DD_OK;
    }

    HRESULT DirectDraw::SetDisplayMode(DWORD width, DWORD height, DWORD bpp, DWORD, DWORD)
    {
        // Fullscreen modes are emulated in the window: the display keeps its mode.
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            LOG("Direct3D 9: display mode {}x{}x{} requested, the game runs in its window", width, height, bpp);
        }
        m_modeWidth = width;
        m_modeHeight = height;
        m_modeBpp = bpp;
        return DD_OK;
    }

    HRESULT DirectDraw::WaitForVerticalBlank(DWORD, HANDLE)
    {
        // The game waits here before every back buffer lock and before flips; presentation paces frames instead.
        return DD_OK;
    }

    HRESULT DirectDraw::GetAvailableVidMem(LPDDSCAPS2, LPDWORD total, LPDWORD free)
    {
        DDCAPS caps;
        fillCaps(caps);
        if (total)
        {
            *total = caps.dwVidMemTotal;
        }
        if (free)
        {
            *free = caps.dwVidMemFree;
        }
        return DD_OK;
    }

    HRESULT DirectDraw::GetSurfaceFromDC(HDC, LPDIRECTDRAWSURFACE7*)
    {
        unsupported("IDirectDraw7::GetSurfaceFromDC");
        return DDERR_NOTFOUND;
    }

    HRESULT DirectDraw::RestoreAllSurfaces()
    {
        return DD_OK;
    }

    HRESULT DirectDraw::TestCooperativeLevel()
    {
        return DD_OK;
    }

    HRESULT DirectDraw::GetDeviceIdentifier(LPDDDEVICEIDENTIFIER2 id, DWORD)
    {
        if (!id)
        {
            return DDERR_INVALIDPARAMS;
        }
        *id = {};
        d9::D3DADAPTER_IDENTIFIER9 adapter = {};
        if (Gpu::d3d() && SUCCEEDED(Gpu::d3d()->GetAdapterIdentifier(Gpu::adapter(), 0, &adapter)))
        {
            strncpy_s(id->szDriver, adapter.Driver, _TRUNCATE);
            strncpy_s(id->szDescription, adapter.Description, _TRUNCATE);
            id->liDriverVersion = adapter.DriverVersion;
            id->dwVendorId = adapter.VendorId;
            id->dwDeviceId = adapter.DeviceId;
            id->dwSubSysId = adapter.SubSysId;
            id->dwRevision = adapter.Revision;
            id->guidDeviceIdentifier = adapter.DeviceIdentifier;
            id->dwWHQLLevel = adapter.WHQLLevel;
        }
        return DD_OK;
    }

    HRESULT DirectDraw::StartModeTest(LPSIZE, DWORD, DWORD)
    {
        unsupported("IDirectDraw7::StartModeTest");
        return DDERR_UNSUPPORTED;
    }

    HRESULT DirectDraw::EvaluateMode(DWORD, DWORD*)
    {
        unsupported("IDirectDraw7::EvaluateMode");
        return DDERR_UNSUPPORTED;
    }

    HRESULT DirectDraw::EnumDevices(LPD3DENUMDEVICESCALLBACK7 callback, LPVOID context)
    {
        if (!callback)
        {
            return DDERR_INVALIDPARAMS;
        }
        struct Entry
        {
            const GUID* type;
            char description[96];
            char name[32];
        };
        Entry devices[] = {
            {&IID_IDirect3DRGBDevice, "Microsoft Direct3D RGB Software Emulation", "RGB Emulation"},
            {&IID_IDirect3DHALDevice, "Microsoft Direct3D Hardware acceleration through Direct3D HAL", "Direct3D HAL"},
            {&IID_IDirect3DTnLHalDevice, "Microsoft Direct3D Hardware Transform and Lighting acceleration capable device",
                "Direct3D T&L HAL"},
        };
        for (Entry& e : devices)
        {
            D3DDEVICEDESC7 desc = deviceDescription(*e.type);
            if (callback(e.description, e.name, &desc, context) == D3DENUMRET_CANCEL)
            {
                break;
            }
        }
        return D3D_OK;
    }

    HRESULT DirectDraw::CreateDevice(REFCLSID type, LPDIRECTDRAWSURFACE7 target, LPDIRECT3DDEVICE7* out)
    {
        if (!out)
        {
            return DDERR_INVALIDPARAMS;
        }
        *out = nullptr;
        Surface* surface = Surface::from(target);
        if (!surface)
        {
            return DDERR_INVALIDPARAMS;
        }
        Device* device = nullptr;
        const HRESULT hr = Device::create(this, type, surface, &device);
        if (SUCCEEDED(hr))
        {
            *out = device;
        }
        return hr;
    }

    HRESULT DirectDraw::CreateVertexBuffer(LPD3DVERTEXBUFFERDESC desc, LPDIRECT3DVERTEXBUFFER7* out, DWORD)
    {
        if (!desc || !out)
        {
            return DDERR_INVALIDPARAMS;
        }
        *out = nullptr;
        VertexBuffer* vb = nullptr;
        const HRESULT hr = VertexBuffer::create(*desc, &vb);
        if (SUCCEEDED(hr))
        {
            *out = vb;
        }
        return hr;
    }

    HRESULT DirectDraw::EnumZBufferFormats(REFCLSID, LPD3DENUMPIXELFORMATSCALLBACK callback, LPVOID context)
    {
        if (!callback)
        {
            return DDERR_INVALIDPARAMS;
        }
        if (!Gpu::available())
        {
            return DDERR_GENERIC;
        }
        for (d9::D3DFORMAT format : {d9::D3DFMT_D16, d9::D3DFMT_D24X8, d9::D3DFMT_D24S8, d9::D3DFMT_D32})
        {
            if (!Gpu::depthFormat(format))
            {
                continue;
            }
            DDPIXELFORMAT pf = Format::toPixelFormat(format);
            if (callback(&pf, context) == D3DENUMRET_CANCEL)
            {
                break;
            }
        }
        return D3D_OK;
    }

    HRESULT DirectDraw::EvictManagedTextures()
    {
        return D3D_OK;
    }

    // ------------------------------------------------------------------------------------------------------------
    // DirectDraw1

    HRESULT DirectDraw1::QueryInterface(REFIID riid, LPVOID* out)
    {
        return m_owner.QueryInterface(riid, out);
    }

    ULONG DirectDraw1::AddRef()
    {
        return m_owner.AddRef();
    }

    ULONG DirectDraw1::Release()
    {
        return m_owner.Release();
    }

    HRESULT DirectDraw1::Compact()
    {
        return m_owner.Compact();
    }

    HRESULT DirectDraw1::CreateClipper(DWORD flags, LPDIRECTDRAWCLIPPER* out, IUnknown* outer)
    {
        return m_owner.CreateClipper(flags, out, outer);
    }

    HRESULT DirectDraw1::CreatePalette(DWORD flags, LPPALETTEENTRY entries, LPDIRECTDRAWPALETTE* out, IUnknown* outer)
    {
        return m_owner.CreatePalette(flags, entries, out, outer);
    }

    HRESULT DirectDraw1::CreateSurface(LPDDSURFACEDESC desc, LPDIRECTDRAWSURFACE* out, IUnknown* outer)
    {
        if (!desc || !out || desc->dwSize != sizeof(DDSURFACEDESC))
        {
            return DDERR_INVALIDPARAMS;
        }
        *out = nullptr;
        DDSURFACEDESC2 desc2;
        toDesc2(*desc, desc2);
        LPDIRECTDRAWSURFACE7 surface = nullptr;
        const HRESULT hr = m_owner.CreateSurface(&desc2, &surface, outer);
        if (SUCCEEDED(hr))
        {
            *out = Surface::from(surface)->v1();
        }
        return hr;
    }

    HRESULT DirectDraw1::DuplicateSurface(LPDIRECTDRAWSURFACE, LPDIRECTDRAWSURFACE*)
    {
        unsupported("IDirectDraw::DuplicateSurface");
        return DDERR_UNSUPPORTED;
    }

    HRESULT DirectDraw1::EnumDisplayModes(DWORD flags, LPDDSURFACEDESC desc, LPVOID context, LPDDENUMMODESCALLBACK callback)
    {
        if (!callback)
        {
            return DDERR_INVALIDPARAMS;
        }
        DDSURFACEDESC2 filter;
        if (desc)
        {
            toDesc2(*desc, filter);
        }
        ModeCallback1 thunk = {callback, context};
        return m_owner.EnumDisplayModes(flags, desc ? &filter : nullptr, &thunk, &ModeCallback1::thunk);
    }

    HRESULT DirectDraw1::EnumSurfaces(DWORD, LPDDSURFACEDESC, LPVOID, LPDDENUMSURFACESCALLBACK)
    {
        unsupported("IDirectDraw::EnumSurfaces");
        return DDERR_UNSUPPORTED;
    }

    HRESULT DirectDraw1::FlipToGDISurface()
    {
        return m_owner.FlipToGDISurface();
    }

    HRESULT DirectDraw1::GetCaps(LPDDCAPS driverCaps, LPDDCAPS helCaps)
    {
        return m_owner.GetCaps(driverCaps, helCaps);
    }

    HRESULT DirectDraw1::GetDisplayMode(LPDDSURFACEDESC desc)
    {
        if (!desc)
        {
            return DDERR_INVALIDPARAMS;
        }
        DDSURFACEDESC2 desc2;
        m_owner.displayMode(desc2);
        toDesc1(desc2, *desc);
        return DD_OK;
    }

    HRESULT DirectDraw1::GetFourCCCodes(LPDWORD count, LPDWORD codes)
    {
        return m_owner.GetFourCCCodes(count, codes);
    }

    HRESULT DirectDraw1::GetGDISurface(LPDIRECTDRAWSURFACE*)
    {
        unsupported("IDirectDraw::GetGDISurface");
        return DDERR_NOTFOUND;
    }

    HRESULT DirectDraw1::GetMonitorFrequency(LPDWORD frequency)
    {
        return m_owner.GetMonitorFrequency(frequency);
    }

    HRESULT DirectDraw1::GetScanLine(LPDWORD line)
    {
        return m_owner.GetScanLine(line);
    }

    HRESULT DirectDraw1::GetVerticalBlankStatus(LPBOOL inVerticalBlank)
    {
        return m_owner.GetVerticalBlankStatus(inVerticalBlank);
    }

    HRESULT DirectDraw1::Initialize(GUID* guid)
    {
        return m_owner.Initialize(guid);
    }

    HRESULT DirectDraw1::RestoreDisplayMode()
    {
        return m_owner.RestoreDisplayMode();
    }

    HRESULT DirectDraw1::SetCooperativeLevel(HWND window, DWORD flags)
    {
        // DirectShow's video stream sets DDSCL_NORMAL without a window: keep the game's.
        if (!window && m_owner.window())
        {
            return DD_OK;
        }
        return m_owner.SetCooperativeLevel(window, flags);
    }

    HRESULT DirectDraw1::SetDisplayMode(DWORD width, DWORD height, DWORD bpp)
    {
        return m_owner.SetDisplayMode(width, height, bpp, 0, 0);
    }

    HRESULT DirectDraw1::WaitForVerticalBlank(DWORD flags, HANDLE event)
    {
        return m_owner.WaitForVerticalBlank(flags, event);
    }

    // ------------------------------------------------------------------------------------------------------------
    // Clipper

    Clipper* Clipper::create()
    {
        return new Clipper();
    }

    HRESULT Clipper::QueryInterface(REFIID riid, LPVOID* out)
    {
        if (!out)
        {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IDirectDrawClipper)
        {
            *out = this;
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

    ULONG Clipper::AddRef()
    {
        return static_cast<ULONG>(InterlockedIncrement(&m_refs));
    }

    ULONG Clipper::Release()
    {
        const LONG refs = InterlockedDecrement(&m_refs);
        if (refs == 0)
        {
            delete this;
        }
        return static_cast<ULONG>(refs);
    }

    HRESULT Clipper::GetClipList(LPRECT, LPRGNDATA, LPDWORD)
    {
        unsupported("IDirectDrawClipper::GetClipList");
        return DDERR_UNSUPPORTED;
    }

    HRESULT Clipper::GetHWnd(HWND* window)
    {
        if (!window)
        {
            return DDERR_INVALIDPARAMS;
        }
        *window = m_window;
        return DD_OK;
    }

    HRESULT Clipper::Initialize(LPDIRECTDRAW, DWORD)
    {
        return DDERR_ALREADYINITIALIZED;
    }

    HRESULT Clipper::IsClipListChanged(BOOL* changed)
    {
        if (!changed)
        {
            return DDERR_INVALIDPARAMS;
        }
        *changed = FALSE;
        return DD_OK;
    }

    HRESULT Clipper::SetClipList(LPRGNDATA, DWORD)
    {
        unsupported("IDirectDrawClipper::SetClipList");
        return DD_OK;
    }

    HRESULT Clipper::SetHWnd(DWORD, HWND window)
    {
        m_window = window;
        return DD_OK;
    }

    // ------------------------------------------------------------------------------------------------------------
    // Exports

    namespace
    {
        HMODULE g_fallback = nullptr;

        template <class Fn>
        Fn fallback(const char* name)
        {
            static bool logged = false;
            if (!logged)
            {
                logged = true;
                LOG("Direct3D 9 backend unavailable: using the system ddraw.dll");
            }
            return g_fallback ? reinterpret_cast<Fn>(GetProcAddress(g_fallback, name)) : nullptr;
        }

        HRESULT WINAPI directDrawCreate(GUID* guid, LPDIRECTDRAW* out, IUnknown* outer)
        {
            if (!Gpu::available())
            {
                auto fn = fallback<decltype(&directDrawCreate)>("DirectDrawCreate");
                return fn ? fn(guid, out, outer) : DDERR_GENERIC;
            }
            if (!out)
            {
                return DDERR_INVALIDPARAMS;
            }
            if (outer)
            {
                return CLASS_E_NOAGGREGATION;
            }
            *out = DirectDraw::create()->v1();
            return DD_OK;
        }

        HRESULT WINAPI directDrawCreateEx(GUID* guid, LPVOID* out, REFIID iid, IUnknown* outer)
        {
            if (!Gpu::available())
            {
                auto fn = fallback<decltype(&directDrawCreateEx)>("DirectDrawCreateEx");
                return fn ? fn(guid, out, iid, outer) : DDERR_GENERIC;
            }
            if (!out)
            {
                return DDERR_INVALIDPARAMS;
            }
            *out = nullptr;
            if (iid != IID_IDirectDraw7)
            {
                return DDERR_INVALIDPARAMS;
            }
            if (outer)
            {
                return CLASS_E_NOAGGREGATION;
            }
            *out = static_cast<IDirectDraw7*>(DirectDraw::create());
            return DD_OK;
        }

        HRESULT WINAPI directDrawCreateClipper(DWORD flags, LPDIRECTDRAWCLIPPER* out, IUnknown* outer)
        {
            if (!Gpu::available())
            {
                auto fn = fallback<decltype(&directDrawCreateClipper)>("DirectDrawCreateClipper");
                return fn ? fn(flags, out, outer) : DDERR_GENERIC;
            }
            if (!out)
            {
                return DDERR_INVALIDPARAMS;
            }
            if (outer)
            {
                return CLASS_E_NOAGGREGATION;
            }
            *out = Clipper::create();
            return DD_OK;
        }

        // One device: the primary display driver (null GUID), the one the game creates.
        HRESULT WINAPI directDrawEnumerateA(LPDDENUMCALLBACKA callback, LPVOID context)
        {
            if (!callback)
            {
                return DDERR_INVALIDPARAMS;
            }
            char description[] = "Primary Display Driver", name[] = "display";
            callback(nullptr, description, name, context);
            return DD_OK;
        }

        HRESULT WINAPI directDrawEnumerateW(LPDDENUMCALLBACKW callback, LPVOID context)
        {
            if (!callback)
            {
                return DDERR_INVALIDPARAMS;
            }
            wchar_t description[] = L"Primary Display Driver", name[] = L"display";
            callback(nullptr, description, name, context);
            return DD_OK;
        }

        HRESULT WINAPI directDrawEnumerateExA(LPDDENUMCALLBACKEXA callback, LPVOID context, DWORD)
        {
            if (!callback)
            {
                return DDERR_INVALIDPARAMS;
            }
            char description[] = "Primary Display Driver", name[] = "display";
            callback(nullptr, description, name, context, nullptr);
            return DD_OK;
        }

        HRESULT WINAPI directDrawEnumerateExW(LPDDENUMCALLBACKEXW callback, LPVOID context, DWORD)
        {
            if (!callback)
            {
                return DDERR_INVALIDPARAMS;
            }
            wchar_t description[] = L"Primary Display Driver", name[] = L"display";
            callback(nullptr, description, name, context, nullptr);
            return DD_OK;
        }

        const Export g_exports[] = {
            {"DirectDrawCreate", reinterpret_cast<FARPROC>(&directDrawCreate)},
            {"DirectDrawCreateEx", reinterpret_cast<FARPROC>(&directDrawCreateEx)},
            {"DirectDrawCreateClipper", reinterpret_cast<FARPROC>(&directDrawCreateClipper)},
            {"DirectDrawEnumerateA", reinterpret_cast<FARPROC>(&directDrawEnumerateA)},
            {"DirectDrawEnumerateW", reinterpret_cast<FARPROC>(&directDrawEnumerateW)},
            {"DirectDrawEnumerateExA", reinterpret_cast<FARPROC>(&directDrawEnumerateExA)},
            {"DirectDrawEnumerateExW", reinterpret_cast<FARPROC>(&directDrawEnumerateExW)},
        };
    }

    std::span<const Export> exports()
    {
        return g_exports;
    }

    void setFallback(HMODULE systemDdraw)
    {
        g_fallback = systemDdraw;
    }
}
