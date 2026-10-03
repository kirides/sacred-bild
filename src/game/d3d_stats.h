#pragma once
#include <cstdint>

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
        TimerCount
    };

    void count(Counter c, uint32_t n = 1);
    void addTime(Timer t, int64_t ticks);
    int64_t now();

    void setRenderThread(unsigned long threadId);
    void onFrame();

    // Called by the device proxy on SetTexture(0, ...); tracks per-frame uniqueness and texture sizes.
    void onTexture(void* surface);

    // Detail histograms for the batching design, logged every ~30 s.
    void onDraw(uint32_t primitiveType, uint32_t fvf, uint32_t vertexCount, bool indexed);
    void onRenderState(uint32_t state, uint32_t value);
    void onStageState(uint32_t stage, uint32_t type, uint32_t value);

    struct Scope
    {
        Timer timer;
        int64_t start = now();
        ~Scope() { addTime(timer, now() - start); }
    };
}
