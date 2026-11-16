#include "inventory.h"
#include "game/gear_addr.h"
#include "log.h"
#include "world.h"

#include <windows.h>

#include <cstring>

namespace
{
    using namespace Gear;

    // Container layout (docs/Gear.md, "Engine notes"): 0x1180 bytes each; +0x18 + cell * 12 the cells, +0x115C
    // columns, +0x115E rows, +0x1160 the container's number. A cell: dword item id (on an item's top-left cell),
    // byte flags at +4 (bit 0 taken), at +5 / +6 the item's width / height (its top-left cell) or the column / row of
    // its top-left cell (the others).
    constexpr uintptr_t kContainerSize = 0x1180, kCells = 0x18, kCellSize = 12, kColumns = 0x115C, kRows = 0x115E;
    constexpr uint32_t kMaxCells = 0xE5;    // (0x115C - 0x18) / 12

    // An item object: +0x10 type, +0x100 / +0x104 / +0x108 the begin / end / capacity of a vector of 0x82-byte
    // entries (its random properties), +0x118 its 0x82-byte data, +0x120 a number made from that data.
    constexpr uintptr_t kItemType = 0x10, kItemEntries = 0x100, kItemRecord = 0x118, kItemHash = 0x120;
    constexpr size_t kEntrySize = 0x82;
    constexpr uint32_t kMaxEntries = 64;
    // A weapon (cWeapon3D) keeps four more dwords after the common data, written in this order by the engine's save
    // and network code (005CF1A0, 005CF2D0): exported as a 16-byte trailer after the record.
    constexpr uintptr_t kWeaponExtra[4] = {0x19C, 0x1AC, 0x1A0, 0x1BC};
    constexpr size_t kWeaponExtraSize = 16;

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

    template <class T>
    bool readAt(uintptr_t base, uintptr_t offset, T& out)
    {
        return base && safeCopy(&out, reinterpret_cast<const void*>(base + offset), sizeof(T));
    }

    uintptr_t containerAddress(uint32_t index)
    {
        void* base = nullptr;
        if (index >= Inventory::kContainers || !Addr::g_pInventory ||
            !safeCopy(&base, Addr::g_pInventory.get(), sizeof(base)) || !base)
        {
            return 0;
        }
        return reinterpret_cast<uintptr_t>(base) + index * kContainerSize;
    }

    void* objectManager()
    {
        void* m = nullptr;
        return Addr::g_pObjectManager && safeCopy(&m, Addr::g_pObjectManager.get(), sizeof(m)) ? m : nullptr;
    }

    struct Cell
    {
        uint32_t id;
        uint8_t flags;
        uint8_t a;
        uint8_t b;
    };

    bool readCell(uintptr_t c, uint32_t cell, Cell& out)
    {
        uint8_t raw[8];
        if (!safeCopy(raw, reinterpret_cast<const void*>(c + kCells + cell * kCellSize), sizeof(raw)))
        {
            return false;
        }
        std::memcpy(&out.id, raw, 4);
        out.flags = raw[4];
        out.a = raw[5];
        out.b = raw[6];
        return true;
    }

    bool readGeometry(uintptr_t c, uint32_t& columns, uint32_t& rows)
    {
        uint16_t cols = 0, rws = 0;
        if (!readAt(c, kColumns, cols) || !readAt(c, kRows, rws) || !cols || !rws || cols * rws > kMaxCells)
        {
            return false;
        }
        columns = cols;
        rows = rws;
        return true;
    }

