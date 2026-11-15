#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// The engine's data tables as Sacred Gear sees them: arrays of fixed-size records that the game loads once from its
// pak and bin files and then reads by index. Gear reads them in place, in the game's own memory, every time (the
// game may reallocate an array, so no pointer is kept), and checks each access against the table's current bounds.
// Read only: the object types and the creature types (Pak\Creature.pak).
namespace Tables
{
    enum class Kind : uint8_t { U8, I8, U16, I16, U32, I32, F32 };

    struct Info
    {
        size_t count;   // 0 while the game has not loaded the table
        size_t stride;  // record size in bytes
    };

    // False if `name` is not a table Gear knows.
    bool info(std::string_view name, Info& out);

    // One field of record `index` at byte `offset`; false if the table, index or offset is out of range or the
    // game's memory there cannot be read.
    bool read(std::string_view name, size_t index, size_t offset, Kind kind, double& out);

    // A zero-terminated string of at most `maxLength` bytes (and not past the record) at byte `offset`.
    bool readString(std::string_view name, size_t index, size_t offset, size_t maxLength, std::string& out);
}
