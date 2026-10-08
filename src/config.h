#pragma once
#include <string>

// SacredBild.ini next to the game exe. The settings themselves are in config/, a header per ini section, so a new
// setting rebuilds only the code that reads its section.
namespace ConfigFile
{
    void load(const std::wstring& gameDir);

    // SacredBild.ini itself, for the settings changed in game.
    const std::wstring& path();
}
