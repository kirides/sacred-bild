#pragma once
#include <cstdint>
#include <map>
#include <string>

// The game's own settings as mods may read them (never write): Settings.cfg in the game folder, lines "KEY : value"
// (AUTOSAVE, DETAILLEVEL, FULLSCREEN ...). The options window writes the file when the player confirms it, and many
// settings are read only at start, so this is what the game was set to, not a live view. Reread when the file's
// write time changes. Engine thread only.
namespace GameCfg
{
    // Whole file, keys as written (upper case).
    const std::map<std::string, std::string>& all();
    // Counts the rereads: a cached copy of all() is current while this has not changed.
    uint64_t generation();
}
