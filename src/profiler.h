#pragma once

// Statistical profiler: periodically samples one thread's EIP and stack, writes SacredBild-profile.txt.
namespace Profiler
{
    // Starts sampling `threadId`, or switches an active profiler to it.
    void retarget(unsigned long threadId);
    // Also samples `threadId` (one extra thread, e.g. a worker), from when the profiler runs.
    void addThread(unsigned long threadId);
    void stop();    // writes the report; safe to call when not running
}
