#pragma once

struct IDirect3DDevice7;

// The world view's own target on the NVRHI backend ([Render] WorldScale): FrameHooks brackets cWorldView0::render with
// begin and end; between them the device draws into a target `scale` times the frame's size, which end scales down into
// the frame before the interface. Nothing happens for other devices or a scale of 1.
namespace DDrawRhi::World
{
    void begin(IDirect3DDevice7* device, float scale);
    void end(IDirect3DDevice7* device);
}
