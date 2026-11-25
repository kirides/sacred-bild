#pragma once

// The engine hooks of Sacred Gear. M1: the per-frame tick of the in-game loop, which drives the Lua host.
namespace Hooks
{
    // After the addresses resolved. False if a hook could not be installed (then none is).
    bool install();
}
