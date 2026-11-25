#pragma once
#include "ddraw9/d3d9_api.h"
#include "spin_lock.h"

#include <atomic>
#include <memory>
#include <vector>

namespace DDraw9
{
    class DirectDraw;
    class Surface;

    // The IDirectDrawSurface (version 1) interface of a Surface, for DirectShow's DirectDraw video stream.
    class Surface1 final : public IDirectDrawSurface
    {
    public:
        explicit Surface1(Surface& owner) : m_owner(owner) {}
        Surface& owner() const { return m_owner; }

        STDMETHOD(QueryInterface)(REFIID riid, LPVOID* out) override;
        STDMETHOD_(ULONG, AddRef)() override;
        STDMETHOD_(ULONG, Release)() override;
        STDMETHOD(AddAttachedSurface)(LPDIRECTDRAWSURFACE surface) override;
        STDMETHOD(AddOverlayDirtyRect)(LPRECT rect) override;
        STDMETHOD(Blt)(LPRECT dstRect, LPDIRECTDRAWSURFACE src, LPRECT srcRect, DWORD flags, LPDDBLTFX fx) override;
        STDMETHOD(BltBatch)(LPDDBLTBATCH batch, DWORD count, DWORD flags) override;
        STDMETHOD(BltFast)(DWORD x, DWORD y, LPDIRECTDRAWSURFACE src, LPRECT srcRect, DWORD flags) override;
        STDMETHOD(DeleteAttachedSurface)(DWORD flags, LPDIRECTDRAWSURFACE surface) override;
        STDMETHOD(EnumAttachedSurfaces)(LPVOID context, LPDDENUMSURFACESCALLBACK callback) override;
        STDMETHOD(EnumOverlayZOrders)(DWORD flags, LPVOID context, LPDDENUMSURFACESCALLBACK callback) override;
        STDMETHOD(Flip)(LPDIRECTDRAWSURFACE target, DWORD flags) override;
        STDMETHOD(GetAttachedSurface)(LPDDSCAPS caps, LPDIRECTDRAWSURFACE* out) override;
        STDMETHOD(GetBltStatus)(DWORD flags) override;
        STDMETHOD(GetCaps)(LPDDSCAPS caps) override;
        STDMETHOD(GetClipper)(LPDIRECTDRAWCLIPPER* out) override;
        STDMETHOD(GetColorKey)(DWORD flags, LPDDCOLORKEY key) override;
        STDMETHOD(GetDC)(HDC* dc) override;
        STDMETHOD(GetFlipStatus)(DWORD flags) override;
        STDMETHOD(GetOverlayPosition)(LPLONG x, LPLONG y) override;
        STDMETHOD(GetPalette)(LPDIRECTDRAWPALETTE* out) override;
        STDMETHOD(GetPixelFormat)(LPDDPIXELFORMAT pf) override;
        STDMETHOD(GetSurfaceDesc)(LPDDSURFACEDESC desc) override;
        STDMETHOD(Initialize)(LPDIRECTDRAW ddraw, LPDDSURFACEDESC desc) override;
        STDMETHOD(IsLost)() override;
        STDMETHOD(Lock)(LPRECT rect, LPDDSURFACEDESC desc, DWORD flags, HANDLE event) override;
        STDMETHOD(ReleaseDC)(HDC dc) override;
        STDMETHOD(Restore)() override;
        STDMETHOD(SetClipper)(LPDIRECTDRAWCLIPPER clipper) override;
        STDMETHOD(SetColorKey)(DWORD flags, LPDDCOLORKEY key) override;
        STDMETHOD(SetOverlayPosition)(LONG x, LONG y) override;
        STDMETHOD(SetPalette)(LPDIRECTDRAWPALETTE palette) override;
        STDMETHOD(Unlock)(LPVOID bits) override;
        STDMETHOD(UpdateOverlay)(LPRECT srcRect, LPDIRECTDRAWSURFACE dst, LPRECT dstRect, DWORD flags, LPDDOVERLAYFX fx) override;
        STDMETHOD(UpdateOverlayDisplay)(DWORD flags) override;
        STDMETHOD(UpdateOverlayZOrder)(DWORD flags, LPDIRECTDRAWSURFACE reference) override;

    private:
        Surface& m_owner;
    };

    // A DirectDraw surface on Direct3D 9:
    // - Primary: the window. Blt from the render target (windowed) or Flip (flip chain) presents a frame.
    // - RenderTarget: a render target texture; CPU access (Lock, GetDC) reads it back and writes it back.
    // - Depth: a depth/stencil surface.
    // - Texture: a default-pool texture plus a system-memory copy the CPU locks; changes are uploaded when the
    //   texture is next drawn with. Large video-memory textures (SacredBild's atlas pages) have no copy and are
    //   only written by Blt from other textures.
    // - Memory: system memory (a DIB section where GDI can draw into it).
    class Surface final : public IDirectDrawSurface7
    {
    public:
        enum class Kind
        {
            Primary,
            RenderTarget,
            Depth,
            Texture,
            Memory,
        };

