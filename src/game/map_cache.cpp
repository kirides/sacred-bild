#include "game/map_cache.h"
#include "game/d3d_stats.h"
#include "config/render.h"
#include "log.h"
#include "patch.h"
#include "sacred/world.h"
#include "spin_lock.h"

#include <gtl/phmap.hpp>

#include <cstdint>
#include <mutex>
#include <shared_mutex>

namespace
{
    using namespace Sacred;

    using CacheFn = decltype(Addr::recordCache)::Ptr;

    constexpr uintptr_t kNodeRecord = 0x14;

    // The ids one owner's map holds, valid while the map still has the head and size it had when the index last
    // saw it: the cache functions are the only code that inserts or evicts, and clearing resets the size.
    struct Index
    {
        cMapData* owner = nullptr;
        uint8_t* head = nullptr;
        uint32_t size = 0;
        gtl::flat_hash_map<uint32_t, uint8_t*> nodes;
    };

    struct Cache
    {
        CacheFn original;
        RecordMap cMapData::* map;
        uintptr_t stampOffset;  // last use, in the node
        SharedSpinLock mutex;       // the game calls some of these from its logic thread as well
        Index indexes[4];
        uint32_t next = 0;
    };

    Cache g_layers{nullptr, &cMapData::layerRecords, 0x14 + sizeof(LayerRecord)};
    Cache g_records{nullptr, &cMapData::records, 0x14 + 0x40};

    void stamp(const Cache& c, uint8_t* node)
    {
        *reinterpret_cast<uint32_t*>(node + c.stampOffset) = *Addr::g_recordStamp;
    }

    Index* findIndex(Cache& c, cMapData* owner)
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

    uint8_t* lookup(Cache& c, cMapData* owner, void* edx, uint32_t id)
    {
        if (id == 0)
        {
            return c.original(owner, edx, id);
        }
        RecordMap& map = owner->*c.map;
        {
            std::shared_lock lock(c.mutex);
            const Index* ix = findIndex(c, owner);
            if (ix && ix->head == map.head && ix->size == map.size)
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
        if (ix->head != map.head || ix->size != map.size)
        {
            ix->nodes.clear();
            ix->head = map.head;
            ix->size = map.size;
        }
        uint8_t* node = Addr::recordMapFind(&map, &id);
        if (node != map.head)
        {
            stamp(c, node);     // what the game does on a hit
        }
        else
        {
            D3DStats::count(D3DStats::CRecordRead);
            uint8_t* record = c.original(owner, edx, id);
            if (map.size != ix->size + 1)
            {
                ix->nodes.clear();  // a node was evicted: it may be one the index knows
            }
            ix->head = map.head;
            ix->size = map.size;
            // The new node, unless the eviction took it right away (then the game returns a dangling record).
            node = record ? Addr::recordMapFind(&map, &id) : nullptr;
            if (!node || node == map.head || node + kNodeRecord != record)
            {
                return record;
            }
        }
        ix->nodes.emplace(id, node);
        return node + kNodeRecord;
    }

    uint8_t* __fastcall hookLayers(cMapData* owner, void* edx, uint32_t id) { return lookup(g_layers, owner, edx, id); }
    uint8_t* __fastcall hookRecords(cMapData* owner, void* edx, uint32_t id) { return lookup(g_records, owner, edx, id); }
}

void MapCache::install()
{
    if (!Config::render.recordIndex)
    {
        return;
    }
    Patch::hook(g_layers.original, Addr::layerRecordCache, &hookLayers, "layerRecordCache");
    Patch::hook(g_records.original, Addr::recordCache, &hookRecords, "recordCache");
    LOG("Record caches: hash index in front of the tree lookups");
}
