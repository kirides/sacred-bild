#pragma once

namespace FrameHooks
{
    // Hooks dxDriver7 init/flip and the world renderer; call inside a Patch transaction.
    void install();

    // Presents the back buffer the way the game does (dxDriver7::flip, frame statistics included). False before
    // dxDriver7::init ran.
    bool flip();
}
