#pragma once

// Draw-list take-over, static objects (docs/NvrhiRenderer.md stage 7): the world view's object pass (objects2,
// 0x62FF60) walks five draw lists; for an entry of a static object (kind 1: buildings, trees, props) the game picks the
// sprite's animation frame, places, culls, lights and fades its quads and feeds them to its sprite batcher. With the
// NVRHI backend SacredBild does that itself from the same data (the static object, its sprite descriptor, the frame's
// quads) and hands the quads to its batcher, in the list's order. The game's code for that entry is jumped over; every
// other entry kind is still the game's. Off, or on another backend, or where the code there isn't the expected one
// (another build), the game's own code runs.
namespace WorldStatics
{
    void install();
}
