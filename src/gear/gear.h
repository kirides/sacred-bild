#pragma once
#include "version_info.h"

// Sacred Gear: the mod kit for Sacred Gold (LuaJIT). One DLL, SacredGear.dll, that any DLL loader can put into
// sacred.exe: SacredBild loads it when it sits in the game folder, an ASI loader loads it renamed to SacredGear.asi,
// an injector loads it as it is. See docs/Gear.md.
namespace Gear
{
    inline constexpr const char* kVersion = SG_VER_STR;   // the version file, src/gear/SacredGear_version.txt
}