        // Creates the surface `desc` describes, and the back buffers of a flip chain.
        static HRESULT create(DirectDraw* ddraw, const DDSURFACEDESC2& desc, Surface** out);
        // The surface behind an IDirectDrawSurface7 or IDirectDrawSurface pointer, or null if it isn't one of ours.
        static Surface* from(const void* iface);

        Kind kind() const { return m_kind; }
        UINT width() const { return m_desc.dwWidth; }
        UINT height() const { return m_desc.dwHeight; }
        d9::D3DFORMAT format() const { return m_format; }
        DirectDraw* ddraw() const { return m_ddraw; }
        IDirectDrawSurface* v1() { return &m_v1; }

        d9::IDirect3DSurface9* renderTarget() const { return m_rtSurface; }
        d9::IDirect3DSurface9* depthStencil() const { return m_depthSurface; }
        Surface* attachedDepth() const { return m_depth; }
        // The texture to draw with; uploads changes made through the CPU first.
        d9::IDirect3DBaseTexture9* texture();
        bool uploadPending() const { return m_dirty.load(std::memory_order_relaxed); }

        // IUnknown
        STDMETHOD(QueryInterface)(REFIID riid, LPVOID* out) override;
        STDMETHOD_(ULONG, AddRef)() override;
        STDMETHOD_(ULONG, Release)() override;

        // IDirectDrawSurface7
        STDMETHOD(AddAttachedSurface)(LPDIRECTDRAWSURFACE7 surface) override;
        STDMETHOD(AddOverlayDirtyRect)(LPRECT rect) override;
        STDMETHOD(Blt)(LPRECT dstRect, LPDIRECTDRAWSURFACE7 src, LPRECT srcRect, DWORD flags, LPDDBLTFX fx) override;
        STDMETHOD(BltBatch)(LPDDBLTBATCH batch, DWORD count, DWORD flags) override;
        STDMETHOD(BltFast)(DWORD x, DWORD y, LPDIRECTDRAWSURFACE7 src, LPRECT srcRect, DWORD flags) override;
        STDMETHOD(DeleteAttachedSurface)(DWORD flags, LPDIRECTDRAWSURFACE7 surface) override;
        STDMETHOD(EnumAttachedSurfaces)(LPVOID context, LPDDENUMSURFACESCALLBACK7 callback) override;
        STDMETHOD(EnumOverlayZOrders)(DWORD flags, LPVOID context, LPDDENUMSURFACESCALLBACK7 callback) override;
        STDMETHOD(Flip)(LPDIRECTDRAWSURFACE7 target, DWORD flags) override;
        STDMETHOD(GetAttachedSurface)(LPDDSCAPS2 caps, LPDIRECTDRAWSURFACE7* out) override;
        STDMETHOD(GetBltStatus)(DWORD flags) override;
        STDMETHOD(GetCaps)(LPDDSCAPS2 caps) override;
        STDMETHOD(GetClipper)(LPDIRECTDRAWCLIPPER* out) override;
        STDMETHOD(GetColorKey)(DWORD flags, LPDDCOLORKEY key) override;
        STDMETHOD(GetDC)(HDC* dc) override;
        STDMETHOD(GetFlipStatus)(DWORD flags) override;
        STDMETHOD(GetOverlayPosition)(LPLONG x, LPLONG y) override;
        STDMETHOD(GetPalette)(LPDIRECTDRAWPALETTE* out) override;
        STDMETHOD(GetPixelFormat)(LPDDPIXELFORMAT pf) override;
        STDMETHOD(GetSurfaceDesc)(LPDDSURFACEDESC2 desc) override;
        STDMETHOD(Initialize)(LPDIRECTDRAW ddraw, LPDDSURFACEDESC2 desc) override;
        STDMETHOD(IsLost)() override;
        STDMETHOD(Lock)(LPRECT rect, LPDDSURFACEDESC2 desc, DWORD flags, HANDLE event) override;
        STDMETHOD(ReleaseDC)(HDC dc) override;
        STDMETHOD(Restore)() override;
        STDMETHOD(SetClipper)(LPDIRECTDRAWCLIPPER clipper) override;
        STDMETHOD(SetColorKey)(DWORD flags, LPDDCOLORKEY key) override;
        STDMETHOD(SetOverlayPosition)(LONG x, LONG y) override;
        STDMETHOD(SetPalette)(LPDIRECTDRAWPALETTE palette) override;
        STDMETHOD(Unlock)(LPRECT rect) override;
        STDMETHOD(UpdateOverlay)(LPRECT srcRect, LPDIRECTDRAWSURFACE7 dst, LPRECT dstRect, DWORD flags, LPDDOVERLAYFX fx) override;
        STDMETHOD(UpdateOverlayDisplay)(DWORD flags) override;
        STDMETHOD(UpdateOverlayZOrder)(DWORD flags, LPDIRECTDRAWSURFACE7 reference) override;
        STDMETHOD(GetDDInterface)(LPVOID* out) override;
        STDMETHOD(PageLock)(DWORD flags) override;
        STDMETHOD(PageUnlock)(DWORD flags) override;
        STDMETHOD(SetSurfaceDesc)(LPDDSURFACEDESC2 desc, DWORD flags) override;
        STDMETHOD(SetPrivateData)(REFGUID tag, LPVOID data, DWORD size, DWORD flags) override;
        STDMETHOD(GetPrivateData)(REFGUID tag, LPVOID data, LPDWORD size) override;
        STDMETHOD(FreePrivateData)(REFGUID tag) override;
        STDMETHOD(GetUniquenessValue)(LPDWORD value) override;
        STDMETHOD(ChangeUniquenessValue)() override;
        STDMETHOD(SetPriority)(DWORD priority) override;
        STDMETHOD(GetPriority)(LPDWORD priority) override;
        STDMETHOD(SetLOD)(DWORD lod) override;
        STDMETHOD(GetLOD)(LPDWORD lod) override;

