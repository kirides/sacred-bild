#pragma once
#include <string>

// The file overlay: a mod's own data files served where the game opens the original ones, and a mod's own save
// folder. Declared in `mods\<id>\overlay.txt`, one rule per line (`#` starts a comment):
//
//     SRB\balance.bin = data\SRB\balance.bin     the game's open of this path (relative to the game folder) gets that file
//                                                of the mod instead, if the mod has it
//     saves = save                               the game's `Save\` folder is the mod's `save\` folder (created); the
//                                                vanilla saves are neither read nor written
//
// The rules are read when Sacred Gear starts (the game opens its data files before any Lua runs), for the mods the run
// uses (all, or the ones in `-gear-mods=a,b`). The game's file functions (CreateFileA, GetFileAttributesA/ExA,
// FindFirstFileA, CopyFileA, DeleteFileA, CreateDirectoryA, SetFileAttributesA) are hooked through the exe's imports.
namespace Overlay
{
    // Reads the rules and installs the hooks; false (nothing hooked) if no mod has rules.
    bool install(const std::wstring& gameDir);

    // Where the game's open of `path` ends up (the same text if no rule applies). For logs and gear.files.resolve.
    std::string resolve(const std::string& path);

    // How many times the game opened `path` (relative to the game folder) and got a mod's file; 0 if never.
    unsigned served(const std::string& path);

    // The number of file rules and whether the saves are redirected, for the mod `id`.
    bool info(const std::string& id, size_t& files, std::string& saves);
}
