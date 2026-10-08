#pragma once
#include "game/ui_canvas.h"

namespace Sacred
{
    struct cUI_Window2;
}

// In game, the HUD windows move to the screen edges and corners they occupy in the 1024x768 layout (taskbar at
// the bottom, minimap in the top-right corner, inventory bottom-left, ...). Each anchored window keeps its 1024x768
// coordinates and runs in its own UiCanvas frame: its virtual functions (render, events, hit test, show) are
// wrapped so that drawing, cursor reads and mouse events inside them are shifted to where the frame lies. Popups
// (tooltips) take the frame of the code that set their text.
namespace UiAnchor
{
    // Call inside a Patch transaction after UiCanvas::install.
    void install();

    // The frame an anchored window (one of the UI manager's windows, or a popup) runs in; false for any other window.
    bool frame(Sacred::cUI_Window2* window, UiCanvas::Frame& out);
}
