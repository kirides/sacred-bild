#include "pickup.h"
#include "log.h"
#include "patch.h"
#include "world.h"
#include "game/gear_addr.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <shared_mutex>

namespace
{
    std::shared_mutex g_lock;
    std::vector<uint32_t> g_blocked;    // sorted
    std::vector<uint32_t> g_dimmed;     // sorted
    uint32_t g_dimAlpha = 255;
    std::atomic<uint32_t> g_refused{0};

    bool contains(const std::vector<uint32_t>& sorted, uint32_t id)
    {
        return std::binary_search(sorted.begin(), sorted.end(), id);
    }

    uint32_t objectId(const void* object)
    {
        uint32_t id = 0;
        __try
        {
            std::memcpy(&id, static_cast<const uint8_t*>(object) + 0x0C, sizeof(id));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            id = 0;
        }
        return id;
    }

    bool blockedFor(const void* creature, uint32_t item)
    {
        {
            std::shared_lock lock(g_lock);
            if (g_blocked.empty() || !contains(g_blocked, item))
            {
                return false;
            }
        }
        // The local hero only: other creatures (and the peers' heroes on a host) pick up as before.
        const uint32_t hero = World::hero();
        return hero && creature && objectId(creature) == hero;
    }

    decltype(Gear::Addr::cCreature_pickupItem)::Ptr g_origPickup = nullptr;
    decltype(Gear::Addr::cCreature_collectFilter)::Ptr g_origCollect = nullptr;
    decltype(Gear::Addr::cItem_labelColor)::Ptr g_origLabel = nullptr;

    uint32_t __fastcall hookPickup(void* creature, void* edx, uint32_t id)
    {
        if (blockedFor(creature, id))
        {
            if (g_refused.fetch_add(1) < 20)
            {
                LOG("Pickup: item {} refused (a mod's pickup rules)", id);
            }
            return 0;
        }
        return g_origPickup(creature, edx, id);
    }

    uint8_t __fastcall hookCollect(void* creature, void* edx, void* item)
    {
        if (item && blockedFor(creature, objectId(item)))
        {
            return 0;
        }
        return g_origCollect(creature, edx, item);
    }

    uint32_t __fastcall hookLabel(void* item, void* edx, void* record)
    {
        const uint32_t color = g_origLabel(item, edx, record);
        std::shared_lock lock(g_lock);
        if (g_dimAlpha >= 255 || g_dimmed.empty() || !contains(g_dimmed, objectId(item)))
        {
            return color;
        }
        const uint32_t alpha = ((color >> 24) * g_dimAlpha) / 255;
        return (color & 0x00FFFFFF) | (alpha << 24);
    }
}

bool Pickup::install()
{
    namespace A = Gear::Addr;
    if (!A::cCreature_pickupItem || !A::cCreature_collectFilter || !A::cItemBase_typeDescriptor)
    {
        LOG("Pickup: the pickup code was not found: pickup rules are off");
        return false;
    }
    bool ok = Patch::hook(g_origPickup, Gear::Addr::cCreature_pickupItem, hookPickup, "cCreature_pickupItem");
    ok = Patch::hook(g_origCollect, Gear::Addr::cCreature_collectFilter, hookCollect, "cCreature_collectFilter") && ok;
    if (A::cItem_labelColor)
    {
        ok = Patch::hook(g_origLabel, Gear::Addr::cItem_labelColor, hookLabel, "cItem_labelColor") && ok;
    }
    return ok;
}

void Pickup::setRules(std::vector<uint32_t> blocked, std::vector<uint32_t> dimmed, uint8_t dimAlpha)
{
    std::sort(blocked.begin(), blocked.end());
    std::sort(dimmed.begin(), dimmed.end());
    std::unique_lock lock(g_lock);
    g_blocked.swap(blocked);
    g_dimmed.swap(dimmed);
    g_dimAlpha = dimAlpha;
}

uint32_t Pickup::refused()
{
    return g_refused.load();
}
