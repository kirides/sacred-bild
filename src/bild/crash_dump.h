#pragma once
#include <string>

// Writes a minidump (`<prefix>-YYYYMMDD-HHMMSS.dmp` in `dir`) when the process crashes, then hands the crash on to
// the filter the game set ([Debug] CrashDump).
namespace CrashDump
{
    // Call early in DllMain, before the game's CRT starts and sets its own filter.
    void install(const std::wstring& dir, const wchar_t* prefix);
}
