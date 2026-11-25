#pragma once

// [UI] and [UI.Layout] in SacredBild.ini (read by config.cpp).
namespace Config
{
    struct Ui
    {
        // UI canvas: the 1024x768 UI drawn centered. 0 = scale to fit the screen height.
        float scale = 0.0f;
        // ScaleMode=Full: `scale` applies to the menus (start menu, options, ...) too. InGame: in game only, the menus
        // always fit the screen height.
        bool scaleMenus = false;
        // In game, the HUD windows (taskbar, minimap, inventory, ...) are placed on the screen by [UI.Layout] instead
        // of staying in the centered canvas.
        bool anchor = true;
        // [UI.Layout]: where each in-game window's 1024x768 layout goes, as X,Y in 0..4096 of the room the screen
        // leaves around it: 0 = against the left/top edge, 2048 = centered, 4096 = against the right/bottom edge.
        // Windows at the same position keep their 1024x768 arrangement.
        struct Position
        {
            int x, y;
        };
        Position taskbar{2048, 4096};
        Position chat{2048, 4096};
        Position inventory{0, 4096};
        Position equipment{4096, 0};
        Position stats{4096, 0};
        Position minimap{4096, 0};
        Position portraits{0, 0};
        Position shops{0, 0};       // blacksmith, merchant, combat art master, chest, cube, trade
        bool linearFilter = true;   // bilinear filtering for the scaled UI instead of the game's point sampling
    };
    inline Ui ui;
}
