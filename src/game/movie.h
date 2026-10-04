#pragma once

// Intro and cutscene movies (WMV). The game plays them with DirectShow's multimedia streams (amstream), which decode
// into DirectDraw surfaces and only work on a real DirectDraw. With SacredBild's Direct3D 9 backend they play through
// Media Foundation instead: video into a texture drawn over the whole screen (aspect kept), audio to the default
// device, and the window keeps processing messages.
namespace Movie
{
    // Hooks the game's movie functions; call inside a Patch transaction.
    void install();
}
