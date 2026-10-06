#pragma once

// The game's Options window as SacredBild's own screen (Overlay) while the controller is in use: the same settings,
// laid out for gamepad navigation, plus the controller's settings and bindings. The game's window still opens (its
// show fills its controls from the settings) but is neither drawn nor given input; the screen reads its controls,
// and Accept writes them back and presses the window's OK button, so the game saves and applies them as always
// (Cancel presses Cancel). With the keyboard and mouse the game's own window opens as before.
namespace OptionsScreen
{
    // Wraps the options window's vtable ([Controller] Enabled and [DDraw] Backend=d3d9 only).
    void install();
}
