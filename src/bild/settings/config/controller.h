#pragma once

// [Controller] in SacredBild.ini (read by config.cpp; the bindings are [Controller.Bindings], see input/bindings.h).
namespace Config
{
    struct Controller
    {
        bool enabled = true;
        int deadzone = 24;          // percent of a stick's travel ignored around its center
        int cursorSpeed = 900;      // cursor speed at full deflection, in 1024x768 pixels per second
        int moveRadius = 160;       // how far ahead of the hero the left stick walks to, screen pixels
        int aimRange = 450;         // aim assist: enemies within this many pixels of the hero
        int aimCone = 90;           // ... preferring those within this many degrees around the aim
        bool artClick = true;       // combat art slot buttons: select the slot (6-0) and right-click the target
        bool walk = true;           // the left stick pushed less than halfway walks (Shift) instead of running
        bool prompts = true;        // button icons beside what a button does (Direct3D 9 backend)
    };
    inline Controller controller;
}
