#pragma once

// [Render] WarmFileCache: the game reads its pak files (sound samples, textures, ground, models) and Miles its music
// streams synchronously on the render thread while playing. A read that Windows has to fetch from the drive took a
// constant ~8 ms (any size, also with the virus scanner off: the SSD waking from a power-saving state between the
// game's occasional reads), a hitch each time. A background thread reads PAK, World and mp3 once at low I/O priority
// so they sit in Windows' file cache (sound paks first, at most a quarter of the free memory); the game's own reads
// then never reach the drive.
namespace FileCache
{
    void install();
}
