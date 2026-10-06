#pragma once
#include <cstdint>
#include <string_view>

// Game controllers through SDL3 (linked in), which runs on a thread of its own: Xbox pads through raw input and
// XInput, PlayStation and Switch pads through their HID drivers; under Wine through Wine's XInput. Read once per frame
// by the thread presenting frames (the readings are a frame old); of several pads, the one that last gave input is
// used. Buttons are named by their place on an Xbox pad.
namespace Gamepad
{
    enum Button : uint32_t
    {
        A = 1u << 0,
        B = 1u << 1,
        X = 1u << 2,
        Y = 1u << 3,
        LB = 1u << 4,
        RB = 1u << 5,
        LT = 1u << 6,       // triggers count as buttons past a third of their travel
        RT = 1u << 7,
        Back = 1u << 8,
        Start = 1u << 9,
        L3 = 1u << 10,
        R3 = 1u << 11,
        Up = 1u << 12,      // D-pad
        Down = 1u << 13,
        Left = 1u << 14,
        Right = 1u << 15,
    };
    constexpr int buttonCount = 16;

    struct State
    {
        bool connected = false;
        uint32_t buttons = 0;
        // Sticks with the deadzone taken out (radial, rescaled to 0..1 beyond it), y up.
        float lx = 0.0f, ly = 0.0f;
        float rx = 0.0f, ry = 0.0f;
        float lt = 0.0f, rt = 0.0f;     // 0..1
    };

    // Reads the pads; once per frame.
    void poll();
    const State& state();
    uint32_t pressed();     // buttons that went down in the last poll
    uint32_t released();    // ... and up
    // Input in the last poll: a button held, or a stick out of its deadzone.
    bool active();

    // Rumbles the pads for `milliseconds` at `strength` (0..1), replacing a rumble still running.
    void rumble(float strength, uint32_t milliseconds);

    // Fraction of a stick's travel ignored around its center (0..0.9).
    void setDeadzone(float deadzone);

    // "A", "LB", "Up", ...; nullptr for anything but a single button.
    const char* name(uint32_t button);
    // The button with that name (case-insensitive), 0 if there is none.
    uint32_t fromName(std::string_view name);
}
