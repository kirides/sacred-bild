#pragma once
#include "ddraw9/d3d9_api.h"
#include "ddraw9/skin.h"
#include "spin_lock.h"

#include <nvrhi/nvrhi.h>

#include <array>
#include <memory>
#include <optional>
#include <vector>

namespace DDrawRhi
{
    class DirectDraw;
    class Surface;

    // Device description reported for every Direct3D 7 device type.
    D3DDEVICEDESC7 deviceDescription(REFCLSID type);

    // IDirect3DDevice7 on NVRHI. The device keeps the Direct3D 7 state the game set (Get* never reach the GPU) and draws
    // with one fixed-function shader pair (shaders/ff.hlsl) that this state drives through two constant buffers; what
    // Direct3D 7 keeps in render states that pipelines bake in (blending, depth, culling, the vertex format, the primitive
    // type) selects a cached graphics pipeline. Vertices and indices go into the frame's upload ring for every draw.
    class Device final : public IDirect3DDevice7
    {
    public:
        static HRESULT create(DirectDraw* ddraw, REFCLSID type, Surface* target, Device** out);
        // The device behind an IDirect3DDevice7 pointer if it is one of these, else null.
        static Device* from(const void* iface);

        // GroundMesh's quads (ground.h): `quads` quads of `buffer` from `firstQuad` on, 28-byte DDraw9::Ground::Vertex,
        // drawn with the current state as the game's pretransformed quads at `scale` and `offset` with depth `z`. False
        // (nothing drawn) if the state needs what the ground mode doesn't do.
        bool drawGround(nvrhi::IBuffer* buffer, uint32_t firstQuad, uint32_t quads, float scale, float offsetX,
            float offsetY, float z);
        // GpuSkin's pieces (skin.h): `vertexCount` DDraw9::Skin::Vertex of `vertices`, the triangle list at `firstIndex`
        // of `indices`, skinned by `draw`'s palette and lit with the current state. False (nothing drawn: the caller
        // skins on the CPU) if the state needs what the skinning mode doesn't do.
        bool drawSkinned(nvrhi::IBuffer* vertices, uint32_t vertexCount, nvrhi::IBuffer* indices, uint32_t firstIndex,
            uint32_t indexCount, const DDraw9::Skin::Draw& draw);

        // The world view's own target (world.h): draws go into a color and depth target `scale` times the render
        // target's size until endWorld, which scales it down into the render target.
        // The world view is being drawn (FrameHooks): its 2D art may be filtered ([Render] SpriteFilter).
        void setWorldView(bool on);
        void beginWorld(float scale);
        void endWorld();
        // The render target's texture (null without one).
        nvrhi::ITexture* renderTarget() const;

        // IUnknown
        STDMETHOD(QueryInterface)(REFIID riid, LPVOID* out) override;
        STDMETHOD_(ULONG, AddRef)() override;
        STDMETHOD_(ULONG, Release)() override;

        // IDirect3DDevice7
        STDMETHOD(GetCaps)(LPD3DDEVICEDESC7 desc) override;
        STDMETHOD(EnumTextureFormats)(LPD3DENUMPIXELFORMATSCALLBACK callback, LPVOID context) override;
        STDMETHOD(BeginScene)() override;
        STDMETHOD(EndScene)() override;
        STDMETHOD(GetDirect3D)(LPDIRECT3D7* out) override;
        STDMETHOD(SetRenderTarget)(LPDIRECTDRAWSURFACE7 surface, DWORD flags) override;
        STDMETHOD(GetRenderTarget)(LPDIRECTDRAWSURFACE7* out) override;
        STDMETHOD(Clear)(DWORD count, LPD3DRECT rects, DWORD flags, D3DCOLOR color, D3DVALUE z, DWORD stencil) override;
        STDMETHOD(SetTransform)(D3DTRANSFORMSTATETYPE type, LPD3DMATRIX m) override;
        STDMETHOD(GetTransform)(D3DTRANSFORMSTATETYPE type, LPD3DMATRIX m) override;
        STDMETHOD(SetViewport)(LPD3DVIEWPORT7 vp) override;
        STDMETHOD(MultiplyTransform)(D3DTRANSFORMSTATETYPE type, LPD3DMATRIX m) override;
        STDMETHOD(GetViewport)(LPD3DVIEWPORT7 vp) override;
        STDMETHOD(SetMaterial)(LPD3DMATERIAL7 material) override;
        STDMETHOD(GetMaterial)(LPD3DMATERIAL7 material) override;
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
        STDMETHOD(GetTexture)(DWORD stage, LPDIRECTDRAWSURFACE7* out) override;
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
        static constexpr DWORD kRenderStates = 256;
        static constexpr DWORD kStages = 8;
        static constexpr DWORD kStageStates = 32;   // Direct3D 7 types end at 24 (TEXTURETRANSFORMFLAGS)
        static constexpr DWORD kTransforms = 24;    // Direct3D 7 numbering: WORLD 1, VIEW 2, PROJECTION 3, WORLD1-3 4-6, TEXTURE0-7 16-23
        static constexpr DWORD kClipPlanes = 32;
        static constexpr DWORD kShaderStages = 2;   // texture stages the fixed-function shader does

        struct Light
        {
            D3DLIGHT7 light = {};
            bool set = false;
            BOOL enabled = FALSE;
        };

