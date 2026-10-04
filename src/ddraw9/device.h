#pragma once
#include "ddraw9/d3d9_api.h"
#include "ddraw9/skin.h"
#include "spin_lock.h"

#include <array>
#include <memory>
#include <optional>
#include <vector>

namespace DDraw9
{
    class DirectDraw;
    class Surface;

    // Device description reported for every Direct3D 7 device type (all map to the Direct3D 9 HAL device).
    D3DDEVICEDESC7 deviceDescription(REFCLSID type);

    // IDirect3DDevice7 on the Direct3D 9 device. Both are fixed-function pipelines with the same vertex formats,
    // render state and texture stage state numbers (Direct3D 9 moved the texture addressing and filtering states
    // to sampler states), lights, materials and viewports, so most calls translate one to one. The device keeps
    // the Direct3D 7 state the game set (Get* never reaches Direct3D 9) and the Direct3D 9 state it applied
    // (repeated values are not sent again).
    class Device final : public IDirect3DDevice7
    {
    public:
        static HRESULT create(DirectDraw* ddraw, REFCLSID type, Surface* target, Device** out);
        // The device behind an IDirect3DDevice7 pointer if it is one of these, else null.
        static Device* from(const void* iface);

        // GPU skinning (device_skin.cpp, see skin.h).
        bool skinAvailable();
        Skin::Mesh* createSkinMesh(const Skin::Vertex* vertices, uint32_t count);
        Skin::Indices* createSkinIndices(const WORD* indices, uint32_t count);
        bool drawSkinned(const Skin::Draw& draw);

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
        static constexpr DWORD kSamplerStates = 14;
        static constexpr DWORD kTransforms = 24;    // Direct3D 7 numbering: WORLD 1, VIEW 2, PROJECTION 3, WORLD1-3 4-6, TEXTURE0-7 16-23
        static constexpr DWORD kClipPlanes = 32;

        struct Light
        {
            D3DLIGHT7 light = {};
            bool set = false;
            BOOL enabled = FALSE;
            bool lightDirty = false;    // not yet given to Direct3D 9 (applyFixedFunction)
            bool enableDirty = false;
        };

        // Recorded (BeginStateBlock/EndStateBlock) or captured (CreateStateBlock) state, applied through the
        // Set* methods so both state copies stay right.
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

        Device(DirectDraw* ddraw, d9::IDirect3DDevice9Ex* dev);
        ~Device();

        HRESULT bindTarget(Surface* target);
        void initState();
        void setRenderState9(DWORD state, DWORD value);
        void setStageState9(DWORD stage, DWORD type, DWORD value);
        void setSamplerState9(DWORD stage, DWORD type, DWORD value);
        HRESULT applyRenderState(DWORD state, DWORD value);
        HRESULT applyStageState(DWORD stage, DWORD type, DWORD value);
        HRESULT applyTransform(DWORD type, const D3DMATRIX& m);
        // Transforms, material and lights reach Direct3D 9 only before a fixed-function draw: skinned draws read them
        // from the recorded state, and a character sets them for every piece.
        void applyFixedFunction();
        void syncDepth();
        // Before every fixed-function draw: z-buffer, vertex format, and textures with CPU changes uploaded.
        void prepare(DWORD fvf);
        // The z-buffer and texture part of it (skinned draws set their own vertex format).
        void prepareTarget();
        void bindVertexBuffer(d9::IDirect3DVertexBuffer9* vb, UINT stride);
        // Copies `indices` into the dynamic index buffer; returns the start index or -1.
        int uploadIndices(const WORD* indices, DWORD count);
        // Interleaves strided vertex data into m_scratch in the layout of `fvf`.
        bool gather(DWORD fvf, const D3DDRAWPRIMITIVESTRIDEDDATA& data, DWORD count);
        void capture(StateBlock& block, D3DSTATEBLOCKTYPE type) const;
        // Creates the skinning shaders and declarations once; false if the device can't run them.
        bool initSkin();
        // Fills the shader constants from the Direct3D 7 state; false if the state needs something the shader doesn't do.
        bool skinConstants(const Skin::Draw& draw, bool diffuse);
        void releaseSkin();
        void recapture(StateBlock& block) const;
        void apply(const StateBlock& block);
        StateBlock* stateBlock(DWORD handle);

