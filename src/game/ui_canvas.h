#pragma once

// The game's UI keeps running in its native 1024x768 space: it is drawn into a centered (optionally scaled)
// canvas and the cursor is mapped into that space. World code keeps physical screen coordinates.
namespace UiCanvas
{
    // Hooks the cursor and the world's mouse reads; call inside a Patch transaction after Resolution::install.
    void install();
    bool enabled();

    // Canvas placement in physical pixels.
    float scale();
    float left();
    float top();
    float right();
    float bottom();

    int toVirtualX(int physical);
    int toVirtualY(int physical);
    int toPhysicalX(int virt);
    int toPhysicalY(int virt);

    // UI drawing scope (nests, per thread). While active, the device proxy maps draws into the canvas.
    // Canvas: confined to the canvas (windows parked off-screen stay hidden). Overlay: mapped the same way but
    // may cover the whole screen (the cursor, which also points at the world beside the canvas).
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
