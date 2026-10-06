#pragma once
#include "input/bindings.h"
#include "input/gamepad.h"

#include <cstdint>

namespace d9
{
    struct IDirect3DDevice9Ex;
}

// Controller button prompts drawn over the game: icons of the pad's buttons (in the labels of its maker, from the
// atlas tools/gen_prompts.py builds into src/overlay/prompts.png) at screen positions, all in one draw call right
// before the frame is presented (Direct3D 9 backend only). The presenting thread collects a frame's prompts after
// presenting it (Controller), the next present draws them.
namespace Prompts
{
    // Besides Gamepad::Button: the sticks themselves (moved, not clicked).
    constexpr uint32_t LeftStick = 1u << 16;
    constexpr uint32_t RightStick = 1u << 17;

    // Starts a frame's prompts, in the labels of `style`.
    void begin(Gamepad::Style style);
    // A binding's icons (modifier, "+", button) `size` pixels high, placed so that (x, y) lies at (ax, ay) of their
    // bounds (0, 0 top-left, 1, 1 bottom-right); `alpha` 0..1. Returns their width, 0 for an empty binding.
    float add(Bindings::Binding binding, float x, float y, float size, float ax, float ay, float alpha = 1.0f);
    // A binding stacked upright (modifier on top, "+", button), centered on x with its bottom at y; for slots too
    // close together for side by side.
    void addStacked(Bindings::Binding binding, float x, float bottom, float size, float alpha = 1.0f);
    // How wide add would draw `binding`.
    float width(Bindings::Binding binding, float size);
    // Hands the frame's prompts to the next present.
    void end();

    // The overlay: draws the latest prompts (unless they are stale) into the current render target.
    void draw(d9::IDirect3DDevice9Ex* device, float width, float height);
    bool pending();
}
