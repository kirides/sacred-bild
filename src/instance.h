#pragma once
#include <string>

// Which run this process is, for parallel automation runs (docs/Gear.md, "Parallel runs"). Shared by SacredBild and
// Sacred Gear so both name their files the same way. Only an automation run (`-gear-auto` on the command line or
// SACREDGEAR_AUTO=1) has an instance; a normal launch has none and keeps every default.
namespace Instance
{
    // Whether this is an automation run.
    bool automation();

    // Whether this process is an instance: an automation run, or a normal launch with `-gear-instance=NAME`
    // (SACREDGEAR_INSTANCE) to run next to another game (a second client for LAN tests). A manual instance gets its
    // own single-instance mutex, window title suffix and log/run folder, and nothing else changes (no input isolation).
    bool active();

    // The run's name: `-gear-instance=NAME` (SACREDGEAR_INSTANCE), else "pid<PID>". Letters, digits and _ - . only
    // (anything else became '_'), at most 48 characters. Empty when this is not an instance.
    const std::string& name();

    // <game>\GearAutomation\<name>, created; empty when this is not an instance.
    std::wstring directory(const std::wstring& gameDir);

    // Where a log file goes: <game>\<file> normally, the run's directory in an automation run (so parallel runs do
    // not share it).
    std::wstring logPath(const std::wstring& gameDir, const wchar_t* file);
}
