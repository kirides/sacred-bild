#pragma once
#include <string>

// The game's UI keeps running in its native 1024x768 space: it is drawn into a centered (optionally scaled)
// canvas and the cursor is mapped into that space. World code keeps physical screen coordinates.
// The canvas has an in-game placement ([UI] Scale) and one for the menus ([UI] ScaleMode=InGame: as large as fits;
// Full: the in-game one). The UI manager's state picks it; a UI scope keeps the one it started with.
//
// Frames place a 1024x768 layout elsewhere on the screen: windows anchored to a screen edge (UiAnchor) draw, read
// the cursor and receive mouse events in a frame shifted against the canvas, so the game's own layout of each
// window, hard-coded coordinates included, stays intact. The canvas is the frame at offset 0.
namespace UiCanvas
{
    // Hooks the cursor and the world's mouse reads; call inside a Patch transaction after Resolution::install.
    void install();
    bool enabled();

    // Canvas placement in physical pixels (in game or menus, see above).
    float scale();
    float left();
    float top();
    float right();
    float bottom();

    // Cursor in canvas coordinates (what cMouse holds) and back.
    int toVirtualX(int physical);
    int toVirtualY(int physical);
    int toPhysicalX(int virt);
    int toPhysicalY(int virt);

    // A frame: offset against the canvas in virtual units; confined frames clip their draws to their 1024x768
    // rect (windows the game hid by moving them off its screen stay hidden).
    struct Frame
    {
        float x = 0.0f;
        float y = 0.0f;
        bool confine = true;

        bool operator==(const Frame&) const = default;
    };

    // Frame that puts the 1024x768 layout at x, y (0..1) of the room the screen leaves around it: 0 against the
    // left/top edge, 0.5 centered, 1 against the right/bottom edge.
    Frame placed(float x, float y);

    // The calling thread's frame and where it lies on the screen (physical pixels). `clip` is the frame's rect if it
    // is confined, else the screen.
    Frame frame();
    struct Placement
    {
        float scale, originX, originY;
        float clipLeft, clipTop, clipRight, clipBottom;
    };
    Placement placement();

    // The screen in the current frame's coordinates.
    struct Bounds
    {
        float left, top, right, bottom;
    };
    Bounds screenBounds();

    // [Debug] UiTrace: Scroll Lock logs the next UI frame's draws (UiTrace lines with the frame and the calling
    // sacred.exe code), and popup texts set during the next seconds.
    bool tracing();             // the calling thread draws a traced UI frame
    bool tracingPopups();
    void trace(const std::string& line);

    // Draws and cursor reads of the calling thread go through `frame` while it lives (nests).
    class FrameScope
    {
    public:
        explicit FrameScope(const Frame& frame);
        ~FrameScope();
        FrameScope(const FrameScope&) = delete;
        FrameScope& operator=(const FrameScope&) = delete;

    private:
        Frame m_previous;
    };

    // UI drawing scope (nests, per thread). While active, the device proxy maps draws into the current frame.
    // Canvas: starts in the canvas frame, confined (windows parked off-screen stay hidden). Overlay: mapped the same
    // way but may cover the whole screen (the cursor, which also points at the world beside the canvas).
    enum class Mode { Canvas, Overlay };
    void enter(Mode mode = Mode::Canvas);
    void leave();
    bool active();

    struct Scope
    {
        explicit Scope(Mode mode = Mode::Canvas) { enter(mode); }
        ~Scope() { leave(); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };

    // Leaves UI mode for code reached from the UI that renders in physical space (savegame portrait).
    int suspend();
    void resume(int depth);

    struct Suspend
    {
        Suspend() : depth(suspend()) {}
        ~Suspend() { resume(depth); }
        Suspend(const Suspend&) = delete;
        Suspend& operator=(const Suspend&) = delete;
        int depth;
    };
}
