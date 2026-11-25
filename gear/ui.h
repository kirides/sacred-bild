#pragma once
#include "text.h"

#include <array>
#include <cstdint>
#include <memory>
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
    // ItemIcon: an item's model drawn by the game's model renderer (gear/itemicon.*); `image` is the item type, `region` the
    // item object's id (0: make a temporary item of the type).
    // Mesh: a triangle mesh (meshCreate) drawn translated by (x, y), both in the mesh's own units; `w` is how many of them
    // make a layout unit (the RmlUi host's pixels per layout unit); `region` the mesh id; `image` its texture (0 none, a
    // game image id as for Image, or a picture id from pictureCreate32); `flip` as for images; the clip cuts the triangles.
    enum class Kind : uint8_t { Rect, Text, Image, Region, ItemIcon, Mesh };
    enum class Fit : uint8_t { Stretch, Contain, Cover };

    struct Command
    {
        Kind kind = Kind::Rect;
        Layer layer = Layer::Hud;
        int8_t window = -1;         // draw in this in-game window's UI frame (slot 0..22), -1: the canvas
        Fit fit = Fit::Stretch;
        uint8_t flip = 0;
        float crop[4] = {0, 0, 1, 1};   // images: the part of the image drawn (left, top, right, bottom as fractions)           // images: 1 mirrored left-right, 2 upside down
        float x = 0, y = 0, w = 0, h = 0;
        uint32_t color = 0xFFFFFFFF;    // ARGB; for images a tint (white: as is)
        // Text
        std::wstring text;
        uint16_t font = 0;
        uint32_t flags = 0;         // the font's: 1 centered, 4 right, 0x20 curved (see docs/RE_NOTES.md, "The UI pass, fonts and images")
        uint8_t valign = 0;         // in the height: 0 top, 1 middle, 2 bottom, 3 y is the baseline (vector fonts)
        bool vector = false;        // a vector font (style), drawn for the screen's pixels, instead of game font `font`
        Text::Style style;
        uint32_t background = 0;    // ARGB, 0: none; a rect behind the measured text
        float padding = 0;
        // Image
        uint32_t image = 0;
        bool point = false;         // images / meshes: sampled with the nearest texel (no bilinear blur): "gear:image/N?point" in an RmlUi document
        // Region
        uint32_t region = 0;        // the kernel's handle, reported with the events
        // Mesh: a 4x4 transform (column-major, m[column * 4 + row]) applied to (x + translation, y + translation, 0, 1) in the mesh's
        // units, then divided by w; null: none (CSS transforms of the RmlUi host).
        std::shared_ptr<const std::array<float, 16>> matrix;
        // Clip (setClip while it was recorded; canvas commands only): drawn only inside x, y, w, h.
        bool clipped = false;
        float clip[4] = {};
    };

    // Lua thread: the clip rectangle for the commands recorded from now on (canvas coordinates; regions are cut to
    // it, a region left empty is dropped); on = false: none. A scroll area sets it for its content.
    void setClip(bool on, float x = 0, float y = 0, float w = 0, float h = 0);

    // The mouse wheel turned over mod UI since the last call (WM_MOUSEWHEEL units, + away from the player): through
    // SacredBild's window procedure, else Gear's own message hook on the game's window. Over a mod's region the game
    // does not get it.
    int takeWheel();
    // D-pad up / down in a mod's scroll area since the last call (SacredBild's controller; lines, + down).
    int takePadScroll();

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
    // A texture of PAK\Texture.pak by its file name (GUI_MOVE_RUNDUM.TGA, any case) as an image id for gear.ui.image, 0 if the
    // game has none. The engine loads it (the icons of abilities and skills are such textures); it keeps its own size.
    uint32_t textureImage(const std::string& name);
    uint32_t imageCount();          // ids 1 .. count-1
    bool imageInfo(uint32_t id, ImageInfo& out);

    // Pictures: images of Gear's own (a savegame's thumbnail), drawn like the UI images (Kind::Image) by an id from
    // kPictureBase on. Lua thread: pictureCreate keeps the pixels (ARGB 4444, rows top to bottom) and returns the id;
    // the texture is made when it is first drawn. pictureRelease drops it (its texture goes with the next frame).
    constexpr uint32_t kPictureBase = 0x40000000;
    uint32_t pictureCreate(int width, int height, std::vector<uint16_t> pixels);
    // The same in 32 bits (ARGB, rows top to bottom, 0xAARRGGBB); `premultiplied`: the color channels are already scaled by
    // alpha (the mesh blends them as they are).
    uint32_t pictureCreate32(int width, int height, std::vector<uint32_t> pixels, bool premultiplied);
    // A picture of DXT1 / DXT3 / DXT5 blocks (`fourcc` as in a DDS file; a power of two in both sides, at least 4), straight alpha, uploaded as it is: no
    // decoding, a quarter or an eighth of the memory. 0 if the size or the data do not fit.
    uint32_t pictureCreateBlocks(int width, int height, uint32_t fourcc, std::vector<uint8_t> blocks);
    void pictureRelease(uint32_t id);
    // How many frames a released picture's texture waits before it is freed (default 2): the frames queued before the release may still draw it.
    // A backend with fences or several swap chain images sets its number of frames in flight here.
    void setFramesInFlight(int frames);

    // Meshes (Kind::Mesh): vertices with positions in the mesh's units, a color (0xAARRGGBB, alpha premultiplied) and texture
    // coordinates (0..1 over the whole texture or game image); indices into them, three per triangle. Any thread; a
    // mesh stays until meshRelease (drawing a released one draws nothing). 0 if the data is invalid.
    struct MeshVertex
    {
        float x, y;
        uint32_t color;
        float u, v;
    };
    uint32_t meshCreate(std::vector<MeshVertex> vertices, std::vector<int> indices);
    void meshRelease(uint32_t id);

    // Screen pixels per layout unit of the canvas the Lua thread draws in (1 without SacredBild).
    float canvasScale();

    // The right and bottom edge of the whole screen in layout units (at least the 1024 x 768 layout's): where a UI
    // document has to reach to cover it. False without SacredBild's canvas.
    bool screenExtent(float& right, float& bottom);

    // The left and top edge of the whole screen in layout units (at most 0: the canvas leaves room left of and above the 1024 x 768 layout on a wide
    // screen). False without SacredBild's canvas.
    bool screenOrigin(float& left, float& top);

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

    // Automation self tests: shows or hides an in-game window (the window's own `show`, vtable +0x24) without the
    // events that normally open it (the chest window opens when the hero uses a chest). False if there is no such
    // window or the game faulted.
    bool setWindowShown(const std::string& name, bool shown);

    // The game's text field (cUI_EditControl) that has the keyboard focus and is really shown (the control and all its
    // parents visible, not read-only), as its rect in the 1024x768 layout; false if none. The game leaves the focus flag on
    // the control of a closed window, so the focus alone says nothing.
    bool focusedTextField(float& x, float& y, float& w, float& h);

    // Types `text` (UTF-16) into the game as characters of the keyboard (WM_CHAR posted to its window: the focused text
    // field takes them as typed, with its own length limits). Control characters are skipped. Returns the number posted.
    int typeText(const std::wstring& text);

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
        bool showText = false;      // the game shows its overhead labels (Alt held; in game)
        int mouseX = 0, mouseY = 0;
        float screenLeft = 0, screenTop = 0, screenRight = 1024, screenBottom = 768;   // the whole screen, layout units
    };
    State state();
    // The engine of the running game (cEngine_updateZoom's this, each frame), for its world view's flags.
    void setEngine(void* engine);
}
