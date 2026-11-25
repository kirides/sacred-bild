#pragma once
#include "ddraw9/d3d9_api.h"

namespace DDraw9
{
    class DirectDraw;

    // The IDirectDraw (version 1) interface of a DirectDraw object, for DirectShow's DirectDraw video stream and
    // DirectDrawCreate.
    class DirectDraw1 final : public IDirectDraw
    {
    public:
        explicit DirectDraw1(DirectDraw& owner) : m_owner(owner) {}

        STDMETHOD(QueryInterface)(REFIID riid, LPVOID* out) override;
        STDMETHOD_(ULONG, AddRef)() override;
        STDMETHOD_(ULONG, Release)() override;
        STDMETHOD(Compact)() override;
        STDMETHOD(CreateClipper)(DWORD flags, LPDIRECTDRAWCLIPPER* out, IUnknown* outer) override;
        STDMETHOD(CreatePalette)(DWORD flags, LPPALETTEENTRY entries, LPDIRECTDRAWPALETTE* out, IUnknown* outer) override;
        STDMETHOD(CreateSurface)(LPDDSURFACEDESC desc, LPDIRECTDRAWSURFACE* out, IUnknown* outer) override;
        STDMETHOD(DuplicateSurface)(LPDIRECTDRAWSURFACE src, LPDIRECTDRAWSURFACE* out) override;
        STDMETHOD(EnumDisplayModes)(DWORD flags, LPDDSURFACEDESC desc, LPVOID context, LPDDENUMMODESCALLBACK callback) override;
        STDMETHOD(EnumSurfaces)(DWORD flags, LPDDSURFACEDESC desc, LPVOID context, LPDDENUMSURFACESCALLBACK callback) override;
        STDMETHOD(FlipToGDISurface)() override;
        STDMETHOD(GetCaps)(LPDDCAPS driverCaps, LPDDCAPS helCaps) override;
        STDMETHOD(GetDisplayMode)(LPDDSURFACEDESC desc) override;
        STDMETHOD(GetFourCCCodes)(LPDWORD count, LPDWORD codes) override;
        STDMETHOD(GetGDISurface)(LPDIRECTDRAWSURFACE* out) override;
        STDMETHOD(GetMonitorFrequency)(LPDWORD frequency) override;
        STDMETHOD(GetScanLine)(LPDWORD line) override;
        STDMETHOD(GetVerticalBlankStatus)(LPBOOL inVerticalBlank) override;
        STDMETHOD(Initialize)(GUID* guid) override;
        STDMETHOD(RestoreDisplayMode)() override;
        STDMETHOD(SetCooperativeLevel)(HWND window, DWORD flags) override;
        STDMETHOD(SetDisplayMode)(DWORD width, DWORD height, DWORD bpp) override;
        STDMETHOD(WaitForVerticalBlank)(DWORD flags, HANDLE event) override;

    private:
        DirectDraw& m_owner;
    };

    // A DirectDraw object (IDirectDraw7) with its Direct3D interface (IDirect3D7) on the Direct3D 9 backend. Display
    // modes are never changed: the game runs in a window (borderless at the desktop size, or whatever SacredBild's
    // resolution patch made of it) and a fullscreen request only changes what the game is told.
    class DirectDraw final : public IDirectDraw7, public IDirect3D7
    {
    public:
        static DirectDraw* create();

        IDirectDraw* v1() { return &m_v1; }
        HWND window() const { return m_window; }
        // The display mode as the game sees it: the desktop, or the mode SetDisplayMode asked for.
        void displayMode(DDSURFACEDESC2& desc) const;

        // IUnknown (shared by both interfaces)
        STDMETHOD(QueryInterface)(REFIID riid, LPVOID* out) override;
        STDMETHOD_(ULONG, AddRef)() override;
        STDMETHOD_(ULONG, Release)() override;

