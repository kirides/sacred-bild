#pragma once

// Statistical profiler: periodically samples one thread's EIP and stack, writes SacredBild-profile.txt.
namespace Profiler
{
    // Starts sampling `threadId`, or switches an active profiler to it.
    void retarget(unsigned long threadId);
    void stop();    // writes the report; safe to call when not running
}
