#pragma once
#include <cstdint>
#include <string>

// Saving the game as mods see it: the savegame window's own save request, sent to the engine with a slot number
// (Save\GAMEnn.PAK; 0 is the quicksave, the game's list sorts by file name), the state it needs, and the result of
// the write. The engine writes the file on its render thread a moment after the request; this module watches the
// engine's flags for that and compares the file before and after. Single player only. Engine thread only (the Lua
// host's thread).
namespace Save
{
    constexpr uint32_t kMaxSlot = 0x1FD;     // the engine stores the slot in 9 bits (0x1FE and up are internal)

    // Once per in-game frame, before the mods run: remembers the engine (the hook's `this`) and finishes a save the
    // engine completed.
    void frame(void* engine);

    // Whether the addresses were found and a frame has run (the engine is known).
    bool available();

    // Whether a save or a load is queued or being written.
    bool pending();

    // nullptr if a save may start now, else why not (a short sentence): not single player, no hero or a dead one,
    // not in the game, an exclusive window / escape menu / message box / cinematic, a save or load pending.
    const char* blocked();

    // Sends the save request for `slot` (0..kMaxSlot) with the savegame's display name (at most 60 characters of
    // UTF-8, kept as Windows-1252 like the game's own names; other characters become ?). `thumbnail`: let the engine capture the savegame's picture first (the savegame window captures it
    // before it opens). False with `error` set if the request is not possible or the engine refused it.
    // `fromWindow`: as the savegame window's own Save button (a mod's replacement of it saves): only while that window
    // is open in game; the picture is the one the game took when the window opened, the window closes once the
    // engine took the request.
    bool request(uint32_t slot, const std::string& name, bool thumbnail, const char*& error, bool fromWindow = false);

    // The file Save\GAME<slot>.PAK.
    struct FileInfo
    {
        bool exists = false;
        uint64_t size = 0;
        int64_t mtime = 0;      // unix seconds
        uint64_t stamp = 0;     // the write time in 100 ns (FILETIME), to tell two writes apart
    };
    FileInfo fileInfo(uint32_t slot);

    // The finished saves, oldest first: false if there is none (left).
    struct Result
    {
        uint32_t slot = 0;
        bool ok = false;
        std::string message;    // why not, if !ok
        FileInfo info;          // the file after the write
        // A save the game made on its own, or the player's (quicksave, savegame window, the game's Auto-Save at a
        // script's command): not one a mod asked for. Only `slot` and `ok` are set.
        bool external = false;
    };
    bool takeResult(Result& out);
}
