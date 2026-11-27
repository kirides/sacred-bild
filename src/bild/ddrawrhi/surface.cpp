#include "ddrawrhi/surface.h"
#include "ddrawrhi/context.h"
#include "ddrawrhi/directdraw.h"
#include "ddraw9/format.h"
#include "log.h"

#include <algorithm>
#include <cstring>
#include <mutex>

namespace DDrawRhi
{
    namespace Format = DDraw9::Format;

    namespace
    {
        const void* g_vtable7 = nullptr;
        const void* g_vtable1 = nullptr;

        const void* vtableOf(const void* object)
        {
            return *static_cast<const void* const*>(object);
        }

        LONG rectWidth(const RECT& r) { return r.right - r.left; }
        LONG rectHeight(const RECT& r) { return r.bottom - r.top; }

        // Index into Surface::m_colorKeys for a DDCKEY_* flag.
        int colorKeyIndex(DWORD flags)
        {
            if (flags & DDCKEY_DESTBLT) return 0;
            if (flags & DDCKEY_DESTOVERLAY) return 1;
            if (flags & DDCKEY_SRCBLT) return 2;
            if (flags & DDCKEY_SRCOVERLAY) return 3;
            return -1;
        }

        // A 32-bit top-down DIB section.
        HBITMAP createDib(UINT width, UINT height, DWORD bits, const DWORD masks[3], uint8_t*& pixels)
        {
            struct
            {
                BITMAPINFOHEADER header;
                DWORD masks[3];
            } info = {};
            info.header.biSize = sizeof(info.header);
            info.header.biWidth = static_cast<LONG>(width);
            info.header.biHeight = -static_cast<LONG>(height);
            info.header.biPlanes = 1;
            info.header.biBitCount = static_cast<WORD>(bits);
            info.header.biCompression = masks ? BI_BITFIELDS : BI_RGB;
            if (masks)
            {
                std::memcpy(info.masks, masks, sizeof(info.masks));
            }
            void* p = nullptr;
            HBITMAP dib = CreateDIBSection(nullptr, reinterpret_cast<BITMAPINFO*>(&info), DIB_RGB_COLORS, &p, nullptr, 0);
            pixels = static_cast<uint8_t*>(p);
            return dib;
        }

        // GDI can draw into DIB sections of these formats directly.
        bool gdiFormat(d9::D3DFORMAT format, DWORD& bits, DWORD masks[3], bool& bitfields)
        {
            bitfields = true;
            switch (format)
            {
            case d9::D3DFMT_X8R8G8B8: case d9::D3DFMT_A8R8G8B8:
                bits = 32; masks[0] = 0xFF0000; masks[1] = 0xFF00; masks[2] = 0xFF;
                return true;
            case d9::D3DFMT_R5G6B5:
                bits = 16; masks[0] = 0xF800; masks[1] = 0x7E0; masks[2] = 0x1F;
                return true;
            case d9::D3DFMT_X1R5G5B5: case d9::D3DFMT_A1R5G5B5:
                bits = 16; masks[0] = 0x7C00; masks[1] = 0x3E0; masks[2] = 0x1F;
                return true;
            case d9::D3DFMT_R8G8B8:
                bits = 24; bitfields = false;
                return true;
            default:
                return false;
            }
        }

        // A D3DCOLOR from a DirectDraw fill value in the surface's format.
        D3DCOLOR fillColor(const DDPIXELFORMAT& pf, DWORD value)
        {
            const Format::Rgb rgb(pf);
            return rgb.valid() ? rgb.toArgb(value) : value;
        }

        struct EnumThunk1
        {
            LPDDENUMSURFACESCALLBACK callback;
            LPVOID context;

            static HRESULT CALLBACK thunk(LPDIRECTDRAWSURFACE7 surface, LPDDSURFACEDESC2 desc, LPVOID self)
            {
                auto* me = static_cast<EnumThunk1*>(self);
                DDSURFACEDESC desc1;
                toDesc1(*desc, desc1);
                return me->callback(Surface::from(surface)->v1(), &desc1, me->context);
            }
        };
    }

    // ------------------------------------------------------------------------------------------------------------
    // Creation and lifetime

    Surface::Surface(DirectDraw* ddraw, Kind kind, const DDSURFACEDESC2& desc, d9::D3DFORMAT format)
        : m_ddraw(ddraw), m_kind(kind), m_desc(desc), m_format(format)
    {
        m_ddraw->AddRef();
        if (!g_vtable7)
        {
            g_vtable7 = vtableOf(static_cast<IDirectDrawSurface7*>(this));
            g_vtable1 = vtableOf(static_cast<IDirectDrawSurface*>(&m_v1));
        }
    }

    Surface::~Surface()
    {
        for (PrivateData& data : m_privateData)
        {
            if (data.object)
            {
                data.object->Release();
            }
        }
        if (m_dc)
        {
            DeleteDC(m_dc);
        }
        releaseDcTemp();
        if (m_backBuffer)
        {
            m_backBuffer->m_frontBuffer = nullptr;
            m_backBuffer->Release();
        }
        if (m_depth)
        {
            m_depth->Release();
        }
        if (m_clipper)
        {
            m_clipper->Release();
        }
        if (m_gpu)
        {
            Context::forget(m_gpu);
        }
        if (m_dib)
        {
            DeleteObject(m_dib);
        }
        m_ddraw->Release();
    }

    Surface* Surface::from(const void* iface)
    {
        if (!iface)
        {
            return nullptr;
        }
        const void* vtable = vtableOf(iface);
        if (vtable && vtable == g_vtable7)
        {
            return static_cast<Surface*>(static_cast<IDirectDrawSurface7*>(const_cast<void*>(iface)));
        }
        if (vtable && vtable == g_vtable1)
        {
            return &static_cast<Surface1*>(static_cast<IDirectDrawSurface*>(const_cast<void*>(iface)))->owner();
        }
        return nullptr;
    }

