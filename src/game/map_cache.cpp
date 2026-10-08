#include "game/map_cache.h"
#include "game/d3d_stats.h"
#include "game/sacred_addr.h"
#include "config/render.h"
#include "log.h"
#include "patch.h"
#include "spin_lock.h"

#include <gtl/phmap.hpp>

#include <cstdint>
#include <mutex>
#include <shared_mutex>

namespace
{
    using namespace Sacred;

    using CacheFn = uint8_t*(__fastcall*)(void* owner, void* edx, uint32_t id);
    using FindFn = uint8_t*(__fastcall*)(void* map, void* edx, const uint32_t* id);

    FindFn g_find = nullptr;
    constexpr uintptr_t kNodeRecord = 0x14;

    // The ids one owner's map holds, valid while the map still has the head and size it had when the index last
    // saw it: the cache functions are the only code that inserts or evicts, and clearing resets the size.
    struct Index
    {
        void* owner = nullptr;
        uint8_t* head = nullptr;
        uint32_t size = 0;
        gtl::flat_hash_map<uint32_t, uint8_t*> nodes;
    };

    struct Cache
    {
        CacheFn original;
        uintptr_t mapOffset;    // the std::map in the owner: +0 head node, +4 size
        uintptr_t stampOffset;  // last use, in the node
        SharedSpinLock mutex;       // the game calls some of these from its logic thread as well
        Index indexes[4];
        uint32_t next = 0;
    };

    Cache g_layers{nullptr, 0xDE5C, 0x24};
    Cache g_records{nullptr, 0xDE50, 0x54};

    uint8_t*& mapHead(uint8_t* map) { return *reinterpret_cast<uint8_t**>(map); }
    uint32_t mapSize(uint8_t* map) { return *reinterpret_cast<uint32_t*>(map + 4); }

    void stamp(const Cache& c, uint8_t* node)
    {
        *reinterpret_cast<uint32_t*>(node + c.stampOffset) = *reinterpret_cast<const uint32_t*>(Addr::g_recordStamp);
    }

    Index* findIndex(Cache& c, void* owner)
    {
        for (Index& ix : c.indexes)
        {
            if (ix.owner == owner)
            {
                return &ix;
            }
        }
        return nullptr;
    }

    uint8_t* lookup(Cache& c, void* owner, void* edx, uint32_t id)
    {
        if (id == 0)
        {
            return c.original(owner, edx, id);
        }
        uint8_t* map = static_cast<uint8_t*>(owner) + c.mapOffset;
        {
            std::shared_lock lock(c.mutex);
            const Index* ix = findIndex(c, owner);
            if (ix && ix->head == mapHead(map) && ix->size == mapSize(map))
            {
                const auto it = ix->nodes.find(id);
                if (it != ix->nodes.end())
                {
                    stamp(c, it->second);
                    return it->second + kNodeRecord;
                }
            }
        }

        std::unique_lock lock(c.mutex);
        Index* ix = findIndex(c, owner);
        if (!ix)
        {
            ix = &c.indexes[c.next++ % std::size(c.indexes)];
            ix->owner = owner;
            ix->nodes.clear();
            ix->head = nullptr;
        }
        if (ix->head != mapHead(map) || ix->size != mapSize(map))
        {
            ix->nodes.clear();
            ix->head = mapHead(map);
            ix->size = mapSize(map);
        }
        uint8_t* node = g_find(map, nullptr, &id);
        if (node != mapHead(map))
        {
            stamp(c, node);     // what the game does on a hit
        }
        else
        {
            D3DStats::count(D3DStats::CRecordRead);
            uint8_t* record = c.original(owner, edx, id);
            if (mapSize(map) != ix->size + 1)
            {
                ix->nodes.clear();  // a node was evicted: it may be one the index knows
            }
            ix->head = mapHead(map);
            ix->size = mapSize(map);
            // The new node, unless the eviction took it right away (then the game returns a dangling record).
            node = record ? g_find(map, nullptr, &id) : nullptr;
            if (!node || node == mapHead(map) || node + kNodeRecord != record)
            {
                return record;
            }
        }
        ix->nodes.emplace(id, node);
        return node + kNodeRecord;
    }

    uint8_t* __fastcall hookLayers(void* owner, void* edx, uint32_t id) { return lookup(g_layers, owner, edx, id); }
    uint8_t* __fastcall hookRecords(void* owner, void* edx, uint32_t id) { return lookup(g_records, owner, edx, id); }
}

void MapCache::install()
{
    if (!Config::render.recordIndex)
    {
        return;
    }
    g_find = reinterpret_cast<FindFn>(Addr::recordMapFind);
    Patch::hook(g_layers.original, Addr::layerRecordCache, &hookLayers, "layerRecordCache");
    Patch::hook(g_records.original, Addr::recordCache, &hookRecords, "recordCache");
    LOG("Record caches: hash index in front of the tree lookups");
}
