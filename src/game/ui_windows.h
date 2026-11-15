#pragma once
#include "game/ui_canvas.h"
#include "game/ui_state.h"

#include <string>

// The UI manager's windows as SacredBild sees them while they run: each window class's virtual functions
// (receiveEvent, render, isInside, show and, where the class has it, render2) are wrapped once, so that
//
// - current() says which of the manager's objects is drawing or handling an event on the calling thread (the
//   device proxy, UiTrace and the anchoring ask it instead of guessing from what is drawn), and
// - one client (UiAnchor) can run a window's calls in a frame of its own.
//
// The objects are the manager's windows, menu screens, message box, popups and message lines; their vtables are
// patched at runtime from the objects (no addresses needed). Controls inside a window are not wrapped: their calls
// belong to the window that makes them.
namespace UiWindows
{
    enum class Kind : uint8_t
    {
        None,       // the manager's own drawing (letterbox bars, party arrows), or nothing of the UI
        Window,     // cUI_Manager::windows[index]
        Menu,       // cUI_Manager::menus[index]
        MessageBox, // cUI_Manager::dialog
        Popup,      // one of cUI_Manager::popups
        TextLines,  // the message lines over the world
    };

    struct Owner
    {
        Kind kind = Kind::None;
        int index = -1;                         // the Window or menu index
        Sacred::cUI_Control2* object = nullptr;

        bool is(UiState::Window w) const { return kind == Kind::Window && index == static_cast<int>(w); }
        UiState::Window window() const { return kind == Kind::Window ? static_cast<UiState::Window>(index) : UiState::Window::count; }
    };

    // Which of the manager's objects `object` is (Kind::None if none).
    Owner identify(const Sacred::cUI_Control2* object);
    // The innermost wrapped call on the calling thread (Kind::None outside one).
    Owner current();
    // "window inventory", "popup", ... for logs.
    std::string describe(const Owner& owner);

    // What a client may do around the wrapped calls (unset: the original runs as it is).
    struct Handlers
    {
        // The frame the object's calls run in; false: the calling thread's frame. render, render2 and show run in it.
        bool (*frame)(const Owner& owner, UiCanvas::Frame& out) = nullptr;
        // receiveEvent and isInside, for an object with a frame that differs from the calling thread's: what to
        // hand the original (`original` is the function the vtable held).
        uint32_t (*receiveEvent)(const Owner& owner, const UiCanvas::Frame& frame, Sacred::cEvent* event,
            uint32_t(__fastcall* original)(Sacred::cUI_Control2*, void*, Sacred::cEvent*)) = nullptr;
        uint32_t (*isInside)(const Owner& owner, const UiCanvas::Frame& frame, int x, int y,
            uint32_t(__fastcall* original)(Sacred::cUI_Control2*, void*, int, int)) = nullptr;
        // After cUI_Manager::createGameWindows made the in-game windows (they are wrapped by then).
        void (*windowsCreated)(Sacred::cUI_Manager* manager) = nullptr;
    };
    void setHandlers(const Handlers& handlers);

    // Wraps the vtables of the manager's objects that exist now (cheap when nothing is new): from cUI_Manager::render
    // (FrameHooks) and after createGameWindows.
    void sync();

    // Where the manager's windows drew: the device proxy reports each UI draw (the vertices' rect in the calling
    // thread's frame) and the window drawing it gets the union, in its own layout (without its frame, so the rect
    // does not lag behind a move), clipped to 1024x768; unconfined draws (the held item) do not count. publishDrawn
    // (after cUI_Manager::render) makes the frame's rects the ones export SacredBild_UiGetWindowBounds reports: a
    // window's visual bounds, its tabs and ornaments beyond its rect included.
    void noteDraw(float minX, float minY, float maxX, float maxY);
    void publishDrawn();

    // Call inside a Patch transaction after UiCanvas::install (does nothing without the canvas).
    void install();
}
