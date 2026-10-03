#pragma once
#include <cstdint>
#include <string>

// Per-frame counters and phase timers; logs a summary about once per second.
namespace D3DStats
{
    enum Counter
    {
        CDraw,
        CDrawIndexed,
        CDrawVB,
        CVerts,
        CDrawTL,        // pretransformed (XYZRHW) draws
        CDrawQuad,      // DrawPrimitive strip/fan with 4 vertices (one sprite)
        CUniqueTex,     // distinct stage-0 textures used this frame
        CSetTexture,
        CTexSwitch,     // SetTexture(stage 0) with a different texture than before
        CRenderState,
        CStageState,
        CTransform,
        CClear,
        CLockBack,      // dxDriver7::lockBack: CPU drawing into the frame
        // Batcher
        CSubmit,        // draw calls that reached the real device
        CMerged,        // game draws appended to a pending batch
        CAtlasDraw,     // batched draws that sample an atlas page
        // Textured stages kept on the original texture because ...
        CAtlasRange,    // coordinates beyond the edges
        CAtlasSkipTexture,  // the texture can't be copied (size, format, mipmaps, changes too often)
        CAtlasSkipSetup,    // coordinate set, texture transform, wrap render state or mixed addressing
        CAtlasSkipShared,   // two stages share a coordinate set
        CAtlasSkipFull,     // no room in the pages this frame (or the copy failed)
        CFlushPages,        // batches ended by texture: from one atlas page to another
        CFlushOriginal,     // batches ended by texture: an original texture on either side
        CAtlasUpload,   // texture copies into atlas pages
        CAtlasReset,    // atlas pages emptied for reuse
        CFlushTexture,  // pending batch drawn because the next draw needs ...: other textures
        CFlushRenderState,
        CFlushStageState,
        CFlushViewport,
        CFlushFormat,   // another vertex format or draw flags
        CFlushFull,
        CFlushDirect,   // a draw that can't be batched (3D, vertex buffer, lines)
        CFlushAtlas,    // an atlas page is about to change
        CFlushOther,    // end of the world view, Clear, state blocks, ...
        CounterCount
    };

    enum Timer
    {
        TFrame,
        TWorld,         // cWorldView0::render
        TUi,            // cUI_Manager::render
        TFlip,
        TDraw,          // inside DrawPrimitive*
        TState,         // inside SetTexture/SetRenderState/SetTextureStageState/SetTransform
        TLockBack,
        TProxy,         // inside the device proxy's draw and state methods, D3D included
        TWorldProxy,    // the part of TProxy spent inside cWorldView0::render
        TimerCount
    };

    void count(Counter c, uint32_t n = 1);
    void addTime(Timer t, int64_t ticks);
    int64_t total(Timer t);     // accumulated since the last report
    int64_t now();

    void setRenderThread(unsigned long threadId);
    void onFrame();

    // "private N MB, address space N MB used, largest free N MB" (the game is not large address aware).
    std::string memorySummary();

    // Called by the device proxy on SetTexture(0, ...); tracks per-frame uniqueness and texture sizes.
    void onTexture(void* surface);

    // Detail histograms, collected in one frame per second and logged every ~30 s.
    void onDraw(uint32_t primitiveType, uint32_t fvf, uint32_t vertexCount, bool indexed);
    void onRenderState(uint32_t state, uint32_t value);
    void onStageState(uint32_t stage, uint32_t type, uint32_t value);
    // State that ended a batch: render state number, or 0x10000 | stage << 8 | stage state type.
    void onFlushCause(uint32_t key);

    struct Scope
    {
        Timer timer;
        int64_t start = now();
        ~Scope() { addTime(timer, now() - start); }
    };
}
