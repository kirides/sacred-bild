#pragma once

// [Screenshot] in SacredBild.ini (read by config.cpp).
namespace Config
{
    struct Screenshot
    {
        // Screenshots (Print Screen) of the whole screen as Capture\shotNNNN.png, or .jpg with Format=jpg.
        bool jpeg = false;
    };
    inline Screenshot screenshot;
}
