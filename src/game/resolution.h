#pragma once

// Runs the game at an arbitrary resolution instead of its hard-coded 1024x768.
namespace Resolution
{
    // Picks the target size and patches the game; hooks are queued in the caller's Patch transaction.
    void install();

    int width();
    int height();
    bool active();      // false when running at the native 1024x768

    // Re-reads g_unzoomedProjection for the camera conversion fix (call once per frame on the render thread).
    void refresh();

    // Call in dxDriver7::flip before the original: shows the loading screen, drawn into a 1024x768 surface on this
    // thread, scaled to the menus' canvas.
    void beforeFlip(void* dxDriver);

    // Offset that centers a 1024x768 layout on the screen.
    int centerX();
    int centerY();
}