    private:
        friend class Surface1;

        struct PrivateData
        {
            GUID tag;
            std::vector<uint8_t> bytes;
            IUnknown* object = nullptr;     // DDSPD_IUNKNOWNPOINTER: released with the entry
        };

        // CPU view of a locked surface.
        struct Mapping
        {
            uint8_t* bits = nullptr;
            int pitch = 0;
        };

        Surface(DirectDraw* ddraw, Kind kind, const DDSURFACEDESC2& desc, d9::D3DFORMAT format);
        ~Surface();
        HRESULT init();
        void changed() { m_uniqueness.fetch_add(1, std::memory_order_relaxed); }

        // CPU access to `rect` (null: all) of the surface; bits point at the rectangle's top left pixel.
        HRESULT map(const RECT* rect, bool readOnly, Mapping& out);
        void unmap();
        // Render target: copies it into m_staging.
        HRESULT readBack();
        bool fullRect(const RECT* in, RECT& out) const;
        HRESULT fill(const RECT& rect, DWORD color);
        HRESULT copyFrom(Surface& src, const RECT& srcRect, const RECT& dstRect, const DDCOLORKEY* key);
        HRESULT uploadToRenderTarget(Surface& src, const RECT& srcRect, const RECT& dstRect);
        HRESULT present(Surface& src, const RECT* srcRect);
        void releaseDcTemp();

        LONG m_refs = 1;
        DirectDraw* m_ddraw;
        Kind m_kind;
        DDSURFACEDESC2 m_desc;
        d9::D3DFORMAT m_format;
        RecursiveSpinLock m_lock;

        // Primary
        Surface* m_backBuffer = nullptr;    // flip chain (holds a reference)
        Surface* m_frontBuffer = nullptr;   // a back buffer's primary (weak)
        std::unique_ptr<uint8_t[]> m_primaryBits;   // what Lock hands out (the game only reads the description)

        // RenderTarget
        d9::IDirect3DTexture9* m_rtTexture = nullptr;
        d9::IDirect3DSurface9* m_rtSurface = nullptr;
        d9::IDirect3DSurface9* m_staging = nullptr;     // system memory copy for Lock and GetDC
        Surface* m_depth = nullptr;                     // attached z-buffer (holds a reference)

        // Depth
        d9::IDirect3DSurface9* m_depthSurface = nullptr;

        // Texture
        bool m_gpuOnly = false;                         // no system memory copy (see create)
        d9::IDirect3DTexture9* m_gpuTexture = nullptr;
        d9::IDirect3DSurface9* m_gpuLevel = nullptr;
        d9::IDirect3DTexture9* m_sysTexture = nullptr;
        d9::IDirect3DSurface9* m_sysLevel = nullptr;
        std::atomic<bool> m_dirty{false};

        // Memory
        HBITMAP m_dib = nullptr;
        uint8_t* m_bits = nullptr;
        int m_pitch = 0;
        std::unique_ptr<uint8_t[]> m_heapBits;

        // Lock / GetDC state
        bool m_locked = false;              // by Lock (map/unmap are also used inside Blt and GetDC)
        bool m_mapped = false;
        bool m_mapReadOnly = false;
        RECT m_mapRect = {};
        HDC m_dc = nullptr;
        HGDIOBJ m_dcOldBitmap = nullptr;
        HBITMAP m_dcTemp = nullptr;         // 32-bit DIB for surfaces GDI can't draw into directly
        uint8_t* m_dcTempBits = nullptr;
        Mapping m_dcMapping;
        bool m_dcStaging = false;           // the DC belongs to m_staging (render target)

        IDirectDrawClipper* m_clipper = nullptr;
        DDCOLORKEY m_colorKeys[4] = {};     // DDCKEY_DESTBLT, DESTOVERLAY, SRCBLT, SRCOVERLAY
        DWORD m_colorKeySet = 0;
        DWORD m_priority = 0, m_lod = 0;
        std::atomic<DWORD> m_uniqueness{1};
        std::vector<PrivateData> m_privateData;
        Surface1 m_v1{*this};
    };
}
