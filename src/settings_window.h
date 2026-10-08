#pragma once
#include <windows.h>
#include <string>

// The settings window shown before the game starts: the common SacredBild.ini settings, saved back to the ini.
namespace SettingsWindow
{
    // Unless [Launcher] HideSettingsWindow=1; always while Shift is held as the game starts.
    bool wanted();
    // Shows it modally (not under the loader lock: from the exe's entry point). Changed settings are written to
    // SacredBild.ini and the settings are read again. False = the player chose Exit.
    bool show(HMODULE module, const std::wstring& gameDir);
}
