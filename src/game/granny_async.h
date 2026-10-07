#pragma once

// GrannyAdvanceTime (granny.dll, animation library) advances every loaded animation once per frame at the start
// of the render loop. It runs on a worker thread instead, overlapping whatever the game does next; every other
// Granny call, from any thread, first waits for it to finish. Granny work keeps its order and never overlaps.
namespace GrannyAsync
{
    // Patches the game's granny.dll imports; call once from DllMain (not inside a Detours transaction).
    void install();

    // Once per presented frame: logs where the game had to wait for the worker, every few seconds.
    void onFrame();

    // [Render] EarlyAnimation: the game's advance at the start of a frame is only recorded, and run here, after the
    // world view of the same frame (the last Granny use of the frame's world), so it overlaps the UI, the present and
    // the next frame up to its first character instead of only the latter. Characters show the pose of the frame
    // before (one frame of animation latency); the time advanced adds up the same. Render thread.
    void afterWorld();
}
