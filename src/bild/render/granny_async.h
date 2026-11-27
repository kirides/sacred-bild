#pragma once

// GrannyAdvanceTime (granny.dll, animation library) advances every loaded animation once per frame at the start
// of the render loop. It runs on a worker thread instead, overlapping whatever the game does next; every other
// Granny call, from any thread, first waits for it to finish. Granny work keeps its order and never overlaps.
namespace GrannyAsync
{
    // Patches the game's granny.dll imports; call once from DllMain (not inside a Detours transaction).
    void install();

    // After the interface of a frame with the world view: starts the advance the frame asked for ([Render] AnimationAhead).
    void kickAhead();

    // Once per presented frame: logs where the game had to wait for the worker, every few seconds.
    void onFrame();
}
