#pragma once
#include <windows.h>
#include <objbase.h>
#include <ddraw.h>
#include <d3d.h>

#include <cstdint>
#include <memory>
#include <vector>

class TextureAtlas;

// Merges pretransformed (XYZRHW) draws. While active, render states, texture stage states, textures and the
// viewport are only recorded. A draw compares what it needs with what the device has; draws that end up with
// identical state are appended to one pending triangle-list DrawIndexedPrimitive, so toggling a state off and
// on again between draws costs nothing. Small textures are used through copies in shared atlas pages (see
// TextureAtlas), so draws with different textures can still merge.
//
// The caller routes the device calls here while active() and calls sync() before anything else that draws or
// depends on the device state. Render states that only affect transformed and lit vertices, transforms,
// lights and materials go straight to the device: the pending draws don't use them.
class Batcher
{
public:
    struct Options
    {
        bool noClip = true;     // submit with D3DDP_DONOTCLIP
        bool vertexBuffers = true;  // submit through vertex buffers instead of user memory
        bool atlas = true;
        int atlasPageSize = 4096;
        int atlasPages = 4;
        int atlasMaxTextureSize = 512;
    };

    Batcher(IDirect3DDevice7* real, IDirectDraw7* ddraw, const Options& options);
    ~Batcher();
    Batcher(const Batcher&) = delete;
    Batcher& operator=(const Batcher&) = delete;

    // Batching scope. end() leaves the device in exactly the state the game set.
    void begin();
    void end();
    bool active() const { return m_active; }

    HRESULT setRenderState(DWORD state, DWORD value);
    HRESULT getRenderState(DWORD state, DWORD* value);
    HRESULT setStageState(DWORD stage, DWORD type, DWORD value);
    HRESULT getStageState(DWORD stage, DWORD type, DWORD* value);
    HRESULT setTexture(DWORD stage, IDirectDrawSurface7* texture);
    HRESULT getTexture(DWORD stage, IDirectDrawSurface7** texture);
    HRESULT setViewport(const D3DVIEWPORT7& vp);
    HRESULT getViewport(D3DVIEWPORT7* vp);

    // A pretransformed draw; `indices` may be null. Triangle lists, strips and fans are batched, anything else
    // is drawn directly.
    HRESULT draw(D3DPRIMITIVETYPE type, DWORD fvf, const void* verts, DWORD vertCount, const WORD* indices,
        DWORD indexCount, DWORD flags);

    enum class Reason
    {
        Texture, RenderState, StageState, Viewport, Format, Full, Direct, Atlas, Other
    };

    // Draws the pending batch and applies everything recorded, with the game's own textures.
    void sync(Reason reason = Reason::Other);
    // The device state was changed outside the batcher (UI scope, state blocks): forget what it knows.
    // Call sync() first.
    void invalidate();

private:
    struct Remap
    {
        UINT offset;
        float scaleU, scaleV, offsetU, offsetV;
    };

    struct Layout
    {
        DWORD fvf = ~0u;
        UINT stride = 0;
        UINT texCount = 0;
        UINT texOffset[8] = {};     // byte offset of each 2D texture coordinate set, 0 if not 2D
    };

    // Ring buffer of vertices for one vertex format, appended with DDLOCK_NOOVERWRITE.
    struct VertexBuffer
    {
        IDirect3DVertexBuffer7* buffer = nullptr;
        DWORD fvf = 0;
        DWORD cursor = 0;
    };

    static constexpr DWORD kStates = 256;
    static constexpr DWORD kStages = 8;
    static constexpr DWORD kStageTypes = 32;

    DWORD renderState(DWORD state);
    DWORD stageState(DWORD stage, DWORD type);
    IDirectDrawSurface7* texture(DWORD stage);
    void drainDestroyed();
    const Layout& layout(DWORD fvf);
    void useAtlas(DWORD stage, IDirectDrawSurface7* texture, const Layout& layout, const void* verts,
        DWORD vertCount, IDirectDrawSurface7*& binding, Remap* remaps, UINT& remapCount);
    bool stateChanged(Reason& reason);
    void applyStates();
    void bindTexture(DWORD stage, IDirectDrawSurface7* texture);
    void submit(Reason reason);
    bool submitVertexBuffer();
    VertexBuffer* vertexBuffer(DWORD fvf);
    void vertexBufferFailed(const char* what, HRESULT hr);
    HRESULT drawDirect(D3DPRIMITIVETYPE type, DWORD fvf, const void* verts, DWORD vertCount, const WORD* indices,
        DWORD indexCount, DWORD flags);
    static void beforeAtlasChange(void* self);

    IDirect3DDevice7* m_real;
    std::unique_ptr<TextureAtlas> m_atlas;
    DWORD m_submitFlags = 0;
    bool m_useAtlas = false;
    IDirect3D7* m_d3d = nullptr;            // creates the vertex buffers; null: submit from user memory
    std::vector<VertexBuffer> m_vertexBuffers;
    uint32_t m_vertexBufferFailures = 0;
    Layout m_layouts[4];
    uint32_t m_nextLayout = 0;
    bool m_active = false;
    uint32_t m_frame = 0;

    // Recorded (game) and applied (device) values. Flags: Known (game value recorded), Applied (device value
    // known), Dirty (in the dirty list).
    DWORD m_rs[kStates] = {}, m_rsDevice[kStates] = {};
    uint8_t m_rsFlags[kStates] = {};
    std::vector<uint16_t> m_rsDirty;
    DWORD m_tss[kStages * kStageTypes] = {}, m_tssDevice[kStages * kStageTypes] = {};
    uint8_t m_tssFlags[kStages * kStageTypes] = {};
    std::vector<uint16_t> m_tssDirty;
    IDirectDrawSurface7* m_tex[kStages] = {};
    IDirectDrawSurface7* m_texDevice[kStages] = {};
    uint8_t m_texFlags[kStages] = {};
    D3DVIEWPORT7 m_vp = {}, m_vpDevice = {};
    uint8_t m_vpFlags = 0;
    std::vector<IDirectDrawSurface7*> m_destroyed;

    // Pending batch.
    DWORD m_fvf = 0, m_flags = 0;
    UINT m_stride = 0;
    DWORD m_vertCount = 0, m_indexCount = 0;
    std::vector<uint8_t> m_verts;
    std::vector<WORD> m_indices;
};
