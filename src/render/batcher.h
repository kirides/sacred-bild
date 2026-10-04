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
// Untransformed (3D model) draws are submitted the same way: gathered from the game's vertex streams into the
// batch and drawn from a vertex buffer, merged when the state and the world matrix match. The world matrix is
// only recorded (applied when a model batch or a direct draw needs it); view, projection, lights, materials and
// T&L-only render states still go straight to the device, after drawing a pending model batch that used the old
// ones. (Moving model vertices to world space on the CPU to merge across world matrices cost more than it saved:
// each character toggles lighting and vertex format between its shadow and its model anyway.)
//
// The caller routes the device calls here while active() and calls sync() before anything else that draws or
// depends on the device state.
class Batcher
{
public:
    struct Options
    {
        bool noClip = true;     // submit pretransformed batches with D3DDP_DONOTCLIP
        bool vertexBuffers = true;  // submit through vertex buffers instead of user memory
        bool models = true;     // batch untransformed draws as well
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

    // An untransformed draw (from strided or interleaved vertices). False if it can't be batched: then the
    // caller calls sync(Reason::Direct) and draws it itself.
    bool drawModel(D3DPRIMITIVETYPE type, DWORD fvf, const D3DDRAWPRIMITIVESTRIDEDDATA& data, DWORD vertCount,
        const WORD* indices, DWORD indexCount, DWORD flags);
    bool drawModel(D3DPRIMITIVETYPE type, DWORD fvf, const void* verts, DWORD vertCount, const WORD* indices,
        DWORD indexCount, DWORD flags);

    HRESULT setTransform(D3DTRANSFORMSTATETYPE type, const D3DMATRIX& m);
    HRESULT getWorld(D3DMATRIX* m);
    HRESULT setMaterial(const D3DMATERIAL7& material);
    HRESULT setLight(DWORD index, const D3DLIGHT7& light);
    HRESULT lightEnable(DWORD index, BOOL enable);
    // Before any other call that changes how untransformed vertices are drawn (clip planes, ...).
    void beforeModelStateChange();

    enum class Reason
    {
        Texture, RenderState, StageState, Viewport, Format, Full, Direct, Atlas, Lighting, World, Other
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
        UINT setOffset[8] = {};     // byte offset and size of every texture coordinate set
        UINT setBytes[8] = {};
        UINT normalOffset = 0, diffuseOffset = 0, specularOffset = 0;  // 0: not present
    };

    enum class Kind : uint8_t
    {
        Pretransformed,     // XYZRHW vertices
        Model,              // untransformed vertices, drawn with the world matrix in m_batchWorld
    };

    // Vertices of one draw: interleaved (pretransformed draws) or strided streams (models).
    struct Source
    {
        const void* interleaved = nullptr;
        const D3DDRAWPRIMITIVESTRIDEDDATA* strided = nullptr;
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

    // What a draw needs from the texture stages: the active ones, their coordinate sets, and whether an atlas copy
    // may stand in for their textures. It only changes with render and stage states, so it is kept for a state
    // epoch (bumped by every recorded stage state change, WRAP0-7 change and invalidate()).
    struct StageSetup
    {
        uint32_t epoch = 0;             // 0: never made
        DWORD fvf = ~0u;
        DWORD stages = 0;
        uint8_t sameSet[kStages] = {};  // other stages that use this stage's coordinate set (bit mask)
        bool atlas[kStages] = {};       // the setup allows a copy (coordinate set, no transform, no wrap, addressing)
        UINT offset[kStages] = {};      // byte offset of the stage's 2D coordinates
        bool clampEdges[kStages] = {};  // gutter for clamp/mirror instead of wrap
    };
    // A texture's atlas copy as placed in a frame and atlas generation (bumped whenever pages change or textures were
    // destroyed): one probe per textured stage instead of the atlas entry, its node and the setup checks.
    struct AtlasBinding
    {
        IDirectDrawSurface7* texture = nullptr;
        uint32_t frame = 0, generation = 0;
        bool clampEdges = false;
        IDirectDrawSurface7* page = nullptr;    // null: kept on the original texture (skip says why)
        uint8_t skip = 0;                       // D3DStats counter
        float marginU = 0, marginV = 0;         // half a texel: coordinates may reach that far beyond the edges
        float scaleU = 1, scaleV = 1, offsetU = 0, offsetV = 0;
    };
    static constexpr size_t kBindings = 512;

