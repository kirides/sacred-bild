#pragma once
#include <windows.h>
#include <objbase.h>
#include <ddraw.h>
#include <d3d.h>

#include "render/batcher.h"
#include "spin_lock.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// IDirect3DDevice7 wrapper handed to the game instead of the real device. All calls are forwarded;
// draw/state calls are instrumented, UI draws are mapped into the UI canvas and world draws are batched.
// DDrawCompat's shared vtables are never modified.
class DeviceProxy final : public IDirect3DDevice7
{
public:
    // Takes over the caller's reference to `real`. `ddraw` creates the batcher's atlas pages.
    static DeviceProxy* wrap(IDirect3DDevice7* real, IDirectDraw7* ddraw);
    static DeviceProxy* instance();     // the proxy currently handed to the game, if any
    IDirect3DDevice7* real() const { return m_real; }

    // UI canvas mode (see UiCanvas): pretransformed vertices scaled into the calling thread's UI frame. Confined
    // frames: viewport set to the frame's rect, draws outside 1024x768 culled or clipped. Unconfined (cursor,
    // popups): mapped only.
    void beginUi();
    void endUi();
    // The UI frame changed (UiCanvas::FrameScope) while in UI mode.
    void uiFrameChanged();

    // Batching scope (see Batcher), around the world view.
    void beginBatch();
    void endBatch();
    // Draws what is batched and applies recorded state, for code that touches the frame outside the device.
    void syncBatch();

    // The ground's quad batcher (GroundQuads): texture handles for stage 0 and, if not 0, stage 1, looked up with
    // `lookup` in the game's order (stage 0 is set before stage 1 is looked up: a lookup may load and evict), and one
    // pretransformed indexed triangle list. While batching, one call into the batcher; otherwise SetTexture and
    // DrawIndexedPrimitive as the game makes them.
    using TextureLookup = IDirectDrawSurface7* (*)(uint32_t handle);
    HRESULT drawQuads(TextureLookup lookup, uint32_t texture0, uint32_t texture1, DWORD fvf, const void* verts,
        DWORD vertCount, const WORD* indices, DWORD indexCount);

