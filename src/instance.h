#pragma once
#include <string>

// Which run this process is, for parallel automation runs (docs/Gear.md, "Parallel runs"). Shared by SacredBild and
// Sacred Gear so both name their files the same way. Only an automation run (`-gear-auto` on the command line or
// SACREDGEAR_AUTO=1) has an instance; a normal launch has none and keeps every default.
namespace Instance
{
    // Whether this is an automation run.
    bool automation();

    // The run's name: `-gear-instance=NAME` (SACREDGEAR_INSTANCE), else "pid<PID>". Letters, digits and _ - . only
    // (anything else became '_'), at most 48 characters. Empty when this is not an automation run.
    const std::string& name();

    // <game>\GearAutomation\<name>, created; empty when this is not an automation run.
    std::wstring directory(const std::wstring& gameDir);

    // Where a log file goes: <game>\<file> normally, the run's directory in an automation run (so parallel runs do
    // not share it).
    std::wstring logPath(const std::wstring& gameDir, const wchar_t* file);
}
