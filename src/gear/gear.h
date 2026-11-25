#pragma once
#include "gear_res.h"

// Sacred Gear: the mod kit for Sacred Gold (LuaJIT). One DLL, SacredGear.dll, that any DLL loader can put into
// sacred.exe: SacredBild loads it when it sits in the game folder, an ASI loader loads it renamed to SacredGear.asi,
// an injector loads it as it is. See docs/Gear.md.
namespace Gear
{
    inline constexpr const char* kVersion = "0.1.0";
}