    HRESULT Surface::create(DirectDraw* ddraw, const DDSURFACEDESC2& in, Surface** out)
    {
        *out = nullptr;
        DDSURFACEDESC2 desc = in;
        desc.dwSize = sizeof(desc);
        if (!(desc.dwFlags & DDSD_CAPS))
        {
            desc.ddsCaps = {};
        }
        DWORD& caps = desc.ddsCaps.dwCaps;
        const DWORD caps2 = desc.ddsCaps.dwCaps2;
        // Textures the game explicitly puts into video memory without management have no system memory copy: only
        // Blt from other textures writes them (SacredBild's atlas pages).
        const bool gpuOnly = (caps & DDSCAPS_TEXTURE) && (caps & DDSCAPS_VIDEOMEMORY) && !(caps & DDSCAPS_SYSTEMMEMORY) &&
            !(caps2 & (DDSCAPS2_TEXTUREMANAGE | DDSCAPS2_D3DTEXTUREMANAGE));
        if (caps2 & (DDSCAPS2_CUBEMAP | DDSCAPS2_VOLUME))
        {
            unsupported("cube map and volume surfaces");
            return DDERR_UNSUPPORTED;
        }

        Kind kind = Kind::Memory;
        if (caps & DDSCAPS_PRIMARYSURFACE)
        {
            kind = Kind::Primary;
        }
        else if (caps & DDSCAPS_ZBUFFER)
        {
            kind = Kind::Depth;
        }
        else if (caps & DDSCAPS_TEXTURE)
        {
            kind = Kind::Texture;
        }
        else if (caps & DDSCAPS_3DDEVICE)
        {
            kind = Kind::RenderTarget;
        }

        DDSURFACEDESC2 mode;
        ddraw->displayMode(mode);
        if (kind == Kind::Primary)
        {
            desc.dwWidth = mode.dwWidth;
            desc.dwHeight = mode.dwHeight;
            desc.ddpfPixelFormat = mode.ddpfPixelFormat;
        }
        else if (!(desc.dwFlags & DDSD_WIDTH) || !(desc.dwFlags & DDSD_HEIGHT) || !desc.dwWidth || !desc.dwHeight)
        {
            return DDERR_INVALIDPARAMS;
        }

        d9::D3DFORMAT format = d9::D3DFMT_UNKNOWN;
        if (kind == Kind::Depth && !(desc.dwFlags & DDSD_PIXELFORMAT))
        {
            // Old-style description: the depth sits where version 1 descriptions keep dwZBufferBitDepth.
            DDPIXELFORMAT pf = {};
            pf.dwSize = sizeof(pf);
            pf.dwFlags = DDPF_ZBUFFER;
            pf.dwZBufferBitDepth = (desc.dwFlags & DDSD_ZBUFFERBITDEPTH) ? desc.dwMipMapCount : 16;
            format = Format::fromPixelFormat(pf);
            desc.dwFlags &= ~DDSD_ZBUFFERBITDEPTH;
            desc.dwMipMapCount = 0;
        }
        else if (kind != Kind::Primary && (desc.dwFlags & DDSD_PIXELFORMAT))
        {
            format = Format::fromPixelFormat(desc.ddpfPixelFormat);
        }
        else
        {
            format = Format::fromPixelFormat(mode.ddpfPixelFormat);
        }
        if (format == d9::D3DFMT_UNKNOWN)
        {
            const DDPIXELFORMAT& pf = desc.ddpfPixelFormat;
            LOG("Direct3D 9: surface format not supported (caps {:08x}, flags {:08x}, fourcc {:08x}, {} bits, masks {:x} {:x} {:x} {:x})",
                caps, pf.dwFlags, pf.dwFourCC, pf.dwRGBBitCount, pf.dwRBitMask, pf.dwGBitMask, pf.dwBBitMask,
                pf.dwRGBAlphaBitMask);
            return DDERR_INVALIDPIXELFORMAT;
        }
        desc.ddpfPixelFormat = Format::toPixelFormat(format);

        if ((caps & DDSCAPS_MIPMAP) && (desc.dwFlags & DDSD_MIPMAPCOUNT) && desc.dwMipMapCount > 1)
        {
            unsupported("mipmapped textures (only the top level is created)");
        }
        if (desc.dwFlags & DDSD_MIPMAPCOUNT)
        {
            desc.dwMipMapCount = 1;
        }
        switch (kind)
        {
        case Kind::Primary: caps |= DDSCAPS_FRONTBUFFER | DDSCAPS_VISIBLE | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM; break;
        case Kind::RenderTarget: case Kind::Depth: caps |= DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM; break;
        case Kind::Texture:
            if (!(caps & DDSCAPS_SYSTEMMEMORY))
            {
                caps |= DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
            }
            break;
        case Kind::Memory: caps = (caps & ~(DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM)) | DDSCAPS_SYSTEMMEMORY; break;
        }
        desc.dwFlags |= DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | DDSD_PITCH;
        desc.lPitch = static_cast<LONG>(Format::pitch(format, desc.dwWidth));
        desc.lpSurface = nullptr;

        auto* surface = new Surface(ddraw, kind, desc, format);
        surface->m_gpuOnly = gpuOnly;
        HRESULT hr = surface->init();
        if (FAILED(hr))
        {
            surface->Release();
            return hr;
        }

        // A flip chain: one back buffer, whatever the count (frames go through Direct3D 9's swap chain anyway).
        if (kind == Kind::Primary && (caps & DDSCAPS_FLIP) && (desc.dwFlags & DDSD_BACKBUFFERCOUNT) && desc.dwBackBufferCount)
        {
            DDSURFACEDESC2 back = {};
            back.dwSize = sizeof(back);
            back.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | DDSD_PITCH;
            back.dwWidth = desc.dwWidth;
            back.dwHeight = desc.dwHeight;
            back.lPitch = desc.lPitch;
            back.ddpfPixelFormat = desc.ddpfPixelFormat;
            back.ddsCaps.dwCaps = DDSCAPS_BACKBUFFER | DDSCAPS_COMPLEX | DDSCAPS_FLIP | DDSCAPS_3DDEVICE |
                DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
            auto* backBuffer = new Surface(ddraw, Kind::RenderTarget, back, format);
            hr = backBuffer->init();
            if (FAILED(hr))
            {
                backBuffer->Release();
                surface->Release();
                return hr;
            }
            surface->m_backBuffer = backBuffer;
            backBuffer->m_frontBuffer = surface;
        }
        *out = surface;
        return DD_OK;
    }

