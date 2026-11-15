#pragma once
#include "text.h"

#include <cstdint>
#include <string>
#include <vector>

// Mod drawing: rectangles, text in the game's fonts and the game's UI images, drawn by the game's own UI pass.
//
// Mods record draw commands while their handlers run (the Lua thread: the engine thread in game, the automation
// thread in the menus); commit() publishes the frame's commands and the render hooks replay the latest published
// ones inside cUI_Manager_render, with the device the game draws its UI with. So the overlay goes through whatever
// that device is: the game's Direct3D 7 device, or SacredBild's d3d9 backend (whose proxy maps the UI's 1024x768
// layout into its canvas and keeps its own state shadow, so nothing here goes around it), and nothing depends on
// SacredBild. Every render state, texture stage state and texture the drawing changes is read before and set back
// after, so the game's own state caches stay right.
//
// Layers, in the order the manager draws: World (in game, right after the world, before any window), Hud (after
// the windows, before tooltips, the escape menu and the message box; also in the menus), Top (after everything but
// the cursor tooltip and the cursor). Coordinates are the UI's 1024x768 layout, the space gear.auto.ui() reports.
namespace Ui
{
    enum class Layer : uint8_t { World, Hud, Top, Count };
    // Region: draws nothing; a rectangle that takes the mouse (see input below).
    enum class Kind : uint8_t { Rect, Text, Image, Region };
    enum class Fit : uint8_t { Stretch, Contain, Cover };

    struct Command
    {
        Kind kind = Kind::Rect;
        Layer layer = Layer::Hud;
        int8_t window = -1;         // draw in this in-game window's UI frame (slot 0..22), -1: the canvas
        Fit fit = Fit::Stretch;
        float x = 0, y = 0, w = 0, h = 0;
        uint32_t color = 0xFFFFFFFF;    // ARGB; for images a tint (white: as is)
        // Text
        std::wstring text;
        uint16_t font = 0;
        uint32_t flags = 0;         // the font's: 1 centered, 4 right, 0x20 curved (see docs/Gear.md)
        uint8_t valign = 0;         // in the height: 0 top, 1 middle, 2 bottom, 3 y is the baseline (vector fonts)
        bool vector = false;        // a vector font (style), drawn for the screen's pixels, instead of game font `font`
        Text::Style style;
        uint32_t background = 0;    // ARGB, 0: none; a rect behind the measured text
        float padding = 0;
        // Image
        uint32_t image = 0;
        // Region
        uint32_t region = 0;        // the kernel's handle, reported with the events
    };

    // Input. A mouse button pressed over a region (the last one registered on the highest layer wins) is taken
    // from the game: the UI manager reports it handled, so neither the game's windows nor the world see it, and the
    // release that follows is taken too. Regions of the world and hud layers take nothing while the escape menu or a
    // message box is up. Events wait for the next tick.
    struct InputEvent
    {
        bool down = false;          // pressed, else released
        int button = 0;             // the event's +4 (0: left)
        int x = 0, y = 0;           // UI layout coordinates
        uint32_t region = 0;        // the region pressed (a release: the one its press went to)
    };
    std::vector<InputEvent> takeEvents();

    // Whether virtual key `vk` is held, read from the keyboard while the game's window has the focus (false
    // otherwise, so keys typed into other programs do not count).
    bool keyDown(int vk);

    // Lua thread: adds a command to the frame being recorded (false when the frame is full).
    bool add(Command&& command);
    // Lua thread: publishes the recorded frame (and starts the next). A published frame is drawn until the next
    // commit, but not after half a second without one (no mod tick: the game left for the menus, a loading screen).
    void commit();

    // The hooks (inside a Patch transaction for the detour; the call sites are rewritten directly).
    bool install();

    // The UI images.
    struct ImageInfo
    {
        std::string sheet;          // the TGA it is cut from
        int width = 0, height = 0;  // pixels at the 1024x768 layout
        bool loaded = false;        // its sheet has a texture now
    };
    uint32_t imageCount();          // ids 1 .. count-1
    bool imageInfo(uint32_t id, ImageInfo& out);

    // The UI fonts.
    uint32_t fontCount();
    // The text's size in the font (the widest line, the lines' height); false if the font cannot measure. Call it on
    // the engine thread only (in game): measuring may add glyphs to the font.
    bool textSize(uint16_t font, const std::wstring& text, int& width, int& height);
    // The same for a vector font, at the canvas's present scale (layout units).
    bool textSize(const Text::Style& style, const std::wstring& text, float& width, float& height);

    // An in-game window by its control name (UI_WND_INVENTORY ...), any case.
    struct WindowInfo
    {
        int slot = -1;
        std::string name;
        int x = 0, y = 0, width = 0, height = 0;    // in its own frame (the 1024x768 layout)
        bool visible = false;       // open (flag bit 0)
        bool drawn = false;         // open and drawn this frame (not behind a window the manager draws alone)
        float frameX = 0, frameY = 0;   // where its frame puts its layout (SacredBild), 0, 0: the canvas
        bool movable = false;       // moveWindow can place it
        // Where it drew in the last frame (SacredBild), canvas coordinates: its visual bounds, tabs and ornaments
        // beyond its rect included. false: it drew nothing, or no SacredBild.
        bool hasBounds = false;
        float boundsX = 0, boundsY = 0, boundsW = 0, boundsH = 0;
    };
    bool window(const std::string& name, WindowInfo& out);

    // Moves in-game window slot into a frame of its own at (x, y) against the canvas (layout units); set false:
    // back to its own place. Through SacredBild's UI frames: false without them (no SacredBild, its canvas off) and
    // for the windows the game draws alone.
    bool moveWindow(int slot, float x, float y, bool set);
    bool canMoveWindows();
    std::vector<WindowInfo> windows();

    struct State
    {
        bool available = false;
        const char* mode = "none";  // "game", "menus", "cinematic", "none"
        std::string exclusive;      // the window drawn alone in game, empty if none
        bool escapeMenu = false, messageBox = false, help = false;
        int mouseX = 0, mouseY = 0;
        float screenLeft = 0, screenTop = 0, screenRight = 1024, screenBottom = 768;   // the whole screen, layout units
    };
    State state();
}
