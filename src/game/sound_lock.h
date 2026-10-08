#pragma once

// [Render] SoundLock: the sound system guards its state with a kernel mutex (sacred/sound.h) and the render thread
// takes it for every sound command the object passes issue, several per object and frame: two system calls each
// time (WaitForSingleObject + ReleaseMutex, ~4.5% of the render thread in a profile), and a sleep in the kernel
// whenever the sound thread holds it. The wrapper's lock and unlock are replaced by a user-mode recursive lock
// (SRW lock, which spins briefly before it sleeps); the render thread's waits for it are timed for D3DStats.
namespace SoundLock
{
    // Queues the hooks in the caller's Patch transaction.
    void install();
}
