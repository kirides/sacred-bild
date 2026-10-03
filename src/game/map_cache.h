#pragma once

// The map data's record caches look every id up in a std::map with up to 0x8000 nodes: a red-black tree walk per
// ground tile and object per frame, ~6 % of the render thread zoomed out. A hash index in front answers hits;
// misses still go to the game's function (file read, insert, eviction), which stays the owner of the records.
namespace MapCache
{
    // Queues the hooks in the caller's Patch transaction.
    void install();
}
