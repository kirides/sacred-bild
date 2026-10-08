#pragma once

namespace Sacred
{
    struct cEngine;
    struct dxDriver7;
}

namespace FrameHooks
{
    // Hooks dxDriver7 init/flip and the world renderer; call inside a Patch transaction.
    void install();

    // Presents the back buffer the way the game does (dxDriver7::flip, frame statistics included). False before
    // dxDriver7::init ran.
    bool flip();

    // The game's dxDriver7 instance; nullptr before dxDriver7::init ran.
    Sacred::dxDriver7* dxDriver();

    // The cEngine running the in-game render loop; nullptr before the first game started.
    Sacred::cEngine* engine();
}