    // Guarded calls into the game. No C++ objects in these (__try).
    int callFindSpace(uintptr_t c, uint32_t w, uint32_t h, uint16_t* slot)
    {
        __try
        {
            return Addr::cInventory_findSpace(reinterpret_cast<void*>(c), w, h, slot, 0) ? 1 : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    int callPut(uintptr_t c, uint32_t slot, uint32_t id, uint32_t w, uint32_t h, int notify)
    {
        __try
        {
            Addr::cInventory_put(reinterpret_cast<void*>(c), slot, id, w, h, notify);
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    int callRemove(uintptr_t c, uint32_t id, int notify)
    {
        __try
        {
            return Addr::cInventory_remove(reinterpret_cast<void*>(c), id, notify) & 0xFF ? 1 : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    int callSize(uint32_t type, uint16_t* w, uint16_t* h)
    {
        __try
        {
            Addr::cItemType_size(nullptr, type, w, h);
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    int callDestroy(void* m, uint32_t id)
    {
        __try
        {
            Addr::cObjectManager_destroy(m, id, 1, 0, 0);
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    // Gives item object `o` the saved entries and record (what the engine's network and savegame readers do).
    // 1 done, 0 refused, -1 fault.
    int callApply(uintptr_t o, const uint8_t* entries, uint32_t n, const uint8_t* record)
    {
        __try
        {
            uintptr_t begin = *reinterpret_cast<uintptr_t*>(o + kItemEntries);
            uintptr_t end = *reinterpret_cast<uintptr_t*>(o + kItemEntries + 4);
            if (end < begin || (end - begin) % kEntrySize || (end - begin) / kEntrySize > 4096)
            {
                return 0;
            }
            const uint32_t have = static_cast<uint32_t>((end - begin) / kEntrySize);
            if (n < have)
            {
                *reinterpret_cast<uintptr_t*>(o + kItemEntries + 4) = begin + n * kEntrySize;
            }
            else if (n > have)
            {
                uint8_t fill[kEntrySize] = {};
                Addr::cItem_entriesInsert(reinterpret_cast<void*>(o + kItemEntries), reinterpret_cast<void*>(end),
                                          n - have, fill);
            }
            begin = *reinterpret_cast<uintptr_t*>(o + kItemEntries);
            end = *reinterpret_cast<uintptr_t*>(o + kItemEntries + 4);
            if (n && (!begin || end - begin != n * kEntrySize))
            {
                return 0;
            }
            if (n)
            {
                std::memcpy(reinterpret_cast<void*>(begin), entries, n * kEntrySize);
            }
            std::memcpy(reinterpret_cast<void*>(o + kItemRecord), record, kEntrySize);
            *reinterpret_cast<uint32_t*>(o + kItemHash) =
                Addr::cItem_dataHash(nullptr, reinterpret_cast<void*>(o + kItemRecord));
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    int callSocket(uintptr_t item, uint32_t upgradeId)
    {
        __try
        {
            return Addr::cItem_insertUpgrade(reinterpret_cast<void*>(item), upgradeId, 0) & 0xFF ? 1 : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    // Tells the peers about an item the engine has already announced as created (message 0x16F) but whose data we
    // changed afterwards: the engine's own "set record and announce" (message 0x16C, the whole item). 1 done, -1 fault.
    int callAnnounce(uintptr_t item)
    {
        __try
        {
            Addr::cItem_setRecord(reinterpret_cast<void*>(item), reinterpret_cast<void*>(item + kItemRecord), 1);
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    // In a network game only the local hero's backpack and the chest may be touched: those are the containers the
    // engine's own item calls announce to the peers for this player.
    bool containerAllowed(uint32_t index)
    {
        return !World::networkGame() || index == Inventory::kChest || index == World::hero();
    }

    // Whether item object `o` is a weapon: its vtable's slot 8 is cWeapon3D's.
    bool isWeapon(uintptr_t o)
    {
        uintptr_t vtable = 0, slot8 = 0;
        return Addr::cWeapon3D_netIO && readAt(o, 0, vtable) && readAt(vtable, 8 * 4, slot8) &&
               slot8 == static_cast<uintptr_t>(Addr::cWeapon3D_netIO);
    }

    // The weapon's four extra dwords <-> 16 bytes. 1 done, -1 fault.
    int callWeaponExtra(uintptr_t o, uint8_t* bytes, bool write)
    {
        __try
        {
            for (size_t i = 0; i < 4; ++i)
            {
                auto* field = reinterpret_cast<uint8_t*>(o + kWeaponExtra[i]);
                if (write)
                {
                    std::memcpy(field, bytes + i * 4, 4);
                }
                else
                {
                    std::memcpy(bytes + i * 4, field, 4);
                }
            }
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    // Whether the w x h cells at (x, y) are inside the grid and free (the engine's 005DACB0).
    bool cellsFree(uintptr_t c, uint32_t columns, uint32_t rows, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
    {
        if (!w || !h || x + w > columns || y + h > rows)
        {
            return false;
        }
        for (uint32_t cy = y; cy < y + h; ++cy)
        {
            for (uint32_t cx = x; cx < x + w; ++cx)
            {
                Cell cell{};
                if (!readCell(c, cy * columns + cx, cell) || (cell.flags & 1))
                {
                    return false;
                }
            }
        }
        return true;
    }
}

bool Inventory::available()
{
    return Addr::g_pInventory && Addr::cInventory_findSpace && Addr::cInventory_put && Addr::cInventory_remove &&
           Addr::cItemType_size && containerAddress(0) != 0;
}

bool Inventory::container(uint32_t index, Container& out)
{
    const uintptr_t c = containerAddress(index);
    uint32_t columns = 0, rows = 0;
    if (!c || !readGeometry(c, columns, rows))
    {
        return false;
    }
    out = {index, columns, rows};
    return true;
}

std::vector<Inventory::Entry> Inventory::items(uint32_t index)
{
    std::vector<Entry> list;
    const uintptr_t c = containerAddress(index);
    uint32_t columns = 0, rows = 0;
    if (!c || !readGeometry(c, columns, rows))
    {
        return list;
    }
    for (uint32_t cell = 0; cell < columns * rows; ++cell)
    {
        Cell cl{};
        if (!readCell(c, cell, cl))
        {
            break;
        }
        if (cl.id && (cl.flags & 1))
        {
            list.push_back({cl.id, cell % columns, cell / columns, cl.a, cl.b});
        }
    }
    return list;
}

bool Inventory::itemSize(uint32_t id, uint32_t& width, uint32_t& height)
{
    const uintptr_t o = World::itemObject(id);
    uint32_t type = 0;
    if (!o || !Addr::cItemType_size || !readAt(o, kItemType, type))
    {
        return false;
    }
    uint16_t w = 0, h = 0;
    if (callSize(type, &w, &h) <= 0)
    {
        return false;
    }
    width = w ? w : 1;
    height = h ? h : 1;
    return true;
}

bool Inventory::findSpace(uint32_t index, uint32_t width, uint32_t height, uint32_t& x, uint32_t& y)
{
    const uintptr_t c = containerAddress(index);
    uint32_t columns = 0, rows = 0;
    uint16_t slot = 0;
    if (!c || !readGeometry(c, columns, rows) || !width || !height || width > columns || height > rows ||
        callFindSpace(c, width, height, &slot) <= 0 || slot >= columns * rows)
    {
        return false;
    }
    x = slot % columns;
    y = slot / columns;
    return true;
}

bool Inventory::put(uint32_t index, uint32_t id, uint32_t x, uint32_t y, const char*& error)
{
    const uintptr_t c = containerAddress(index);
    uint32_t columns = 0, rows = 0, w = 0, h = 0;
    if (!World::inventoryChangesAllowed())
    {
        error = "inventory changes are not possible now (no hero, or the game does not allow it)";
        return false;
    }
    if (!containerAllowed(index))
    {
        error = "in a network game only your own backpack and the chest can be changed";
        return false;
    }
    if (!c || !readGeometry(c, columns, rows))
    {
        error = "no such container";
        return false;
    }
    if (!itemSize(id, w, h))
    {
        error = "no such item";
        return false;
    }
    if (!cellsFree(c, columns, rows, x, y, w, h))
    {
        error = "no room there";
        return false;
    }
    const uint32_t slot = y * columns + x;
    if (callPut(c, slot, id, w, h, World::networkGame() ? 1 : 0) < 0)
    {
        LOG("Inventory: putting item {} into container {} faulted", id, index);
        error = "the game faulted while putting the item in";
        return false;
    }
    Cell cell{};
    if (!readCell(c, slot, cell) || cell.id != id)
    {
        error = "the game did not take the item";
        return false;
    }
    return true;
}

bool Inventory::remove(uint32_t index, uint32_t id, const char*& error)
{
    const uintptr_t c = containerAddress(index);
    if (!World::inventoryChangesAllowed())
    {
        error = "inventory changes are not possible now (no hero, or the game does not allow it)";
        return false;
    }
    if (!containerAllowed(index))
    {
        error = "in a network game only your own backpack and the chest can be changed";
        return false;
    }
    if (!c)
    {
        error = "no such container";
        return false;
    }
    const int r = callRemove(c, id, World::networkGame() ? 1 : 0);
    if (r < 0)
    {
        LOG("Inventory: taking item {} out of container {} faulted", id, index);
        error = "the game faulted while taking the item out";
        return false;
    }
    if (!r)
    {
        error = "the item is not in that container";
        return false;
    }
    return true;
}

bool Inventory::destroy(uint32_t id, const char*& error)
{
    void* m = objectManager();
    if (!World::inventoryChangesAllowed() || !m || !Addr::cObjectManager_destroy)
    {
        error = "world changes are not possible now";
        return false;
    }
    if (!World::itemObject(id))
    {
        error = "no such item";
        return false;
    }
    for (uint32_t i = 0; i < kContainers; ++i)
    {
        for (const Entry& e : items(i))
        {
            if (e.id == id)
            {
                error = "the item is still in a container";
                return false;
            }
        }
    }
    if (callDestroy(m, id) < 0)
    {
        LOG("Inventory: destroying item {} faulted", id);
        error = "the game faulted while destroying the item";
        return false;
    }
    return true;
}

bool Inventory::socket(uint32_t itemId, uint32_t upgradeId, const char*& error)
{
    const uintptr_t item = World::itemObject(itemId);
    if (!World::inventoryChangesAllowed() || !Addr::cItem_insertUpgrade)
    {
        error = "inventory changes are not possible now (no single player game)";
        return false;
    }
    if (!item || !World::itemObject(upgradeId) || itemId == upgradeId)
    {
        error = "no such item";
        return false;
    }
    const int r = callSocket(item, upgradeId);
    if (r < 0)
    {
        LOG("Inventory: socketing {} into {} faulted", upgradeId, itemId);
        error = "the game faulted while socketing";
        return false;
    }
    if (!r)
    {
        error = "the item has no free socket, or the upgrade does not fit";
        return false;
    }
    return true;
}

bool Inventory::exportItem(uint32_t id, std::string& bytes, const char*& error)
{
    const uintptr_t o = World::itemObject(id);
    uint32_t type = 0;
    uintptr_t begin = 0, end = 0;
    if (!o || !readAt(o, kItemType, type) || !readAt(o, kItemEntries, begin) || !readAt(o, kItemEntries + 4, end))
    {
        error = "no such item";
        return false;
    }
    if (end < begin || (end - begin) % kEntrySize || (end - begin) / kEntrySize > kMaxEntries)
    {
        error = "the item's data does not look as expected";
        return false;
    }
    const uint16_t n = static_cast<uint16_t>((end - begin) / kEntrySize);
    const bool weapon = isWeapon(o);
    std::string out(4 + 2 + n * kEntrySize + kEntrySize + (weapon ? kWeaponExtraSize : 0), '\0');
    std::memcpy(&out[0], &type, 4);
    std::memcpy(&out[4], &n, 2);
    if ((n && !safeCopy(&out[6], reinterpret_cast<const void*>(begin), n * kEntrySize)) ||
        !safeCopy(&out[6 + n * kEntrySize], reinterpret_cast<const void*>(o + kItemRecord), kEntrySize))
    {
        error = "cannot read the item";
        return false;
    }
    if (weapon && callWeaponExtra(o, reinterpret_cast<uint8_t*>(&out[6 + n * kEntrySize + kEntrySize]), false) < 0)
    {
        error = "cannot read the weapon";
        return false;
    }
    bytes = std::move(out);
    return true;
}

uint32_t Inventory::importItem(const std::string& bytes, const char*& error, bool temporary)
{
    const bool net = World::networkGame();
    if (net && temporary)
    {
        error = "temporary items are not made in a network game";
        return 0;
    }
    if (!World::inventoryChangesAllowed())
    {
        error = "inventory changes are not possible now (no hero, or the game does not allow it)";
        return 0;
    }
    if (!Addr::cItem_entriesInsert || !Addr::cItem_dataHash)
    {
        error = "the item functions were not found";
        return 0;
    }
    uint32_t type = 0;
    uint16_t n = 0;
    if (bytes.size() < 6 + kEntrySize)
    {
        error = "the item data is too short";
        return 0;
    }
    std::memcpy(&type, bytes.data(), 4);
    std::memcpy(&n, bytes.data() + 4, 2);
    const size_t base = 6 + n * kEntrySize + kEntrySize;
    if (n > kMaxEntries || (bytes.size() != base && bytes.size() != base + kWeaponExtraSize))
    {
        error = "the item data has the wrong size";
        return 0;
    }
    World::Object hero;
    const uint32_t heroId = World::hero();
    if (!heroId || !World::object(heroId, hero))
    {
        error = "there is no hero";
        return 0;
    }
    // In a network game the engine announces the new item (message 0x16F: id, type, position, default data) when it
    // is made with broadcast; the data we give it follows as the engine's own item update (0x16C).
    const uint32_t id = World::createObject(type, hero.position, net, error);
    if (!id)
    {
        return 0;
    }
    const uintptr_t o = World::itemObject(id);
    const auto* data = reinterpret_cast<const uint8_t*>(bytes.data());
    int r = o ? callApply(o, data + 6, n, data + 6 + n * kEntrySize) : 0;
    if (r > 0 && bytes.size() == base + kWeaponExtraSize)
    {
        // Saved from a weapon: its four extra dwords. Blobs of weapons saved before they were kept have none and
        // leave the new weapon's own values.
        uint8_t extra[kWeaponExtraSize];
        std::memcpy(extra, data + base, sizeof(extra));
        r = isWeapon(o) ? callWeaponExtra(o, extra, true) : 0;
    }
    if (r > 0 && net)
    {
        r = callAnnounce(o);
    }
    if (r <= 0)
    {
        LOG("Inventory: giving the new item {} (type {}) its data {}", id, type, r < 0 ? "faulted" : "was refused");
        const char* ignored = nullptr;
        if (o)
        {
            destroy(id, ignored);
        }
        error = o ? "the item data was refused" : "the type is no item";
        return 0;
    }
    return id;
}
