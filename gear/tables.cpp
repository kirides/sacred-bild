#include "tables.h"
#include "classes.h"
#include "game/gear_addr.h"

#include <windows.h>
#include <algorithm>
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

    // memcpy into the game's memory, failing instead of crashing.
    bool safeWrite(void* dst, const void* src, size_t size)
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

    // The wide string the game's text table holds for `id`; nullptr if there is none. No C++ objects (__try).
    const wchar_t* gameText(uint32_t id)
    {
        if (!Gear::Addr::textResources_instance || !Gear::Addr::textResources_get)
        {
            return nullptr;
        }
        __try
        {
            void* table = Gear::Addr::textResources_instance();
            if (!table)
            {
                return nullptr;
            }
            const wchar_t* const* entry = Gear::Addr::textResources_get(table, id);
            return entry ? *entry : nullptr;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            return nullptr;
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
        bool writable;
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

    // Object types: the type manager's first 0x7E60 records of 0x80 bytes, one per object type (creatures, items,
    // effects, buildings), indexed by the type id every object keeps at +0x10 (00425EA0 returns manager + id *
    // 0x80; record 0 is unused). +0x2E is the kind (item kinds 8, 0x14, 0x1A, 0x1C; creature kinds below 4,
    // see 005FB530), +0x37 the internal name (0043CED0 shows it, "Bad Item" if empty).
    constexpr size_t kObjectTypeStride = 0x80;
    constexpr size_t kObjectTypeCount = 0x7E60;

    bool locateObjectTypes(Span& out)
    {
        uintptr_t manager = 0;
        if (!Gear::Addr::g_pTypeManager ||
            !safeCopy(&manager, Gear::Addr::g_pTypeManager.get(), sizeof(manager)) || !manager)
        {
            return false;
        }
        out = {manager, kObjectTypeCount};
        return true;
    }

    // Ability tables: static arrays; the first record and the end come from the lookup functions' own code.
    constexpr size_t kAbility1Stride = 0x78;
    constexpr size_t kAbility2Stride = 0x94;

    bool locateAbilities(Span& out, const uint8_t* first, const uint8_t* end, size_t stride)
    {
        const auto a = reinterpret_cast<uintptr_t>(first), b = reinterpret_cast<uintptr_t>(end);
        if (!a || b <= a || (b - a) % stride != 0 || (b - a) / stride > 4096)
        {
            return false;
        }
        out = {a, (b - a) / stride};
        return true;
    }

    bool locateAbility1(Span& out)
    {
        return Gear::Addr::abilityTable1First && Gear::Addr::abilityTable1End &&
               locateAbilities(out, Gear::Addr::abilityTable1First.get(), Gear::Addr::abilityTable1End.get(), kAbility1Stride);
    }

    bool locateAbility2(Span& out)
    {
        return Gear::Addr::abilityTable2First && Gear::Addr::abilityTable2End &&
               locateAbilities(out, Gear::Addr::abilityTable2First.get(), Gear::Addr::abilityTable2End.get(), kAbility2Stride);
    }

    // The alias records of custom classes (classes_alias.cpp): copies of ability records, in two static pools.
    bool locateAlias1(Span& out)
    {
        uintptr_t first = 0;
        size_t count = 0;
        Classes::aliasPool(1, first, count);
        out = {first, count};
        return true;
    }

    bool locateAlias2(Span& out)
    {
        uintptr_t first = 0;
        size_t count = 0;
        Classes::aliasPool(2, first, count);
        out = {first, count};
        return true;
    }

    // Region kill table: 32 static records of 0x40 bytes (record 0 unused). +0x30 the kills a region needs, +0x34 the boss
    // flag, +0x38 the kills left (the boss comes when it reaches 0), +0x3C the region id.
    constexpr size_t kRegionStride = 0x40;
    constexpr size_t kRegionCount = 32;

    bool locateRegions(Span& out)
    {
        if (!Gear::Addr::regionTable)
        {
            return false;
        }
        out = {reinterpret_cast<uintptr_t>(Gear::Addr::regionTable.get()), kRegionCount};
        return true;
    }

    constexpr Table kTables[] = {
        {"creature_types", kCreatureStride, &locateCreatureTypes, false},
        {"object_types", kObjectTypeStride, &locateObjectTypes, false},
        {"ability_t1", kAbility1Stride, &locateAbility1, true},
        {"ability_t2", kAbility2Stride, &locateAbility2, true},
        {"ability_a1", kAbility1Stride, &locateAlias1, true},
        {"ability_a2", kAbility2Stride, &locateAlias2, true},
        {"regions", kRegionStride, &locateRegions, true},
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

bool Tables::readString(std::string_view name, size_t index, size_t offset, size_t maxLength, std::string& out)
{
    const Table* t = find(name);
    Span span;
    if (!t || offset >= t->stride || !t->locate(span) || index >= span.count)
    {
        return false;
    }
    char bytes[256] = {};
    const size_t size = std::min({maxLength, t->stride - offset, sizeof(bytes)});
    if (!safeCopy(bytes, reinterpret_cast<const void*>(span.first + index * t->stride + offset), size))
    {
        return false;
    }
    out.assign(bytes, strnlen(bytes, size));
    return true;
}

bool Tables::write(std::string_view name, size_t index, size_t offset, Kind kind, double value)
{
    const Table* t = find(name);
    Span span;
    const size_t size = sizeOf(kind);
    if (!t || !t->writable || offset > t->stride || size > t->stride - offset || !t->locate(span) || index >= span.count)
    {
        return false;
    }
    uint8_t bytes[4] = {};
    switch (kind)
    {
    case Kind::F32:
    {
        const float v = static_cast<float>(value);
        std::memcpy(bytes, &v, 4);
        break;
    }
    default:
    {
        // an integer that fits the kind
        const double lo = kind == Kind::U8 || kind == Kind::U16 || kind == Kind::U32 ? 0.0
            : kind == Kind::I8 ? -128.0 : kind == Kind::I16 ? -32768.0 : -2147483648.0;
        const double hi = kind == Kind::U8 ? 255.0 : kind == Kind::I8 ? 127.0 : kind == Kind::U16 ? 65535.0
            : kind == Kind::I16 ? 32767.0 : kind == Kind::I32 ? 2147483647.0 : 4294967295.0;
        if (!(value >= lo && value <= hi) || value != static_cast<double>(static_cast<int64_t>(value)))
        {
            return false;
        }
        const int64_t v = static_cast<int64_t>(value);
        std::memcpy(bytes, &v, size);     // little endian: the low bytes
        break;
    }
    }
    return safeWrite(reinterpret_cast<void*>(span.first + index * t->stride + offset), bytes, size);
}

bool Tables::text(uint32_t id, std::string& out)
{
    const wchar_t* wide = gameText(id);
    if (!wide || !*wide)
    {
        return false;
    }
    const int length = static_cast<int>(wcsnlen(wide, 4096));
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, length, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0)
    {
        return false;
    }
    out.resize(static_cast<size_t>(bytes));
    WideCharToMultiByte(CP_UTF8, 0, wide, length, out.data(), bytes, nullptr, nullptr);
    return true;
}

bool Tables::regionOfSector(uint32_t sector, size_t& index, uint8_t& code)
{
    uintptr_t manager = 0, world = 0, range[2] = {}, object = 0, record = 0;
    if (sector == 0 || !Gear::Addr::g_pObjectManager ||
        !safeCopy(&manager, Gear::Addr::g_pObjectManager.get(), sizeof(manager)) || !manager ||
        !safeCopy(&world, reinterpret_cast<const void*>(manager), sizeof(world)) || !world ||
        !safeCopy(range, reinterpret_cast<const void*>(world + 0x240), sizeof(range)) || !range[0] || range[1] < range[0] ||
        sector >= (range[1] - range[0]) / sizeof(uintptr_t) ||
        !safeCopy(&object, reinterpret_cast<const void*>(range[0] + sector * sizeof(uintptr_t)), sizeof(object)) || !object ||
        !safeCopy(&record, reinterpret_cast<const void*>(object + 0x17C), sizeof(record)) || !record ||
        !safeCopy(&code, reinterpret_cast<const void*>(record + 0xD7), 1))
    {
        return false;
    }
    Span span;
    if (!locateRegions(span))
    {
        return false;
    }
    for (size_t i = 1; i < span.count; ++i)
    {
        uint8_t chars[32] = {};
        if (!safeCopy(chars, reinterpret_cast<const void*>(span.first + i * kRegionStride), sizeof(chars)))
        {
            return false;
        }
        if (std::find(std::begin(chars), std::end(chars), code) != std::end(chars))
        {
            index = i;
            return true;
        }
    }
    return false;
}
