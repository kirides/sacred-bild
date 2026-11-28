#pragma once
#include <string_view>

// What kind of process the DLLs are in, decided from the main module (the exe), never from a file next to it.
// Sacred Gear is a mod kit that can change the game's state; some exes it must never touch (a third-party build whose
// makers do not want it). So Gear is allowed only in a process that is positively the stock game, and both DLLs ask
// here: SacredBild before it loads SacredGear.dll, SacredGear.dll itself in DllMain (so any DLL or ASI loader that
// puts it into such an exe gets a failed load, not a loaded module).
//
// The checks are independent of each other, so renaming the exe or editing one field does not get around them:
//   - the file name (the third-party exe's own name, or not one of the game's names at all),
//   - the PE header (build timestamp of the builds that are known to be third-party),
//   - the build's own banner string inside the image (" Compiled on <date> at <time>"), which a renamed or
//     re-stamped copy of the same build still carries.
// Anything not the game by name is `Other` (Gear stays off there, as before); only a positive third-party finding
// is `ThirdParty` (Gear refuses to load at all).
namespace HostGuard
{
    enum class Host
    {
        Game,        // sacred.exe / SacredReborn.exe, no third-party sign
        GameServer,  // gameserver.exe
        Other,       // any other exe (config tool, a loader's host, a renamed exe): Gear does nothing there
        ThirdParty,  // a build Gear must never run in
    };

    struct Verdict
    {
        Host host;
        const char* reason;     // why, for the log
    };

    // The main module's verdict; computed once.
    const Verdict& verdict();

    // Whether Sacred Gear may run in this process: only `Game` (and `GameServer`, which only overlays data files).
    bool gearAllowed();

    // Same decision for a given exe path and image (tests, tools).
    Verdict classify(std::wstring_view exePath, const void* imageBase);
}
