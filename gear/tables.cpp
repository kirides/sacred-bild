#include "tables.h"
#include "game/gear_addr.h"

#include <windows.h>
#include <cstring>

namespace
{
    // memcpy from the game's memory that fails instead of crashing on a bad pointer (a table that is being torn
    // down, a layout that does not match this build). No C++ objects here: __try needs a function without unwinding.
    bool safeCopy(void* dst, const void* src, size_t size)
    {
        __try
        {
            std::memcpy(dst, src, size);
            return true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            return false;
        }
    }

    struct Span
    {
        uintptr_t first = 0;
        size_t count = 0;
    };

    // A table: its record size and where its records are now.
    struct Table
    {
        std::string_view name;
        size_t stride;
        bool (*locate)(Span& out);
    };

    // Creature types: Pak\Creature.pak is a 0x100-byte header ("CIF", record count at +4) and 0x56-byte records,
    // which the loader (ENG 0x4371C0, thiscall on *g_pTypeManager with the path) copies into a std::vector at
    // +0x563008 (begin) / +0x56300C (end). Record +0 is a type index (the loader numbers +0x1A of the 0x80-byte
    // entries it selects).
    constexpr size_t kCreatureStride = 0x56;
    constexpr uintptr_t kCreatureVector = 0x563008;

    bool locateCreatureTypes(Span& out)
    {
        if (!Gear::Addr::g_pTypeManager)
        {
            return false;
        }
        uintptr_t manager = 0;
        if (!safeCopy(&manager, Gear::Addr::g_pTypeManager.get(), sizeof(manager)) || !manager)
        {
            return false;
        }
        uintptr_t range[2] = {};
        if (!safeCopy(range, reinterpret_cast<const void*>(manager + kCreatureVector), sizeof(range)))
        {
            return false;
        }
        const auto [first, last] = range;
        if (!first || last < first || (last - first) % kCreatureStride != 0 || (last - first) / kCreatureStride > 65536)
        {
            return false;
        }
        out = {first, (last - first) / kCreatureStride};
        return true;
    }

    constexpr Table kTables[] = {
        {"creature_types", kCreatureStride, &locateCreatureTypes},
    };

    const Table* find(std::string_view name)
    {
        for (const Table& t : kTables)
        {
            if (t.name == name)
            {
                return &t;
            }
        }
        return nullptr;
    }

    size_t sizeOf(Tables::Kind kind)
    {
        switch (kind)
        {
        case Tables::Kind::U8:
        case Tables::Kind::I8:
            return 1;
        case Tables::Kind::U16:
        case Tables::Kind::I16:
            return 2;
        default:
            return 4;
        }
    }
}

bool Tables::info(std::string_view name, Info& out)
{
    const Table* t = find(name);
    if (!t)
    {
        return false;
    }
    Span span;
    out = {t->locate(span) ? span.count : 0, t->stride};
    return true;
}

bool Tables::read(std::string_view name, size_t index, size_t offset, Kind kind, double& out)
{
    const Table* t = find(name);
    Span span;
    const size_t size = sizeOf(kind);
    if (!t || offset > t->stride || size > t->stride - offset || !t->locate(span) || index >= span.count)
    {
        return false;
    }
    uint8_t bytes[4] = {};
    if (!safeCopy(bytes, reinterpret_cast<const void*>(span.first + index * t->stride + offset), size))
    {
        return false;
    }
    switch (kind)
    {
    case Kind::U8: out = bytes[0]; break;
    case Kind::I8: out = static_cast<int8_t>(bytes[0]); break;
    case Kind::U16: { uint16_t v; std::memcpy(&v, bytes, 2); out = v; break; }
    case Kind::I16: { int16_t v; std::memcpy(&v, bytes, 2); out = v; break; }
    case Kind::U32: { uint32_t v; std::memcpy(&v, bytes, 4); out = v; break; }
    case Kind::I32: { int32_t v; std::memcpy(&v, bytes, 4); out = v; break; }
    case Kind::F32: { float v; std::memcpy(&v, bytes, 4); out = v; break; }
    }
    return true;
}
