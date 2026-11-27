#pragma once

// [Render] ReadCache: the game reads its map data in many small pieces while new areas stream in (World\Floor.PAK,
// MIXED.PAK: ~5000 reads of 512 bytes, each after a seek, per sector change), and every read and seek is a system
// call. For the files sacred.exe opens for reading only, SacredBild keeps the file position itself and answers reads
// of up to 16 KB from 64 KB blocks it reads once (at most 32 MB for all files, the least recently used go first);
// larger reads go to Windows from the position SacredBild kept. The hooks are sacred.exe's imports of CreateFileA,
// ReadFile, SetFilePointer and CloseHandle, installed before the game opens anything.
namespace ReadCache
{
    void install();
    // [Debug] D3DStats: reads answered from memory and system reads since the last call, logged.
    void logStats();
}