        // Recorded (BeginStateBlock/EndStateBlock) or captured (CreateStateBlock) state, applied through the
        // Set* methods.
        struct StateBlock
        {
            std::vector<std::pair<DWORD, DWORD>> renderStates;
            std::vector<std::array<DWORD, 3>> stageStates;
            std::vector<std::pair<DWORD, Surface*>> textures;   // holds references
            std::vector<std::pair<DWORD, D3DMATRIX>> transforms;
            std::optional<D3DVIEWPORT7> viewport;
            std::optional<D3DMATERIAL7> material;
            std::vector<std::pair<DWORD, D3DLIGHT7>> lights;
            std::vector<std::pair<DWORD, BOOL>> lightEnables;
            std::vector<std::pair<DWORD, std::array<float, 4>>> clipPlanes;
            ~StateBlock();
        };

        // Where the vertices of a draw are: user memory, or a vertex buffer.
        struct VertexSource
        {
            const uint8_t* data;    // the first vertex used
            DWORD fvf;
            UINT stride;
            DWORD count;            // vertices from `data` on
        };

        explicit Device(DirectDraw* ddraw);
        ~Device();

        HRESULT bindTarget(Surface* target);
        void initState();
        void logRenderState(DWORD state, DWORD value);
        void logStageState(DWORD stage, DWORD type, DWORD value);
        // Records a draw: `indices` (null: none) index the vertices of `source`.
        HRESULT draw(D3DPRIMITIVETYPE type, const VertexSource& source, const WORD* indices, DWORD indexCount);
        // Pretransformed triangles with the same pipeline, bindings, target, viewport and constants as the draw before
        // are held back and drawn as one (the interface's quads, the sprites the batcher can't merge): the triangles
        // keep their order, and anything else recorded draws them first (Context::setPendingFlush).
        bool drawMerged(D3DPRIMITIVETYPE type, const VertexSource& source, const WORD* indices, DWORD indexCount);
    public:
        void flushPending();
    private:
        // Everything but the buffers and the draw call: textures (uploaded), constants, pipeline, framebuffer, viewport
        // and scissor. False if there is nothing to draw into or the pipeline failed.
        bool setup(nvrhi::ICommandList* cmd, DWORD fvf, nvrhi::PrimitiveType prim, nvrhi::GraphicsState& state);
        // The vertex and pixel shader constants from the Direct3D 7 state, written when they changed.
        void updateConstants(nvrhi::ICommandList* cmd, DWORD fvf);
        nvrhi::IGraphicsPipeline* pipeline(DWORD fvf, nvrhi::PrimitiveType type, nvrhi::IFramebuffer* fb);
        nvrhi::IBindingSet* bindings();
        // Interleaves strided vertex data into m_scratch in the layout of `fvf`.
        bool gather(DWORD fvf, const D3DDRAWPRIMITIVESTRIDEDDATA& data, DWORD count);
        void capture(StateBlock& block, D3DSTATEBLOCKTYPE type) const;
        void recapture(StateBlock& block) const;
        void apply(const StateBlock& block);
        StateBlock* stateBlock(DWORD handle);
        Surface* depthSurface() const;

        RecursiveSpinLock& m_lock;              // Context::lock(): one command list for the frame
        LONG m_refs = 1;
        DirectDraw* m_ddraw;
        Surface* m_target = nullptr;            // holds a reference
        bool m_inScene = false;

        // Direct3D 7 state as the game set it.
        DWORD m_rs[kRenderStates] = {};
        DWORD m_tss[kStages][kStageStates] = {};
        Surface* m_textures[kStages] = {};      // hold references
        D3DMATRIX m_transforms[kTransforms] = {};
        D3DVIEWPORT7 m_viewport = {};
        D3DMATERIAL7 m_material = {};
        std::vector<Light> m_lights;
        float m_clipPlanes[kClipPlanes][4] = {};
        D3DCLIPSTATUS m_clipStatus = {};

        // Constants are written again when the state they come from changed or a new command list opened.
        bool m_vsDirty = true;
        bool m_psDirty = true;
        uint32_t m_epoch = 0;
        DWORD m_constantsFvf = ~0u;
        float m_ground[4] = {};                 // the ground mode's transform (drawGround)
        const DDraw9::Skin::Draw* m_skin = nullptr;     // the skinned draw being made (drawSkinned)
        // The world view's target while it is drawn (beginWorld .. endWorld), and its scale.
        nvrhi::TextureHandle m_worldColor, m_worldDepth;
        bool m_inWorld = false;
        bool m_worldView = false;
        bool m_psFilter = false;                // the pixel constants were written with the 2D filter
        float m_worldScale = 1.0f;
        nvrhi::BufferHandle m_boneConstants;
        uint32_t m_paletteId = 0;               // the palette in m_boneConstants (0: none)
        uint32_t m_paletteEpoch = 0;
        std::vector<float> m_palette;
        nvrhi::BufferHandle m_vsConstants;
        nvrhi::BufferHandle m_psConstants;
        std::vector<uint8_t> m_scratch;
        std::vector<WORD> m_indexScratch;
        struct Pending
        {
            bool active = false;
            DWORD fvf = 0, stride = 0;
            std::vector<uint8_t> vertices;
            std::vector<WORD> indices;
            nvrhi::GraphicsState state;     // without buffers
            uint32_t epoch = 0;
        };
        Pending m_pending;
        std::vector<std::unique_ptr<StateBlock>> m_stateBlocks;
        std::unique_ptr<StateBlock> m_recording;
    };
}
