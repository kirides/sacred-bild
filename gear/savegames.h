#pragma once
#include <cstdint>
#include <string>
#include <vector>

// The savegames as a savegame screen shows them: Save\GAME<n>.PAK files (where the file overlay puts the game's Save
// folder), each read for what the game's own savegame window reads (0071E600) and what it leaves out: the hero's name,
// class and level, when it was saved, its picture. Loading and deleting go the game's own ways (the savegame window's).
//
// The file: a 0x100-byte header ("AMS", the format version in byte 3: 0x1B for Sacred Gold; +4 the number of
// chunks; +0x5C the local time it was saved, int32 year, month, day, day of week, hour, minute, second, ms, zero in
// old ones), then a table of 12-byte entries {type, offset, size} from +0x100. Chunks read here: 0x83 the savegame's
// name (64 bytes, Windows-1252), 0x95 the game world's date as text (UTF-16), 0x93 the picture (128 x 96, ARGB 4444, 24576
// bytes), 0xC3 the hero (int32 level, int32 class 1..9, then the name in UTF-16, 64 characters at most).
namespace Savegames
{
    constexpr uint32_t kFormat = 0x1B;      // the game loads only this version (its window refuses the others)

    struct Info
    {
        uint32_t slot = 0;
        std::string file;           // GAME05.PAK
        std::string name;           // the savegame's name (UTF-8), may be empty
        std::string hero;           // UTF-8; empty if the file has no hero chunk
        int classId = 0;            // 1 seraphim .. 9 daemon, 0 unknown
        int level = 0;
        uint32_t version = 0;
        int64_t time = 0;           // when it was saved (unix seconds, from the header's local time), 0 if unknown
        int64_t mtime = 0;          // the file's write time (unix seconds)
        uint64_t size = 0;
        std::string worldDate;      // the game world's date line ("Date[15/ 2/1370] Time[15.39]"), UTF-8
        bool picture = false;       // has a 128 x 96 picture
    };

    // The folder the game reads its savegames from (no trailing backslash).
    std::string folder();

    // Every savegame, by file name.
    std::vector<Info> list();

    // The savegame's picture as a Gear picture (Ui::pictureCreate), made on first use and kept while the file stays
    // the same; 0 if it has none.
    uint32_t picture(uint32_t slot);

    // Deletes the savegame's file (as the savegame window's Delete does; its list is refreshed). False and `error`.
    bool remove(uint32_t slot, const char*& error);

    // Loads the savegame (single player): closes the savegame window if it is open and posts the game's load
    // message, as the window's Load does (0071CC90 command 6). False and `error` if not possible now.
    bool load(uint32_t slot, const char*& error);

    // The first number from 1 up whose file does not exist: the rule of the game's own "first free number" (0x619B80,
    // which a hook of `install` replaces: it gave up at 509). 0 if the folder holds every number up to Save::kMaxSlot.
    uint32_t firstFreeSlot();

    // Slots that firstFreeSlot (and so the game's own picker) leaves alone although their file does not exist yet: the
    // numbers a mod writes to itself (the autosave mod's rotation), so that a manual save never takes one first.
    void setReserved(std::vector<uint32_t> slots);

    // Inside the Patch transaction: detours the game's own load / new game command (cCommand_armaPlay::execute), so
    // mods learn which savegame is loaded or that a new game starts (gear.savegame), and replaces the game's "first
    // free savegame number" by one that looks up to Save::kMaxSlot. False if the load command was not found.
    bool install();

    // What the game was told to do, in order (the oldest first).
    struct Command
    {
        enum class Kind { Load, LoadHero, New } kind = Kind::New;
        uint32_t number = 0;        // Load: the savegame slot; LoadHero: the network hero's number
        std::string scope;          // Load / LoadHero: scope(); empty for New
    };
    bool take(Command& out);

    // A load Gear itself posts to the game window (automation, Savegames::load): the game's command is not run then.
    void noteLoad(uint32_t slot);

    // A name for "this savegame" that is safe in a file name: GAME05 for Save/GAME05.PAK, <folder>-GAME05 when the game
    // reads its savegames from another folder (ReBorn: mods/reborn/save), HERO03 for a network hero file. Mods keep
    // per-savegame data in saves/<scope>/ of their folder.
    std::string scopeOf(uint32_t slot);
    std::string heroScopeOf(uint32_t number);
}
