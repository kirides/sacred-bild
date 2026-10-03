#pragma once

namespace FrameHooks
{
    // Hooks dxDriver7 init/flip and the world renderer; call inside a Patch transaction.
    void install();
}