    // Once per presented frame, from the presenting thread: device calls from any other thread are counted
    // (they decide whether the proxy could do without its lock) and logged every few seconds.
    void onPresent(DWORD thread);

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, LPVOID* ppvObj) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;

    // IDirect3DDevice7
    STDMETHOD(GetCaps)(LPD3DDEVICEDESC7 desc) override;
    STDMETHOD(EnumTextureFormats)(LPD3DENUMPIXELFORMATSCALLBACK cb, LPVOID ctx) override;
    STDMETHOD(BeginScene)() override;
    STDMETHOD(EndScene)() override;
    STDMETHOD(GetDirect3D)(LPDIRECT3D7* d3d) override;
    STDMETHOD(SetRenderTarget)(LPDIRECTDRAWSURFACE7 surface, DWORD flags) override;
    STDMETHOD(GetRenderTarget)(LPDIRECTDRAWSURFACE7* surface) override;
    STDMETHOD(Clear)(DWORD count, LPD3DRECT rects, DWORD flags, D3DCOLOR color, D3DVALUE z, DWORD stencil) override;
    STDMETHOD(SetTransform)(D3DTRANSFORMSTATETYPE type, LPD3DMATRIX m) override;
    STDMETHOD(GetTransform)(D3DTRANSFORMSTATETYPE type, LPD3DMATRIX m) override;
    STDMETHOD(SetViewport)(LPD3DVIEWPORT7 vp) override;
    STDMETHOD(MultiplyTransform)(D3DTRANSFORMSTATETYPE type, LPD3DMATRIX m) override;
    STDMETHOD(GetViewport)(LPD3DVIEWPORT7 vp) override;
    STDMETHOD(SetMaterial)(LPD3DMATERIAL7 mat) override;
    STDMETHOD(GetMaterial)(LPD3DMATERIAL7 mat) override;
    STDMETHOD(SetLight)(DWORD index, LPD3DLIGHT7 light) override;
    STDMETHOD(GetLight)(DWORD index, LPD3DLIGHT7 light) override;
    STDMETHOD(SetRenderState)(D3DRENDERSTATETYPE state, DWORD value) override;
    STDMETHOD(GetRenderState)(D3DRENDERSTATETYPE state, LPDWORD value) override;
    STDMETHOD(BeginStateBlock)() override;
    STDMETHOD(EndStateBlock)(LPDWORD handle) override;
    STDMETHOD(PreLoad)(LPDIRECTDRAWSURFACE7 texture) override;
    STDMETHOD(DrawPrimitive)(D3DPRIMITIVETYPE type, DWORD fvf, LPVOID verts, DWORD count, DWORD flags) override;
    STDMETHOD(DrawIndexedPrimitive)(D3DPRIMITIVETYPE type, DWORD fvf, LPVOID verts, DWORD vertCount, LPWORD indices,
        DWORD indexCount, DWORD flags) override;
    STDMETHOD(SetClipStatus)(LPD3DCLIPSTATUS status) override;
    STDMETHOD(GetClipStatus)(LPD3DCLIPSTATUS status) override;
    STDMETHOD(DrawPrimitiveStrided)(D3DPRIMITIVETYPE type, DWORD fvf, LPD3DDRAWPRIMITIVESTRIDEDDATA data, DWORD count,
        DWORD flags) override;
    STDMETHOD(DrawIndexedPrimitiveStrided)(D3DPRIMITIVETYPE type, DWORD fvf, LPD3DDRAWPRIMITIVESTRIDEDDATA data,
        DWORD vertCount, LPWORD indices, DWORD indexCount, DWORD flags) override;
    STDMETHOD(DrawPrimitiveVB)(D3DPRIMITIVETYPE type, LPDIRECT3DVERTEXBUFFER7 vb, DWORD start, DWORD count, DWORD flags) override;
    STDMETHOD(DrawIndexedPrimitiveVB)(D3DPRIMITIVETYPE type, LPDIRECT3DVERTEXBUFFER7 vb, DWORD start, DWORD vertCount,
        LPWORD indices, DWORD indexCount, DWORD flags) override;
    STDMETHOD(ComputeSphereVisibility)(LPD3DVECTOR centers, LPD3DVALUE radii, DWORD count, DWORD flags, LPDWORD result) override;
    STDMETHOD(GetTexture)(DWORD stage, LPDIRECTDRAWSURFACE7* texture) override;
    STDMETHOD(SetTexture)(DWORD stage, LPDIRECTDRAWSURFACE7 texture) override;
    STDMETHOD(GetTextureStageState)(DWORD stage, D3DTEXTURESTAGESTATETYPE type, LPDWORD value) override;
    STDMETHOD(SetTextureStageState)(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) override;
    STDMETHOD(ValidateDevice)(LPDWORD passes) override;
    STDMETHOD(ApplyStateBlock)(DWORD handle) override;
    STDMETHOD(CaptureStateBlock)(DWORD handle) override;
    STDMETHOD(DeleteStateBlock)(DWORD handle) override;
    STDMETHOD(CreateStateBlock)(D3DSTATEBLOCKTYPE type, LPDWORD handle) override;
    STDMETHOD(Load)(LPDIRECTDRAWSURFACE7 dst, LPPOINT dstPoint, LPDIRECTDRAWSURFACE7 src, LPRECT srcRect, DWORD flags) override;
    STDMETHOD(LightEnable)(DWORD index, BOOL enable) override;
    STDMETHOD(GetLightEnable)(DWORD index, BOOL* enable) override;
    STDMETHOD(SetClipPlane)(DWORD index, D3DVALUE* plane) override;
    STDMETHOD(GetClipPlane)(DWORD index, D3DVALUE* plane) override;
    STDMETHOD(GetInfo)(DWORD id, LPVOID info, DWORD size) override;