        RecursiveSpinLock m_lock;
        LONG m_refs = 1;
        DirectDraw* m_ddraw;
        d9::IDirect3DDevice9Ex* m_dev;
        Surface* m_target = nullptr;            // holds a reference
        Surface* m_depth = nullptr;             // the target's z-buffer as bound to Direct3D 9 (the target holds it)
        bool m_inScene = false;

        // Direct3D 7 state as the game set it.
        DWORD m_rs[kRenderStates] = {};
        DWORD m_tss[kStages][kStageStates] = {};
        Surface* m_textures[kStages] = {};      // hold references
        D3DMATRIX m_transforms[kTransforms] = {};
        D3DVIEWPORT7 m_viewport = {};
        D3DMATERIAL7 m_material = {};
        std::vector<Light> m_lights;
        uint32_t m_transformsDirty = 0;         // bit per transform type set since the last fixed-function draw
        bool m_materialDirty = false;
        bool m_lightsDirty = false;
        float m_clipPlanes[kClipPlanes][4] = {};
        D3DCLIPSTATUS m_clipStatus = {};

        // Direct3D 9 state as applied.
        DWORD m_rs9[kRenderStates] = {};
        bool m_rs9Known[kRenderStates] = {};
        DWORD m_tss9[kStages][kStageStates + 4] = {};
        bool m_tss9Known[kStages][kStageStates + 4] = {};
        DWORD m_samp9[kStages][kSamplerStates] = {};
        bool m_samp9Known[kStages][kSamplerStates] = {};
        d9::IDirect3DBaseTexture9* m_textures9[kStages] = {};
        d9::D3DVIEWPORT9 m_viewport9 = {};      // clamped to the render target
        DWORD m_fvf9 = 0;
        d9::IDirect3DVertexBuffer9* m_vb9 = nullptr;
        UINT m_vbStride9 = 0;
        d9::IDirect3DIndexBuffer9* m_ib9Bound = nullptr;
        DWORD m_maxStage = 0;                   // stages above have no texture

        // Index buffer ring for DrawIndexedPrimitiveVB (Direct3D 9 draws from vertex buffers need indices in one).
        d9::IDirect3DIndexBuffer9* m_indexBuffer = nullptr;
        UINT m_indexCapacity = 0, m_indexCursor = 0;

        // GPU skinning.
        int m_skinState = 0;                    // 0 not tried, 1 ready, -1 unavailable
        d9::IDirect3DVertexShader9* m_skinShaders[2] = {};      // without / with a diffuse stream
        d9::IDirect3DVertexDeclaration9* m_skinDecls[2] = {};
        d9::IDirect3DVertexBuffer9* m_skinDiffuse = nullptr;    // ring of per-vertex diffuse colors
        UINT m_skinDiffuseCursor = 0;
        uint32_t m_skinPaletteId = 0;           // palette in the bone constants (0: none)
        std::vector<float> m_skinPalette;       // its values
        d9::IDirect3DIndexBuffer9* m_skinArena = nullptr;   // index arena new pieces go into
        UINT m_skinArenaUsed = 0, m_skinArenaCapacity = 0;
        // The skinning shader stays bound until the next fixed-function draw (prepare): a character's pieces and
        // its shadow are consecutive skinned draws.
        bool m_skinBound = false;
        int m_skinBoundVariant = -1;
        // Constants as last uploaded, to skip repeats.
        float m_skinConstants[73][4] = {};
        UINT m_skinConstantCount = 0;
        BOOL m_skinFlags[5] = {};
        int m_skinLightCount = -1;

        std::vector<uint8_t> m_scratch;
        std::vector<std::unique_ptr<StateBlock>> m_stateBlocks;
        std::unique_ptr<StateBlock> m_recording;
    };
}
