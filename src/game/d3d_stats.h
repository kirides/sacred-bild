#pragma once
#include <intrin.h>
#include <atomic>
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
        CFlushLighting, // a model batch ends: view/projection, lights, material or T&L render state change
        CFlushWorld,    // a model draw with another world matrix
        CFlushOther,    // end of the world view, Clear, state blocks, ...
        CModelDraw,     // untransformed draws batched (moved to world space)
        CModelVerts,
        CModelDirect,   // untransformed draws in the world view that could not be batched
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

    namespace Detail
    {
        inline std::atomic<uint32_t> counters[CounterCount];
        inline std::atomic<int64_t> times[TimerCount];
    }

    // Called tens of thousands of times per frame. Plain load + store instead of a locked add: nearly every call
    // comes from the render thread, and a count lost to a race with another thread doesn't matter.
    inline void count(Counter c, uint32_t n = 1)
    {
        auto& v = Detail::counters[c];
        v.store(v.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
    }

    inline void addTime(Timer t, int64_t ticks)
    {
        auto& v = Detail::times[t];
        v.store(v.load(std::memory_order_relaxed) + ticks, std::memory_order_relaxed);
    }

    // Accumulated since the last report.
    inline int64_t total(Timer t)
    {
        return Detail::times[t].load(std::memory_order_relaxed);
    }

    inline int64_t now()
    {
        return static_cast<int64_t>(__rdtsc());
    }

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
