#pragma once

// [Debug] in SacredBild.ini (read by config.cpp): diagnostics.
namespace Config
{
    struct Debug
    {
        bool d3dStats = false;         // per-second D3D7 call counts in the log
        bool profiler = false;         // sample the render thread, dump hot spots on exit
        int profilerIntervalUs = 500;
        bool uiTrace = false;          // Scroll Lock logs one UI frame's draws with their frames and callers
        // Minidump next to the exe when the game crashes: 0 = off, 1 = stacks and the memory they point to, 2 = all memory.
        int crashDump = 1;
        // Movies always through the fallback (DirectShow into a system memory surface) instead of Media Foundation.
        bool movieFallback = false;
        // Compare Granny's CPU skinning with per-vertex weights (groundwork for skinning on the GPU).
        bool skinCheck = false;
        // With [Render] AnimationThreads: every 2 s the controls are sampled on one thread and on the threads from the
        // same state and the results compared; a difference keeps the walk on one thread.
        bool animationCheck = false;
    };
    inline Debug debug;
}
