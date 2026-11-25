#pragma once

// Screenshots (Print Screen). The game saves the top-left 1024x768 of the back buffer as Capture\shotNNNN.tga and
// converts that to a JPEG, both with a 1024x768 layout; at other resolutions the pictures come out sheared.
// SacredBild saves the whole screen instead, as PNG or JPEG ([Screenshot] Format), encoded on a worker thread.
namespace Screenshot
{
    // Hooks the game's capture function; call inside a Patch transaction.
    void install();
}
