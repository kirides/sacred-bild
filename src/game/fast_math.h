#pragma once

// [Render] FastMath: the game's x87 math helpers replaced where they show up in profiles.
// - MSVC's __ftol (float -> integer, ~1.8% of the render thread at 2560x1440): it stores the x87 control word, sets
//   truncation, converts and restores the control word, a pipeline stall each time. SSE3's fisttp truncates
//   regardless of the control word; the function is overwritten with a fisttp version (same result for every input,
//   0x8000000000000000 for NaN and out of range as before).
namespace FastMath
{
    // Code patches; outside a Patch transaction.
    void install();
}