        // IDirectDraw7
        STDMETHOD(Compact)() override;
        STDMETHOD(CreateClipper)(DWORD flags, LPDIRECTDRAWCLIPPER* out, IUnknown* outer) override;
        STDMETHOD(CreatePalette)(DWORD flags, LPPALETTEENTRY entries, LPDIRECTDRAWPALETTE* out, IUnknown* outer) override;
        STDMETHOD(CreateSurface)(LPDDSURFACEDESC2 desc, LPDIRECTDRAWSURFACE7* out, IUnknown* outer) override;
        STDMETHOD(DuplicateSurface)(LPDIRECTDRAWSURFACE7 src, LPDIRECTDRAWSURFACE7* out) override;
        STDMETHOD(EnumDisplayModes)(DWORD flags, LPDDSURFACEDESC2 desc, LPVOID context, LPDDENUMMODESCALLBACK2 callback) override;
        STDMETHOD(EnumSurfaces)(DWORD flags, LPDDSURFACEDESC2 desc, LPVOID context, LPDDENUMSURFACESCALLBACK7 callback) override;
        STDMETHOD(FlipToGDISurface)() override;
        STDMETHOD(GetCaps)(LPDDCAPS driverCaps, LPDDCAPS helCaps) override;
        STDMETHOD(GetDisplayMode)(LPDDSURFACEDESC2 desc) override;
        STDMETHOD(GetFourCCCodes)(LPDWORD count, LPDWORD codes) override;
        STDMETHOD(GetGDISurface)(LPDIRECTDRAWSURFACE7* out) override;
        STDMETHOD(GetMonitorFrequency)(LPDWORD frequency) override;
        STDMETHOD(GetScanLine)(LPDWORD line) override;
        STDMETHOD(GetVerticalBlankStatus)(LPBOOL inVerticalBlank) override;
        STDMETHOD(Initialize)(GUID* guid) override;
        STDMETHOD(RestoreDisplayMode)() override;
        STDMETHOD(SetCooperativeLevel)(HWND window, DWORD flags) override;
        STDMETHOD(SetDisplayMode)(DWORD width, DWORD height, DWORD bpp, DWORD refreshRate, DWORD flags) override;
        STDMETHOD(WaitForVerticalBlank)(DWORD flags, HANDLE event) override;
        STDMETHOD(GetAvailableVidMem)(LPDDSCAPS2 caps, LPDWORD total, LPDWORD free) override;
        STDMETHOD(GetSurfaceFromDC)(HDC dc, LPDIRECTDRAWSURFACE7* out) override;
        STDMETHOD(RestoreAllSurfaces)() override;
        STDMETHOD(TestCooperativeLevel)() override;
        STDMETHOD(GetDeviceIdentifier)(LPDDDEVICEIDENTIFIER2 id, DWORD flags) override;
        STDMETHOD(StartModeTest)(LPSIZE modes, DWORD count, DWORD flags) override;
        STDMETHOD(EvaluateMode)(DWORD flags, DWORD* timeout) override;

        // IDirect3D7
        STDMETHOD(EnumDevices)(LPD3DENUMDEVICESCALLBACK7 callback, LPVOID context) override;
        STDMETHOD(CreateDevice)(REFCLSID type, LPDIRECTDRAWSURFACE7 target, LPDIRECT3DDEVICE7* out) override;
        STDMETHOD(CreateVertexBuffer)(LPD3DVERTEXBUFFERDESC desc, LPDIRECT3DVERTEXBUFFER7* out, DWORD flags) override;
        STDMETHOD(EnumZBufferFormats)(REFCLSID type, LPD3DENUMPIXELFORMATSCALLBACK callback, LPVOID context) override;
        STDMETHOD(EvictManagedTextures)() override;

    private:
        DirectDraw() = default;
        ~DirectDraw() = default;

        LONG m_refs = 1;
        HWND m_window = nullptr;
        DWORD m_modeWidth = 0, m_modeHeight = 0, m_modeBpp = 0;    // SetDisplayMode (not applied)
        DirectDraw1 m_v1{*this};
    };

    // A clipper only remembers its window: frames always go to the whole client area.
    class Clipper final : public IDirectDrawClipper
    {
    public:
        static Clipper* create();

        STDMETHOD(QueryInterface)(REFIID riid, LPVOID* out) override;
        STDMETHOD_(ULONG, AddRef)() override;
        STDMETHOD_(ULONG, Release)() override;
        STDMETHOD(GetClipList)(LPRECT rect, LPRGNDATA list, LPDWORD size) override;
        STDMETHOD(GetHWnd)(HWND* window) override;
        STDMETHOD(Initialize)(LPDIRECTDRAW ddraw, DWORD flags) override;
        STDMETHOD(IsClipListChanged)(BOOL* changed) override;
        STDMETHOD(SetClipList)(LPRGNDATA list, DWORD flags) override;
        STDMETHOD(SetHWnd)(DWORD flags, HWND window) override;

    private:
        Clipper() = default;
        ~Clipper() = default;

        LONG m_refs = 1;
        HWND m_window = nullptr;
    };

    // Converts between the two surface description versions (the version 1 caps are the first DWORD of DDSCAPS2).
    void toDesc2(const DDSURFACEDESC& in, DDSURFACEDESC2& out);
    void toDesc1(const DDSURFACEDESC2& in, DDSURFACEDESC& out);
}
