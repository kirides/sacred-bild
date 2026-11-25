#pragma once

// Clicks on far ground. A click walks by path finding, and both path finders (cPathAstar, cPathFloodfill) search a
// 64x64 tile grid around the walker (start - 32 .. start + 31 per tile axis): a target further out on either axis
// finds no path and the hero stays put. At 1024x768 the screen never reached that far; a large, zoomed-out view does
// (its corners lie ~50 tiles out along one tile axis). The hero's path-finding walk orders to such targets are pulled
// back along the line to the hero until they lie 30 tiles out, so the hero heads the way that was clicked.
namespace ClickWalk
{
    // Queues the hook in the caller's Patch transaction.
    void install();
}