    HRESULT Surface::init()
    {
        const UINT w = width(), h = height();
        if (m_kind == Kind::Primary)
        {
            return DD_OK;
        }
        if (m_kind == Kind::Memory)
        {
            DWORD bits = 0, masks[3] = {};
            bool bitfields = false;
            if (gdiFormat(m_format, bits, masks, bitfields))
            {
                m_dib = createDib(w, h, bits, bitfields ? masks : nullptr, m_bits);
                if (m_dib)
                {
                    m_pitch = static_cast<int>(((w * bits + 31) / 32) * 4);
                }
            }
            if (!m_dib)
            {
                m_pitch = static_cast<int>((Format::pitch(m_format, w) + 3) & ~3u);
                const size_t size = size_t(m_pitch) * Format::rows(m_format, h);
                m_heapBits.reset(new (std::nothrow) uint8_t[size]());
                m_bits = m_heapBits.get();
                if (!m_bits)
                {
                    return DDERR_OUTOFMEMORY;
                }
            }
            m_desc.lPitch = m_pitch;
            return DD_OK;
        }

        if (!Context::available())
        {
            return DDERR_GENERIC;
        }
        const nvrhi::Format gpuFormat = DDrawRhi::gpuFormat(m_format);
        if (gpuFormat == nvrhi::Format::UNKNOWN)
        {
            LOG("NVRHI backend: no GPU format for {}", Format::name(m_format));
            return DDERR_INVALIDPIXELFORMAT;
        }
        nvrhi::TextureDesc desc;
        desc.width = w;
        desc.height = h;
        desc.format = gpuFormat;
        switch (m_kind)
        {
        case Kind::RenderTarget:
            desc.debugName = "render target";
            desc.isRenderTarget = true;
            desc.initialState = nvrhi::ResourceStates::RenderTarget;
            desc.keepInitialState = true;
            desc.setClearValue(nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
            break;
        case Kind::Depth:
            desc.debugName = "z-buffer";
            desc.isRenderTarget = true;
            desc.initialState = nvrhi::ResourceStates::DepthWrite;
            desc.keepInitialState = true;
            desc.setClearValue(nvrhi::Color(1.0f, 0.0f, 0.0f, 0.0f));
            break;
        case Kind::Texture:
            desc.debugName = m_gpuOnly ? "texture (GPU only)" : "texture";
            desc.initialState = nvrhi::ResourceStates::ShaderResource;
            desc.keepInitialState = true;
            break;
        default:
            break;
        }
        {
            std::scoped_lock lock(Context::lock());
            m_gpu = Context::device()->createTexture(desc);
            if (m_gpu && m_kind == Kind::RenderTarget)
            {
                Context::commands()->clearTextureFloat(m_gpu, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
            }
            else if (m_gpu && m_kind == Kind::Depth)
            {
                Context::commands()->clearDepthStencilTexture(m_gpu, nvrhi::AllSubresources, true, 1.0f, true, 0);
            }
            else if (m_gpu && m_kind == Kind::Texture && gpuFormat == nvrhi::Format::BGRA8_UNORM)
            {
                // Transparent black until the first upload, as Direct3D 12's fresh memory is (Vulkan's is undefined).
                thread_local std::vector<uint8_t> zeros;
                zeros.assign(size_t(w) * h * 4, 0);
                Context::commands()->writeTexture(m_gpu, 0, 0, zeros.data(), size_t(w) * 4);
            }
        }
        if (!m_gpu)
        {
            LOG("NVRHI backend: creating a {}x{} {} {} failed", w, h, Format::name(m_format),
                m_kind == Kind::RenderTarget ? "render target" : m_kind == Kind::Depth ? "depth buffer" : "texture");
            return DDERR_OUTOFVIDEOMEMORY;
        }
        if (m_kind == Kind::Texture && !m_gpuOnly)
        {
            m_cpuPitch = Format::pitch(m_format, w);
            m_cpu.assign(size_t(m_cpuPitch) * Format::rows(m_format, h), 0);
            m_dirty.store(true, std::memory_order_relaxed);
        }
        return DD_OK;
    }

    HRESULT Surface::QueryInterface(REFIID riid, LPVOID* out)
    {
        if (!out)
        {
            return E_POINTER;
        }
        *out = nullptr;
        if (riid == IID_IUnknown || riid == IID_IDirectDrawSurface7)
        {
            *out = static_cast<IDirectDrawSurface7*>(this);
        }
        else if (riid == IID_IDirectDrawSurface)
        {
            *out = &m_v1;
        }
        else
        {
            unsupported("IDirectDrawSurface7::QueryInterface for an interface other than IDirectDrawSurface7, IDirectDrawSurface");
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    ULONG Surface::AddRef()
    {
        return static_cast<ULONG>(InterlockedIncrement(&m_refs));
    }

    ULONG Surface::Release()
    {
        const LONG refs = InterlockedDecrement(&m_refs);
        if (refs == 0)
        {
            delete this;
        }
        return static_cast<ULONG>(refs);
    }

    // ------------------------------------------------------------------------------------------------------------
    // CPU access

    bool Surface::fullRect(const RECT* in, RECT& out) const
    {
        const RECT all = {0, 0, static_cast<LONG>(width()), static_cast<LONG>(height())};
        if (!in)
        {
            out = all;
            return true;
        }
        return IntersectRect(&out, in, &all) != FALSE;
    }

    HRESULT Surface::map(const RECT* rect, bool readOnly, Mapping& out)
    {
        std::scoped_lock lock(m_lock);
        if (m_mapped)
        {
            return DDERR_SURFACEBUSY;
        }
        RECT r;
        fullRect(rect, r);
        const UINT bytes = Format::bitsPerPixel(m_format) / 8;
        HRESULT hr = DD_OK;
        switch (m_kind)
        {
        case Kind::Primary:
            if (!m_primaryBits)
            {
                m_primaryBits.reset(new (std::nothrow) uint8_t[size_t(m_desc.lPitch) * height()]());
                if (!m_primaryBits)
                {
                    return DDERR_OUTOFMEMORY;
                }
            }
            out.bits = m_primaryBits.get() + r.top * m_desc.lPitch + r.left * bytes;
            out.pitch = m_desc.lPitch;
            break;
        case Kind::Memory:
            GdiFlush();
            out.bits = m_bits + r.top * m_pitch + Format::pitch(m_format, r.left);
            out.pitch = m_pitch;
            break;
        case Kind::RenderTarget:
        {
            hr = readBack();
            if (FAILED(hr))
            {
                return hr;
            }
            out.bits = m_cpu.data() + size_t(r.top) * m_cpuPitch + Format::pitch(m_format, r.left);
            out.pitch = static_cast<int>(m_cpuPitch);
            break;
        }
        case Kind::Texture:
        {
            if (m_cpu.empty())
            {
                unsupported("locking a video memory texture without system memory copy");
                return DDERR_UNSUPPORTED;
            }
            const UINT row = Format::blockCompressed(m_format) ? r.top / 4 : r.top;
            const UINT col = Format::blockCompressed(m_format) ? Format::pitch(m_format, r.left & ~3) : Format::pitch(m_format, r.left);
            out.bits = m_cpu.data() + size_t(row) * m_cpuPitch + col;
            out.pitch = static_cast<int>(m_cpuPitch);
            break;
        }
        case Kind::Depth:
            unsupported("locking a depth buffer");
            return DDERR_UNSUPPORTED;
        }
        m_mapped = true;
        m_mapReadOnly = readOnly;
        m_mapRect = r;
        return DD_OK;
    }

    HRESULT Surface::readBack()
    {
        const UINT w = width(), h = height();
        std::scoped_lock lock(Context::lock());
        nvrhi::IDevice* dev = Context::device();
        if (!m_staging)
        {
            m_staging = dev->createStagingTexture(nvrhi::TextureDesc().setWidth(w).setHeight(h)
                .setFormat(nvrhi::Format::BGRA8_UNORM).setDebugName("render target read-back"), nvrhi::CpuAccessMode::Read);
            if (!m_staging)
            {
                LOG("NVRHI backend: creating the render target's read-back texture failed");
                return DDERR_GENERIC;
            }
        }
        Context::commands()->copyTexture(m_staging, nvrhi::TextureSlice(), m_gpu, nvrhi::TextureSlice());
        Context::flush(true);
        size_t pitch = 0;
        auto* bits = static_cast<uint8_t*>(dev->mapStagingTexture(m_staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &pitch));
        if (!bits)
        {
            unsupported("reading the render target back (mapping failed)");
            return DDERR_GENERIC;
        }
        m_cpuPitch = Format::pitch(m_format, w);
        m_cpu.resize(size_t(m_cpuPitch) * h);
        Format::Image from = {bits, static_cast<int>(pitch), w, h, d9::D3DFMT_A8R8G8B8, Format::toPixelFormat(d9::D3DFMT_A8R8G8B8)};
        Format::Image to = {m_cpu.data(), static_cast<int>(m_cpuPitch), w, h, m_format, m_desc.ddpfPixelFormat};
        const RECT all = {0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
        if (m_format == d9::D3DFMT_X8R8G8B8 || m_format == d9::D3DFMT_A8R8G8B8)
        {
            for (UINT y = 0; y < h; ++y)
            {
                std::memcpy(m_cpu.data() + size_t(y) * m_cpuPitch, bits + y * pitch, size_t(w) * 4);
            }
        }
        else
        {
            Format::copy(from, all, to, all, nullptr);
        }
        dev->unmapStagingTexture(m_staging);
        return DD_OK;
    }

    nvrhi::TextureHandle Surface::temporaryTexture(const uint8_t* bits, int pitch, UINT w, UINT h, d9::D3DFORMAT format,
        const DDPIXELFORMAT& pf)
    {
        const nvrhi::Format gpu = DDrawRhi::gpuFormat(format);
        nvrhi::TextureHandle temp = Context::device()->createTexture(nvrhi::TextureDesc().setWidth(w).setHeight(h)
            .setFormat(gpu).setDebugName("copy source").setInitialState(nvrhi::ResourceStates::ShaderResource)
            .setKeepInitialState(true));
        if (!temp)
        {
            return nullptr;
        }
        if (gpu == nvrhi::Format::BGRA8_UNORM)
        {
            std::vector<uint8_t> converted(size_t(w) * h * 4);
            if (!toBgra8(format, pf, bits, pitch, w, h, converted.data(), w * 4))
            {
                return nullptr;
            }
            Context::commands()->writeTexture(temp, 0, 0, converted.data(), w * 4);
        }
        else
        {
            Context::commands()->writeTexture(temp, 0, 0, bits, static_cast<size_t>(pitch));
        }
        return temp;
    }

    void Surface::uploadRect(const RECT& rect)
    {
        const UINT w = static_cast<UINT>(rect.right - rect.left), h = static_cast<UINT>(rect.bottom - rect.top);
        if (!m_gpu || !w || !h || m_cpu.empty())
        {
            return;
        }
        std::scoped_lock lock(Context::lock());
        const uint8_t* bits = m_cpu.data() + size_t(rect.top) * m_cpuPitch + Format::pitch(m_format, rect.left);
        nvrhi::TextureHandle temp = temporaryTexture(bits, static_cast<int>(m_cpuPitch), w, h, m_format, m_desc.ddpfPixelFormat);
        if (!temp)
        {
            unsupported("writing the render target from the CPU");
            return;
        }
        nvrhi::TextureSlice dst;
        dst.x = static_cast<uint32_t>(rect.left);
        dst.y = static_cast<uint32_t>(rect.top);
        dst.width = w;
        dst.height = h;
        nvrhi::TextureSlice src;
        src.width = w;
        src.height = h;
        Context::commands()->copyTexture(m_gpu, dst, temp, src);
    }

    void Surface::unmap()
    {
        std::scoped_lock lock(m_lock);
        if (!m_mapped)
        {
            return;
        }
        m_mapped = false;
        switch (m_kind)
        {
        case Kind::Primary:
            m_primaryBits.reset();      // 4 bytes per desktop pixel nobody looks at again
            break;
        case Kind::RenderTarget:
            if (!m_mapReadOnly)
            {
                uploadRect(m_mapRect);
            }
            break;
        case Kind::Texture:
            if (!m_mapReadOnly)
            {
                m_dirty.store(true, std::memory_order_release);
            }
            break;
        default:
            break;
        }
        if (!m_mapReadOnly)
        {
            changed();
        }
    }

    HRESULT Surface::Lock(LPRECT rect, LPDDSURFACEDESC2 desc, DWORD flags, HANDLE)
    {
        if (!desc || desc->dwSize != sizeof(DDSURFACEDESC2))
        {
            return DDERR_INVALIDPARAMS;
        }
        std::scoped_lock lock(m_lock);
        if (m_locked || m_dc)
        {
            return DDERR_SURFACEBUSY;
        }
        RECT r;
        if (!fullRect(rect, r))
        {
            return DDERR_INVALIDRECT;
        }
        Mapping m;
        const HRESULT hr = map(&r, (flags & DDLOCK_READONLY) != 0, m);
        if (FAILED(hr))
        {
            return hr;
        }
        m_locked = true;
        *desc = m_desc;
        desc->dwFlags |= DDSD_LPSURFACE | DDSD_PITCH;
        desc->lpSurface = m.bits;
        desc->lPitch = m.pitch;
        return DD_OK;
    }

    HRESULT Surface::Unlock(LPRECT)
    {
        std::scoped_lock lock(m_lock);
        if (!m_locked)
        {
            return DDERR_NOTLOCKED;
        }
        m_locked = false;
        unmap();
        return DD_OK;
    }

    void Surface::releaseDcTemp()
    {
        if (m_dcTemp)
        {
            DeleteObject(m_dcTemp);
            m_dcTemp = nullptr;
            m_dcTempBits = nullptr;
        }
    }

    HRESULT Surface::GetDC(HDC* dc)
    {
        if (!dc)
        {
            return DDERR_INVALIDPARAMS;
        }
        std::scoped_lock lock(m_lock);
        if (m_dc)
        {
            return DDERR_DCALREADYCREATED;
        }
        if (m_locked)
        {
            return DDERR_SURFACEBUSY;
        }
        if (m_kind == Kind::Memory && m_dib)
        {
            GdiFlush();
            m_dc = CreateCompatibleDC(nullptr);
            m_dcOldBitmap = SelectObject(m_dc, m_dib);
            *dc = m_dc;
            return DD_OK;
        }
        // Anything else (the render target too): GDI draws into a 32-bit copy, converted back on ReleaseDC.
        HRESULT hr = map(nullptr, false, m_dcMapping);
        if (FAILED(hr))
        {
            return DDERR_CANTCREATEDC;
        }
        const DWORD masks[3] = {0xFF0000, 0xFF00, 0xFF};
        m_dcTemp = createDib(width(), height(), 32, masks, m_dcTempBits);
        if (!m_dcTemp)
        {
            unmap();
            return DDERR_CANTCREATEDC;
        }
        Format::Image surface = {m_dcMapping.bits, m_dcMapping.pitch, width(), height(), m_format, m_desc.ddpfPixelFormat};
        Format::Image temp = {m_dcTempBits, static_cast<int>(width() * 4), width(), height(), d9::D3DFMT_X8R8G8B8,
            Format::toPixelFormat(d9::D3DFMT_X8R8G8B8)};
        const RECT all = {0, 0, static_cast<LONG>(width()), static_cast<LONG>(height())};
        if (!Format::copy(surface, all, temp, all, nullptr))
        {
            unsupported("GetDC on a surface of this format");
            releaseDcTemp();
            unmap();
            return DDERR_CANTCREATEDC;
        }
        m_dc = CreateCompatibleDC(nullptr);
        m_dcOldBitmap = SelectObject(m_dc, m_dcTemp);
        *dc = m_dc;
        return DD_OK;
    }

    HRESULT Surface::ReleaseDC(HDC dc)
    {
        std::scoped_lock lock(m_lock);
        if (!m_dc || dc != m_dc)
        {
            return DDERR_NODC;
        }
        GdiFlush();
        if (m_dcTemp)
        {
            SelectObject(m_dc, m_dcOldBitmap);
            DeleteDC(m_dc);
            Format::Image surface = {m_dcMapping.bits, m_dcMapping.pitch, width(), height(), m_format, m_desc.ddpfPixelFormat};
            Format::Image temp = {m_dcTempBits, static_cast<int>(width() * 4), width(), height(), d9::D3DFMT_X8R8G8B8,
                Format::toPixelFormat(d9::D3DFMT_X8R8G8B8)};
            const RECT all = {0, 0, static_cast<LONG>(width()), static_cast<LONG>(height())};
            Format::copy(temp, all, surface, all, nullptr);
            releaseDcTemp();
            unmap();
        }
        else
        {
            SelectObject(m_dc, m_dcOldBitmap);
            DeleteDC(m_dc);
            changed();
        }
        m_dc = nullptr;
        return DD_OK;
    }

    // ------------------------------------------------------------------------------------------------------------
    // Blt

    HRESULT Surface::fill(const RECT& rect, DWORD color)
    {
        switch (m_kind)
        {
        case Kind::RenderTarget:
        {
            const D3DCOLOR argb = fillColor(m_desc.ddpfPixelFormat, color);
            const float rgba[4] = {((argb >> 16) & 255) / 255.0f, ((argb >> 8) & 255) / 255.0f, (argb & 255) / 255.0f,
                (argb >> 24) / 255.0f};
            std::scoped_lock lock(Context::lock());
            Context::fill(m_gpu, nullptr, rect, rgba, true, false, 0.0f);
            changed();
            return DD_OK;
        }
        case Kind::Depth:
            unsupported("depth fill Blt");
            return DD_OK;
        default:
        {
            Mapping m;
            const HRESULT hr = map(&rect, false, m);
            if (FAILED(hr))
            {
                return hr;
            }
            Format::Image image = {m.bits, m.pitch, width(), height(), m_format, m_desc.ddpfPixelFormat};
            Format::fill(image, {0, 0, rectWidth(rect), rectHeight(rect)}, color);
            unmap();
            return DD_OK;
        }
        }
    }

    HRESULT Surface::uploadToRenderTarget(Surface& src, const RECT& srcRect, const RECT& dstRect)
    {
        const UINT w = static_cast<UINT>(rectWidth(srcRect)), h = static_cast<UINT>(rectHeight(srcRect));
        Mapping from;
        HRESULT hr = src.map(&srcRect, true, from);
        if (FAILED(hr))
        {
            unsupported("Blt from a texture or system memory surface to the render target");
            return DDERR_GENERIC;
        }
        {
            std::scoped_lock lock(Context::lock());
            nvrhi::TextureHandle temp = temporaryTexture(from.bits, from.pitch, w, h, src.m_format, src.m_desc.ddpfPixelFormat);
            if (temp)
            {
                const bool scaled = rectWidth(dstRect) != static_cast<LONG>(w) || rectHeight(dstRect) != static_cast<LONG>(h);
                Context::copy(temp, {0, 0, static_cast<LONG>(w), static_cast<LONG>(h)}, m_gpu, dstRect, scaled);
            }
            else
            {
                hr = DDERR_GENERIC;
            }
        }
        src.unmap();
        if (FAILED(hr))
        {
            unsupported("Blt from a texture or system memory surface to the render target");
            return DDERR_GENERIC;
        }
        changed();
        return DD_OK;
    }

    HRESULT Surface::copyFrom(Surface& src, const RECT& srcRect, const RECT& dstRect, const DDCOLORKEY* key)
    {
        const bool sameSize = rectWidth(srcRect) == rectWidth(dstRect) && rectHeight(srcRect) == rectHeight(dstRect);
        if (m_kind == Kind::RenderTarget && !key)
        {
            if (src.m_kind == Kind::RenderTarget)
            {
                std::scoped_lock lock(Context::lock());
                if (&src == this)
                {
                    // Within one render target: through a copy of the source rectangle.
                    const UINT w = static_cast<UINT>(rectWidth(srcRect)), h = static_cast<UINT>(rectHeight(srcRect));
                    nvrhi::TextureHandle temp = Context::device()->createTexture(nvrhi::TextureDesc().setWidth(w).setHeight(h)
                        .setFormat(nvrhi::Format::BGRA8_UNORM).setDebugName("copy source")
                        .setInitialState(nvrhi::ResourceStates::ShaderResource).setKeepInitialState(true));
                    nvrhi::TextureSlice from;
                    from.x = static_cast<uint32_t>(srcRect.left);
                    from.y = static_cast<uint32_t>(srcRect.top);
                    from.width = w;
                    from.height = h;
                    nvrhi::TextureSlice to;
                    to.width = w;
                    to.height = h;
                    Context::commands()->copyTexture(temp, to, m_gpu, from);
                    Context::copy(temp, {0, 0, static_cast<LONG>(w), static_cast<LONG>(h)}, m_gpu, dstRect, !sameSize);
                }
                else
                {
                    Context::copy(src.m_gpu, srcRect, m_gpu, dstRect, !sameSize);
                }
                changed();
                return DD_OK;
            }
            return uploadToRenderTarget(src, srcRect, dstRect);
        }
        if (m_kind == Kind::Texture && m_cpu.empty())
        {
            // A video memory texture: copied on the GPU from another texture's system memory copy.
            if (src.m_kind == Kind::Texture && !src.m_cpu.empty() && sameSize && src.m_format == m_format && !key)
            {
                const UINT w = static_cast<UINT>(rectWidth(srcRect)), h = static_cast<UINT>(rectHeight(srcRect));
                std::scoped_lock lock(Context::lock());
                std::scoped_lock srcLock(src.m_lock);
                const bool bc = Format::blockCompressed(m_format);
                const uint8_t* bits = src.m_cpu.data() + size_t(bc ? srcRect.top / 4 : srcRect.top) * src.m_cpuPitch +
                    Format::pitch(m_format, bc ? srcRect.left & ~3 : srcRect.left);
                nvrhi::TextureHandle temp = temporaryTexture(bits, static_cast<int>(src.m_cpuPitch), w, h, m_format,
                    m_desc.ddpfPixelFormat);
                if (!temp)
                {
                    return DDERR_GENERIC;
                }
                nvrhi::TextureSlice to;
                to.x = static_cast<uint32_t>(dstRect.left);
                to.y = static_cast<uint32_t>(dstRect.top);
                to.width = w;
                to.height = h;
                nvrhi::TextureSlice from;
                from.width = w;
                from.height = h;
                Context::commands()->copyTexture(m_gpu, to, temp, from);
                changed();
                return DD_OK;
            }
            unsupported("Blt into a video memory texture from anything but a same-format texture of the same size");
            return DDERR_UNSUPPORTED;
        }

        // Everything else on the CPU.
        if (&src == this)
        {
            // Overlapping copies within one surface go through a temporary copy of the source.
            Mapping m;
            HRESULT hr = map(nullptr, false, m);
            if (FAILED(hr))
            {
                return hr;
            }
            const UINT pitch = Format::pitch(m_format, width());
            std::vector<uint8_t> copy(size_t(pitch) * Format::rows(m_format, height()));
            for (UINT y = 0; y < Format::rows(m_format, height()); ++y)
            {
                std::memcpy(copy.data() + size_t(y) * pitch, m.bits + size_t(y) * m.pitch, pitch);
            }
            Format::Image from = {copy.data(), static_cast<int>(pitch), width(), height(), m_format, m_desc.ddpfPixelFormat};
            Format::Image to = {m.bits, m.pitch, width(), height(), m_format, m_desc.ddpfPixelFormat};
            const bool ok = Format::copy(from, srcRect, to, dstRect, key);
            unmap();
            return ok ? DD_OK : DDERR_UNSUPPORTED;
        }
        Mapping from, to;
        HRESULT hr = src.map(nullptr, true, from);
        if (FAILED(hr))
        {
            return hr;
        }
        hr = map(nullptr, false, to);
        if (FAILED(hr))
        {
            src.unmap();
            return hr;
        }
        Format::Image in = {from.bits, from.pitch, src.width(), src.height(), src.m_format, src.m_desc.ddpfPixelFormat};
        Format::Image out = {to.bits, to.pitch, width(), height(), m_format, m_desc.ddpfPixelFormat};
        const bool ok = Format::copy(in, srcRect, out, dstRect, key);
        unmap();
        src.unmap();
        if (!ok)
        {
            unsupported("Blt between these surface formats");
            return DDERR_UNSUPPORTED;
        }
        return DD_OK;
    }

    HRESULT Surface::present(Surface& src, const RECT* srcRect)
    {
        if (src.m_kind != Kind::RenderTarget)
        {
            unsupported("Blt to the primary surface from anything but the render target");
            return DD_OK;
        }
        return Context::present(src.m_gpu, srcRect);
    }

    HRESULT Surface::Blt(LPRECT dstRect, LPDIRECTDRAWSURFACE7 srcIface, LPRECT srcRect, DWORD flags, LPDDBLTFX fx)
    {
        if (m_kind == Kind::Primary)
        {
            // The window: a Blt from the render target presents a frame (the destination is the window's client
            // area in screen coordinates, wherever it is). Fills would only be overwritten by the next frame.
            if (flags & (DDBLT_COLORFILL | DDBLT_DEPTHFILL))
            {
                return DD_OK;
            }
            Surface* src = from(srcIface);
            if (!src)
            {
                return DDERR_INVALIDPARAMS;
            }
            // The game derives the source rectangle from the window's screen position; whole frames it is.
            RECT srcArea;
            const bool inside = srcRect && src->fullRect(srcRect, srcArea) && EqualRect(&srcArea, srcRect);
            return present(*src, inside ? &srcArea : nullptr);
        }
        RECT dst;
        if (!fullRect(dstRect, dst))
        {
            return DD_OK;   // entirely outside: nothing to do
        }
        if (flags & DDBLT_COLORFILL)
        {
            return fx ? fill(dst, fx->dwFillColor) : DDERR_INVALIDPARAMS;
        }
        if (flags & DDBLT_DEPTHFILL)
        {
            unsupported("depth fill Blt");
            return DD_OK;
        }
        Surface* src = from(srcIface);
        if (!src)
        {
            return DDERR_INVALIDPARAMS;
        }
        RECT srcArea;
        if (!src->fullRect(srcRect, srcArea))
        {
            return DD_OK;
        }
        const DDCOLORKEY* key = nullptr;
        if ((flags & DDBLT_KEYSRCOVERRIDE) && fx)
        {
            key = &fx->ddckSrcColorkey;
        }
        else if ((flags & DDBLT_KEYSRC) && (src->m_colorKeySet & (1u << 2)))
        {
            key = &src->m_colorKeys[2];
        }
        if (flags & (DDBLT_KEYDEST | DDBLT_KEYDESTOVERRIDE))
        {
            unsupported("Blt with a destination color key");
        }
        return copyFrom(*src, srcArea, dst, key);
    }

    HRESULT Surface::BltFast(DWORD x, DWORD y, LPDIRECTDRAWSURFACE7 srcIface, LPRECT srcRect, DWORD flags)
    {
        Surface* src = from(srcIface);
        if (!src)
        {
            return DDERR_INVALIDPARAMS;
        }
        RECT srcArea;
        if (!src->fullRect(srcRect, srcArea))
        {
            return DD_OK;
        }
        RECT dst = {static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x) + rectWidth(srcArea),
            static_cast<LONG>(y) + rectHeight(srcArea)};
        if (m_kind == Kind::Primary)
        {
            return present(*src, &srcArea);
        }
        RECT clipped;
        if (!fullRect(&dst, clipped))
        {
            return DD_OK;
        }
        srcArea.right -= dst.right - clipped.right;
        srcArea.bottom -= dst.bottom - clipped.bottom;
        const DDCOLORKEY* key = (flags & DDBLTFAST_SRCCOLORKEY) && (src->m_colorKeySet & (1u << 2)) ? &src->m_colorKeys[2] : nullptr;
        if (flags & DDBLTFAST_DESTCOLORKEY)
        {
            unsupported("BltFast with a destination color key");
        }
        return copyFrom(*src, srcArea, clipped, key);
    }

    HRESULT Surface::BltBatch(LPDDBLTBATCH, DWORD, DWORD)
    {
        unsupported("IDirectDrawSurface7::BltBatch");
        return DDERR_UNSUPPORTED;
    }

    HRESULT Surface::Flip(LPDIRECTDRAWSURFACE7, DWORD)
    {
        if (m_kind == Kind::Primary && m_backBuffer)
        {
            return present(*m_backBuffer, nullptr);
        }
        return DDERR_NOTFLIPPABLE;
    }

    nvrhi::ITexture* Surface::texture()
    {
        if (m_kind == Kind::Texture)
        {
            if (m_dirty.load(std::memory_order_relaxed) && m_lock.try_lock())
            {
                // Locked elsewhere right now (another thread is writing it): uploaded next time.
                if (m_dirty.exchange(false, std::memory_order_acquire) && !m_cpu.empty() && !m_mapped)
                {
                    const UINT w = width(), h = height();
                    if (DDrawRhi::gpuFormat(m_format) == nvrhi::Format::BGRA8_UNORM)
                    {
                        thread_local std::vector<uint8_t> converted;
                        converted.resize(size_t(w) * h * 4);
                        if (toBgra8(m_format, m_desc.ddpfPixelFormat, m_cpu.data(), static_cast<int>(m_cpuPitch), w, h,
                                converted.data(), w * 4))
                        {
                            Context::commands()->writeTexture(m_gpu, 0, 0, converted.data(), w * 4);
                        }
                    }
                    else
                    {
                        Context::commands()->writeTexture(m_gpu, 0, 0, m_cpu.data(), m_cpuPitch);
                    }
                }
                else if (m_mapped)
                {
                    m_dirty.store(true, std::memory_order_relaxed);
                }
                m_lock.unlock();
            }
            return m_gpu;
        }
        if (m_kind == Kind::RenderTarget)
        {
            return m_gpu;
        }
        return nullptr;
    }

    // ------------------------------------------------------------------------------------------------------------
    // Attachments and the rest of IDirectDrawSurface7

    HRESULT Surface::AddAttachedSurface(LPDIRECTDRAWSURFACE7 iface)
    {
        Surface* surface = from(iface);
        if (!surface)
        {
            return DDERR_INVALIDPARAMS;
        }
        if (surface->m_kind == Kind::Depth && (m_kind == Kind::RenderTarget || (m_kind == Kind::Primary && m_backBuffer)))
        {
            Surface* target = m_kind == Kind::Primary ? m_backBuffer : this;
            if (target->m_depth)
            {
                return DDERR_SURFACEALREADYATTACHED;
            }
            surface->AddRef();
            target->m_depth = surface;
            return DD_OK;
        }
        unsupported("AddAttachedSurface of anything but a z-buffer to a render target");
        return DDERR_CANNOTATTACHSURFACE;
    }

    HRESULT Surface::DeleteAttachedSurface(DWORD, LPDIRECTDRAWSURFACE7 iface)
    {
        Surface* surface = from(iface);
        if (m_depth && (!surface || surface == m_depth))
        {
            m_depth->Release();
            m_depth = nullptr;
            return DD_OK;
        }
        return DDERR_SURFACENOTATTACHED;
    }

    HRESULT Surface::EnumAttachedSurfaces(LPVOID context, LPDDENUMSURFACESCALLBACK7 callback)
    {
        if (!callback)
        {
            return DDERR_INVALIDPARAMS;
        }
        for (Surface* surface : {m_backBuffer, m_depth})
        {
            if (!surface)
            {
                continue;
            }
            surface->AddRef();
            DDSURFACEDESC2 desc = surface->m_desc;
            if (callback(surface, &desc, context) == DDENUMRET_CANCEL)
            {
                break;
            }
        }
        return DD_OK;
    }

    HRESULT Surface::GetAttachedSurface(LPDDSCAPS2 caps, LPDIRECTDRAWSURFACE7* out)
    {
        if (!caps || !out)
        {
            return DDERR_INVALIDPARAMS;
        }
        *out = nullptr;
        Surface* found = nullptr;
        if (m_backBuffer && (caps->dwCaps & (DDSCAPS_BACKBUFFER | DDSCAPS_FLIP | DDSCAPS_3DDEVICE)))
        {
            found = m_backBuffer;
        }
        else if (caps->dwCaps & DDSCAPS_ZBUFFER)
        {
            found = m_kind == Kind::Primary && m_backBuffer ? m_backBuffer->m_depth : m_depth;
        }
        else if (m_frontBuffer && (caps->dwCaps & (DDSCAPS_FRONTBUFFER | DDSCAPS_FLIP)))
        {
            found = m_frontBuffer;
        }
        if (!found)
        {
            return DDERR_NOTFOUND;
        }
        found->AddRef();
        *out = found;
        return DD_OK;
    }

    HRESULT Surface::AddOverlayDirtyRect(LPRECT)
    {
        return DDERR_NOTAOVERLAYSURFACE;
    }

    HRESULT Surface::EnumOverlayZOrders(DWORD, LPVOID, LPDDENUMSURFACESCALLBACK7)
    {
        return DDERR_NOTAOVERLAYSURFACE;
    }

    HRESULT Surface::GetBltStatus(DWORD)
    {
        return DD_OK;
    }

    HRESULT Surface::GetCaps(LPDDSCAPS2 caps)
    {
        if (!caps)
        {
            return DDERR_INVALIDPARAMS;
        }
        *caps = m_desc.ddsCaps;
        return DD_OK;
    }

    HRESULT Surface::GetClipper(LPDIRECTDRAWCLIPPER* out)
    {
        if (!out)
        {
            return DDERR_INVALIDPARAMS;
        }
        *out = m_clipper;
        if (!m_clipper)
        {
            return DDERR_NOCLIPPERATTACHED;
        }
        m_clipper->AddRef();
        return DD_OK;
    }

    HRESULT Surface::GetColorKey(DWORD flags, LPDDCOLORKEY key)
    {
        const int i = colorKeyIndex(flags);
        if (!key || i < 0)
        {
            return DDERR_INVALIDPARAMS;
        }
        if (!(m_colorKeySet & (1u << i)))
        {
            return DDERR_NOCOLORKEY;
        }
        *key = m_colorKeys[i];
        return DD_OK;
    }

    HRESULT Surface::GetFlipStatus(DWORD)
    {
        return DD_OK;
    }

    HRESULT Surface::GetOverlayPosition(LPLONG, LPLONG)
    {
        return DDERR_NOTAOVERLAYSURFACE;
    }

    HRESULT Surface::GetPalette(LPDIRECTDRAWPALETTE*)
    {
        return DDERR_NOPALETTEATTACHED;
    }

    HRESULT Surface::GetPixelFormat(LPDDPIXELFORMAT pf)
    {
        if (!pf)
        {
            return DDERR_INVALIDPARAMS;
        }
        *pf = m_desc.ddpfPixelFormat;
        return DD_OK;
    }

    HRESULT Surface::GetSurfaceDesc(LPDDSURFACEDESC2 desc)
    {
        if (!desc || desc->dwSize != sizeof(DDSURFACEDESC2))
        {
            return DDERR_INVALIDPARAMS;
        }
        *desc = m_desc;
        return DD_OK;
    }

    HRESULT Surface::Initialize(LPDIRECTDRAW, LPDDSURFACEDESC2)
    {
        return DDERR_ALREADYINITIALIZED;
    }

    HRESULT Surface::IsLost()
    {
        return DD_OK;
    }

    HRESULT Surface::Restore()
    {
        return DD_OK;
    }

    HRESULT Surface::SetClipper(LPDIRECTDRAWCLIPPER clipper)
    {
        if (clipper)
        {
            clipper->AddRef();
        }
        if (m_clipper)
        {
            m_clipper->Release();
        }
        m_clipper = clipper;
        return DD_OK;
    }

    HRESULT Surface::SetColorKey(DWORD flags, LPDDCOLORKEY key)
    {
        const int i = colorKeyIndex(flags);
        if (i < 0)
        {
            return DDERR_INVALIDPARAMS;
        }
        if (!key)
        {
            m_colorKeySet &= ~(1u << i);
            return DD_OK;
        }
        m_colorKeys[i] = *key;
        if (!(flags & DDCKEY_COLORSPACE))
        {
            m_colorKeys[i].dwColorSpaceHighValue = key->dwColorSpaceLowValue;
        }
        m_colorKeySet |= 1u << i;
        if (m_kind == Kind::Texture && i == 2)
        {
            unsupported("color keyed textures (the key is ignored when drawing)");
        }
        return DD_OK;
    }

    HRESULT Surface::SetOverlayPosition(LONG, LONG)
    {
        return DDERR_NOTAOVERLAYSURFACE;
    }

    HRESULT Surface::SetPalette(LPDIRECTDRAWPALETTE)
    {
        unsupported("palettes");
        return DDERR_UNSUPPORTED;
    }

    HRESULT Surface::UpdateOverlay(LPRECT, LPDIRECTDRAWSURFACE7, LPRECT, DWORD, LPDDOVERLAYFX)
    {
        return DDERR_NOTAOVERLAYSURFACE;
    }

    HRESULT Surface::UpdateOverlayDisplay(DWORD)
    {
        return DDERR_NOTAOVERLAYSURFACE;
    }

    HRESULT Surface::UpdateOverlayZOrder(DWORD, LPDIRECTDRAWSURFACE7)
    {
        return DDERR_NOTAOVERLAYSURFACE;
    }

    HRESULT Surface::GetDDInterface(LPVOID* out)
    {
        if (!out)
        {
            return DDERR_INVALIDPARAMS;
        }
        *out = static_cast<IDirectDraw7*>(m_ddraw);
        m_ddraw->AddRef();
        return DD_OK;
    }

    HRESULT Surface::PageLock(DWORD)
    {
        return DD_OK;
    }

    HRESULT Surface::PageUnlock(DWORD)
    {
        return DD_OK;
    }

    HRESULT Surface::SetSurfaceDesc(LPDDSURFACEDESC2, DWORD)
    {
        unsupported("IDirectDrawSurface7::SetSurfaceDesc");
        return DDERR_UNSUPPORTED;
    }

    HRESULT Surface::SetPrivateData(REFGUID tag, LPVOID data, DWORD size, DWORD flags)
    {
        if (!data && size)
        {
            return DDERR_INVALIDPARAMS;
        }
        PrivateData entry = {tag};
        if (flags & DDSPD_IUNKNOWNPOINTER)
        {
            if (size != sizeof(IUnknown*))
            {
                return DDERR_INVALIDPARAMS;
            }
            entry.object = static_cast<IUnknown*>(data);
            entry.object->AddRef();
        }
        else
        {
            entry.bytes.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
        }
        IUnknown* replaced = nullptr;
        {
            std::scoped_lock lock(m_lock);
            auto it = std::find_if(m_privateData.begin(), m_privateData.end(), [&](const PrivateData& d) { return d.tag == tag; });
            if (it != m_privateData.end())
            {
                replaced = it->object;
                *it = std::move(entry);
            }
            else
            {
                m_privateData.push_back(std::move(entry));
            }
        }
        if (replaced)
        {
            replaced->Release();
        }
        return DD_OK;
    }

    HRESULT Surface::GetPrivateData(REFGUID tag, LPVOID data, LPDWORD size)
    {
        if (!size)
        {
            return DDERR_INVALIDPARAMS;
        }
        std::scoped_lock lock(m_lock);
        auto it = std::find_if(m_privateData.begin(), m_privateData.end(), [&](const PrivateData& d) { return d.tag == tag; });
        if (it == m_privateData.end())
        {
            return DDERR_NOTFOUND;
        }
        const DWORD needed = it->object ? sizeof(IUnknown*) : static_cast<DWORD>(it->bytes.size());
        if (!data || *size < needed)
        {
            *size = needed;
            return DDERR_MOREDATA;
        }
        if (it->object)
        {
            std::memcpy(data, &it->object, sizeof(IUnknown*));
        }
        else if (needed)
        {
            std::memcpy(data, it->bytes.data(), needed);
        }
        *size = needed;
        return DD_OK;
    }

    HRESULT Surface::FreePrivateData(REFGUID tag)
    {
        IUnknown* object = nullptr;
        {
            std::scoped_lock lock(m_lock);
            auto it = std::find_if(m_privateData.begin(), m_privateData.end(), [&](const PrivateData& d) { return d.tag == tag; });
            if (it == m_privateData.end())
            {
                return DDERR_NOTFOUND;
            }
            object = it->object;
            m_privateData.erase(it);
        }
        if (object)
        {
            object->Release();
        }
        return DD_OK;
    }

    HRESULT Surface::GetUniquenessValue(LPDWORD value)
    {
        if (!value)
        {
            return DDERR_INVALIDPARAMS;
        }
        *value = m_uniqueness.load(std::memory_order_relaxed);
        return DD_OK;
    }

    HRESULT Surface::ChangeUniquenessValue()
    {
        changed();
        return DD_OK;
    }

    HRESULT Surface::SetPriority(DWORD priority)
    {
        m_priority = priority;
        return DD_OK;
    }

    HRESULT Surface::GetPriority(LPDWORD priority)
    {
        if (!priority)
        {
            return DDERR_INVALIDPARAMS;
        }
        *priority = m_priority;
        return DD_OK;
    }

    HRESULT Surface::SetLOD(DWORD lod)
    {
        m_lod = lod;
        return DD_OK;
    }

    HRESULT Surface::GetLOD(LPDWORD lod)
    {
        if (!lod)
        {
            return DDERR_INVALIDPARAMS;
        }
        *lod = m_lod;
        return DD_OK;
    }

    // ------------------------------------------------------------------------------------------------------------
    // Surface1: the version 1 interface

    HRESULT Surface1::QueryInterface(REFIID riid, LPVOID* out)
    {
        return m_owner.QueryInterface(riid, out);
    }

    ULONG Surface1::AddRef()
    {
        return m_owner.AddRef();
    }

    ULONG Surface1::Release()
    {
        return m_owner.Release();
    }

    HRESULT Surface1::AddAttachedSurface(LPDIRECTDRAWSURFACE surface)
    {
        Surface* s = Surface::from(surface);
        return m_owner.AddAttachedSurface(s);
    }

    HRESULT Surface1::AddOverlayDirtyRect(LPRECT rect)
    {
        return m_owner.AddOverlayDirtyRect(rect);
    }

    HRESULT Surface1::Blt(LPRECT dstRect, LPDIRECTDRAWSURFACE src, LPRECT srcRect, DWORD flags, LPDDBLTFX fx)
    {
        return m_owner.Blt(dstRect, Surface::from(src), srcRect, flags, fx);
    }

    HRESULT Surface1::BltBatch(LPDDBLTBATCH batch, DWORD count, DWORD flags)
    {
        return m_owner.BltBatch(batch, count, flags);
    }

    HRESULT Surface1::BltFast(DWORD x, DWORD y, LPDIRECTDRAWSURFACE src, LPRECT srcRect, DWORD flags)
    {
        return m_owner.BltFast(x, y, Surface::from(src), srcRect, flags);
    }

    HRESULT Surface1::DeleteAttachedSurface(DWORD flags, LPDIRECTDRAWSURFACE surface)
    {
        return m_owner.DeleteAttachedSurface(flags, Surface::from(surface));
    }

    HRESULT Surface1::EnumAttachedSurfaces(LPVOID context, LPDDENUMSURFACESCALLBACK callback)
    {
        if (!callback)
        {
            return DDERR_INVALIDPARAMS;
        }
        EnumThunk1 thunk = {callback, context};
        return m_owner.EnumAttachedSurfaces(&thunk, &EnumThunk1::thunk);
    }

    HRESULT Surface1::EnumOverlayZOrders(DWORD, LPVOID, LPDDENUMSURFACESCALLBACK)
    {
        return DDERR_NOTAOVERLAYSURFACE;
    }

    HRESULT Surface1::Flip(LPDIRECTDRAWSURFACE target, DWORD flags)
    {
        return m_owner.Flip(Surface::from(target), flags);
    }

    HRESULT Surface1::GetAttachedSurface(LPDDSCAPS caps, LPDIRECTDRAWSURFACE* out)
    {
        if (!caps || !out)
        {
            return DDERR_INVALIDPARAMS;
        }
        DDSCAPS2 caps2 = {caps->dwCaps};
        LPDIRECTDRAWSURFACE7 found = nullptr;
        const HRESULT hr = m_owner.GetAttachedSurface(&caps2, &found);
        *out = SUCCEEDED(hr) ? Surface::from(found)->v1() : nullptr;
        return hr;
    }

    HRESULT Surface1::GetBltStatus(DWORD flags)
    {
        return m_owner.GetBltStatus(flags);
    }

    HRESULT Surface1::GetCaps(LPDDSCAPS caps)
    {
        if (!caps)
        {
            return DDERR_INVALIDPARAMS;
        }
        caps->dwCaps = m_owner.m_desc.ddsCaps.dwCaps;
        return DD_OK;
    }

    HRESULT Surface1::GetClipper(LPDIRECTDRAWCLIPPER* out)
    {
        return m_owner.GetClipper(out);
    }

    HRESULT Surface1::GetColorKey(DWORD flags, LPDDCOLORKEY key)
    {
        return m_owner.GetColorKey(flags, key);
    }

    HRESULT Surface1::GetDC(HDC* dc)
    {
        return m_owner.GetDC(dc);
    }

    HRESULT Surface1::GetFlipStatus(DWORD flags)
    {
        return m_owner.GetFlipStatus(flags);
    }

    HRESULT Surface1::GetOverlayPosition(LPLONG x, LPLONG y)
    {
        return m_owner.GetOverlayPosition(x, y);
    }

    HRESULT Surface1::GetPalette(LPDIRECTDRAWPALETTE* out)
    {
        return m_owner.GetPalette(out);
    }

    HRESULT Surface1::GetPixelFormat(LPDDPIXELFORMAT pf)
    {
        return m_owner.GetPixelFormat(pf);
    }

    HRESULT Surface1::GetSurfaceDesc(LPDDSURFACEDESC desc)
    {
        if (!desc || desc->dwSize != sizeof(DDSURFACEDESC))
        {
            return DDERR_INVALIDPARAMS;
        }
        toDesc1(m_owner.m_desc, *desc);
        return DD_OK;
    }

    HRESULT Surface1::Initialize(LPDIRECTDRAW, LPDDSURFACEDESC)
    {
        return DDERR_ALREADYINITIALIZED;
    }

    HRESULT Surface1::IsLost()
    {
        return m_owner.IsLost();
    }

    HRESULT Surface1::Lock(LPRECT rect, LPDDSURFACEDESC desc, DWORD flags, HANDLE event)
    {
        if (!desc || desc->dwSize != sizeof(DDSURFACEDESC))
        {
            return DDERR_INVALIDPARAMS;
        }
        DDSURFACEDESC2 desc2 = {};
        desc2.dwSize = sizeof(desc2);
        const HRESULT hr = m_owner.Lock(rect, &desc2, flags, event);
        if (SUCCEEDED(hr))
        {
            toDesc1(desc2, *desc);
        }
        return hr;
    }

    HRESULT Surface1::ReleaseDC(HDC dc)
    {
        return m_owner.ReleaseDC(dc);
    }

    HRESULT Surface1::Restore()
    {
        return m_owner.Restore();
    }

    HRESULT Surface1::SetClipper(LPDIRECTDRAWCLIPPER clipper)
    {
        return m_owner.SetClipper(clipper);
    }

    HRESULT Surface1::SetColorKey(DWORD flags, LPDDCOLORKEY key)
    {
        return m_owner.SetColorKey(flags, key);
    }

    HRESULT Surface1::SetOverlayPosition(LONG x, LONG y)
    {
        return m_owner.SetOverlayPosition(x, y);
    }

    HRESULT Surface1::SetPalette(LPDIRECTDRAWPALETTE palette)
    {
        return m_owner.SetPalette(palette);
    }

    HRESULT Surface1::Unlock(LPVOID)
    {
        return m_owner.Unlock(nullptr);
    }

    HRESULT Surface1::UpdateOverlay(LPRECT, LPDIRECTDRAWSURFACE, LPRECT, DWORD, LPDDOVERLAYFX)
    {
        return DDERR_NOTAOVERLAYSURFACE;
    }

    HRESULT Surface1::UpdateOverlayDisplay(DWORD flags)
    {
        return m_owner.UpdateOverlayDisplay(flags);
    }

    HRESULT Surface1::UpdateOverlayZOrder(DWORD, LPDIRECTDRAWSURFACE)
    {
        return DDERR_NOTAOVERLAYSURFACE;
    }
}
