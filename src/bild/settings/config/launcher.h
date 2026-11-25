#pragma once

// [Launcher] in SacredBild.ini (read by config.cpp).
namespace Config
{
    struct Launcher
    {
        // The settings window before the game starts ([Launcher] HideSettingsWindow=0).
        bool settingsWindow = true;
    };
    inline Launcher launcher;
}