    DWORD renderState(DWORD state);
    DWORD stageState(DWORD stage, DWORD type);
    IDirectDrawSurface7* texture(DWORD stage);
    void drainDestroyed();
    const Layout& layout(DWORD fvf);
    static DWORD triangles(D3DPRIMITIVETYPE type, DWORD count);
    void append(Kind kind, D3DPRIMITIVETYPE type, DWORD fvf, const Source& source, DWORD vertCount,
        const WORD* indices, DWORD tris, DWORD flags);
    static void gather(uint8_t* dst, const Layout& layout, const D3DDRAWPRIMITIVESTRIDEDDATA& data, DWORD count);
    const D3DMATRIX& world();
    void bindWorld(const D3DMATRIX& m);
    DWORD tnlRenderState(DWORD state);
    void endModelBatch();   // draws a pending model batch: something it depends on is about to change
    const StageSetup& stageSetup(DWORD fvf, const Layout& layout);
    const AtlasBinding& atlasBinding(IDirectDrawSurface7* texture, bool clampEdges);
    bool stateChanged(Reason& reason);
    void applyStates();
    void bindTexture(DWORD stage, IDirectDrawSurface7* texture);
    void submit(Reason reason);
    // Model batches keep the game's flags: the GPU's T&L clips them either way.
    DWORD submitFlags() const { return m_flags | (m_kind == Kind::Pretransformed ? m_submitFlags : 0); }
    bool submitVertexBuffer();
    VertexBuffer* vertexBuffer(DWORD fvf);
    void vertexBufferFailed(const char* what, HRESULT hr);
    HRESULT drawDirect(D3DPRIMITIVETYPE type, DWORD fvf, const void* verts, DWORD vertCount, const WORD* indices,
        DWORD indexCount, DWORD flags);
    static void beforeAtlasChange(void* self);

    IDirect3DDevice7* m_real;
    std::unique_ptr<TextureAtlas> m_atlas;
    uint32_t m_stateEpoch = 1;
    uint32_t m_atlasGeneration = 1;
    StageSetup m_setups[2];
    uint32_t m_nextSetup = 0;
    AtlasBinding m_bindings[kBindings];
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

    // Untransformed draws: recorded world matrix, and caches of state that is applied right away (so repeating
    // the same value does not end a model batch).
    bool m_batchModels = true;
    D3DMATRIX m_world = {}, m_worldDevice = {};
    uint8_t m_worldFlags = 0;
    DWORD m_tnlRs[kStates] = {};
    bool m_tnlRsKnown[kStates] = {};
    D3DMATERIAL7 m_material = {};
    bool m_materialKnown = false;
    static constexpr DWORD kCachedLights = 8;
    D3DLIGHT7 m_lights[kCachedLights] = {};
    bool m_lightKnown[kCachedLights] = {};
    BOOL m_lightEnabled[kCachedLights] = {};
    bool m_lightEnabledKnown[kCachedLights] = {};
    D3DMATRIX m_batchWorld = {};            // world matrix a pending model batch is drawn with

    // Pending batch.
    Kind m_kind = Kind::Pretransformed;
    DWORD m_fvf = 0, m_flags = 0;
    UINT m_stride = 0;
    DWORD m_vertCount = 0, m_indexCount = 0;
    std::vector<uint8_t> m_verts;
    std::vector<WORD> m_indices;
};