private:
    explicit DeviceProxy(IDirect3DDevice7* real) : m_real(real) {}

    // Held for every call that touches the proxy's or the batcher's state.
    class CallLock
    {
    public:
        CallLock(DeviceProxy& proxy, void* site);
        ~CallLock() { m_proxy.m_mutex.unlock(); }
        CallLock(const CallLock&) = delete;
        CallLock& operator=(const CallLock&) = delete;

    private:
        DeviceProxy& m_proxy;
    };
    void noteForeignCall(DWORD thread, void* site);

    // Calls go through the batcher: inside a batching scope, outside UI mode and state block recording.
    bool batching() const { return m_batcher && m_batcher->active() && !m_ui && !m_recording; }

    // Copies pretransformed vertices into the UI frame; false if all of them land outside a confined frame.
    bool mapToCanvas(DWORD fvf, const void* verts, DWORD count, const void*& mapped);
    // The mapped quad is an untextured translucent black one over the whole 1024x768 screen, the dimmed background
    // of the escape menu and of message boxes (windows with flag 0x800): draws it over the whole screen instead;
    // false if it is something else.
    bool drawScreenDim(D3DPRIMITIVETYPE type, DWORD fvf, DWORD count, DWORD flags, HRESULT& hr);
    void traceUiDraw(const char* what, DWORD count);
    // Clips an axis-aligned 4-vertex quad (strip/fan) to the UI frame in place; false if it can't.
    bool clipQuad(DWORD fvf, uint8_t* verts);
    // Texture coordinates of a mapped, axis-aligned, textured 4-vertex quad: each pixel samples the stage 0 texture
    // at its middle and the edges stay inside the quad's texels (no seams between UI images under scaling and
    // bilinear filtering). Leaves other quads alone.
    void fitTexels(DWORD fvf, uint8_t* verts);
    // `virt` (1024x768 space) mapped into the UI frame and clamped to it.
    D3DVIEWPORT7 canvasViewport(const D3DVIEWPORT7& virt) const;
    // Unconfined (cursor) mode: 3D draws such as a dragged item model project into 1024x768 and need the
    // frame-mapped viewport; returns false if nothing changed.
    bool beginOverlay3D(D3DVIEWPORT7& restore);
    DWORD uiFilter(DWORD value) const;

    IDirect3DDevice7* m_real;
    LPDIRECTDRAWSURFACE7 m_texture0 = nullptr;
    DWORD m_texture0Width = 0, m_texture0Height = 0;    // 0: not looked up since the last SetTexture(0)
    std::unique_ptr<Batcher> m_batcher;
    bool m_recording = false;               // between BeginStateBlock and EndStateBlock

    RecursiveSpinLock m_mutex;
    DWORD m_presentThread = 0;
    struct ForeignSite
    {
        void* site;
        DWORD thread;
        uint32_t calls;
    };
    std::vector<ForeignSite> m_foreignSites;
    uint32_t m_foreignCalls = 0;
    DWORD m_foreignReportTick = 0;
    bool m_ui = false;
    D3DVIEWPORT7 m_savedViewport = {};      // physical viewport before entering UI mode
    D3DVIEWPORT7 m_uiViewport = {};         // viewport as the UI sees it (virtual 1024x768 space)
    DWORD m_filters[2][2] = {{1, 1}, {1, 1}};   // game's MAG/MIN filter for stages 0/1
    std::vector<uint8_t> m_scratch;
    const void* m_site = nullptr;           // game call site of the current UI draw (diagnostics)
    float m_virtMinX = 0, m_virtMinY = 0, m_virtMaxX = 0, m_virtMaxY = 0;   // its bounds in 1024x768 space

    // GetTransform answered from the last SetTransform: the game reads the matrices back for every 3D model, and
    // each read costs a trip through the runtime, DDrawCompat and the driver. Indexed by D3DTRANSFORMSTATETYPE.
    static constexpr DWORD kTransforms = 24;
    D3DMATRIX m_transforms[kTransforms] = {};
    bool m_transformKnown[kTransforms] = {};
    void forgetTransforms() { std::fill(std::begin(m_transformKnown), std::end(m_transformKnown), false); }
};
