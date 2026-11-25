#pragma once

// [Debug] D3DStats: the game's and Miles' file opens and reads (CreateFileA, ReadFile imported by sacred.exe and
// mss32.dll) timed. The render thread's time in them goes to D3DStats (per second and per hitch); any call slower than
// 2 ms is logged with its file and caller. The remaining ~10 ms hitches had the render thread in the sound system's
// file access (playing a sample reads it from the pak; streams open their file).
namespace FileIoStats
{
    // IAT patches; outside a Patch transaction.
    void install();
}
