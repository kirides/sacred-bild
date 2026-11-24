#include "game/pets.h"
#include "game/layout_check.h"
#include "log.h"
#include "patch.h"
#include "sacred/world.h"

#include <windows.h>
#include <d3dtypes.h>
#include <algorithm>
#include <atomic>
#include <unordered_map>

namespace
{
    using namespace Sacred;

    struct Entry
    {
        float scale = 1.0f;
        bool untargetable = false;
    };

    SRWLOCK g_lock = SRWLOCK_INIT;
    std::unordered_map<uint32_t, Entry> g_pets;
    std::atomic<int> g_count{0};
    std::atomic<int> g_untargetable{0};
    thread_local float t_scale = 1.0f;

    decltype(Addr::cObject3D_drawModel)::Ptr g_origDrawModel = nullptr;

    // The engine's object id at +0x0C; the hook knows `self` is an object (cObject3D::drawModel is its method).
    uint32_t __fastcall hookDrawModel(void* self, void* edx, IDirect3DDevice7* device, void* model, void* instance,
        uint32_t flagsLow, uint32_t flagsHigh)
    {
        float scale = 1.0f;
        if (g_count.load(std::memory_order_relaxed) > 0 && self)
        {
            const uint32_t id = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(self) + 0x0C);
            AcquireSRWLockShared(&g_lock);
            const auto it = g_pets.find(id);
            if (it != g_pets.end())
            {
                scale = it->second.scale;
            }
            ReleaseSRWLockShared(&g_lock);
        }
        const float before = t_scale;
        t_scale = scale;
        const uint32_t r = g_origDrawModel(self, edx, device, model, instance, flagsLow, flagsHigh);
        t_scale = before;
        return r;
    }

    decltype(Addr::worldPick)::Ptr g_origPick = nullptr;

    // Pets that cannot be targeted are taken out of the pick list before every pick.
    uint32_t __fastcall hookPick(cWorldView* view, void* edx, int x, int y, uint32_t exclude, int32_t* rect)
    {
        Pets::filterPick(view);
        return g_origPick(view, edx, x, y, exclude, rect);
    }

    void recount()
    {
        int untargetable = 0;
        for (const auto& kv : g_pets)
        {
            untargetable += kv.second.untargetable ? 1 : 0;
        }
        g_count.store(static_cast<int>(g_pets.size()));
        g_untargetable.store(untargetable);
    }
}

void Pets::install()
{
    Patch::begin();
    Patch::hook(g_origDrawModel, Addr::cObject3D_drawModel, &hookDrawModel, "cObject3D::drawModel (pets)");
    Patch::hook(g_origPick, Addr::worldPick, &hookPick, "worldPick (pets)");
    if (Patch::commit())
    {
        LOG("Pets: scale and pick hooks installed");
    }
}

bool Pets::any()
{
    return g_count.load(std::memory_order_relaxed) > 0;
}

void Pets::filterPick(cWorldView* view)
{
    if (!view || !Sacred::Layout::worldPick() || g_untargetable.load(std::memory_order_relaxed) == 0)
    {
        return;
    }
    EnterCriticalSection(&view->pickLock);
    PickEntry* begin = view->pickBegin;
    PickEntry* end = view->pickEnd;
    if (begin && end > begin && static_cast<size_t>(end - begin) <= 4096)
    {
        AcquireSRWLockShared(&g_lock);
        PickEntry* out = begin;
        for (PickEntry* p = begin; p < end; ++p)
        {
            const auto it = g_pets.find(p->id);
            if (it != g_pets.end() && it->second.untargetable)
            {
                continue;
            }
            if (out != p)
            {
                *out = *p;
            }
            ++out;
        }
        ReleaseSRWLockShared(&g_lock);
        view->pickEnd = out;
    }
    LeaveCriticalSection(&view->pickLock);
}

void Pets::scaleWorld(_D3DMATRIX& m)
{
    const float s = t_scale;
    if (s == 1.0f)
    {
        return;
    }
    m._11 *= s; m._12 *= s; m._13 *= s;
    m._21 *= s; m._22 *= s; m._23 *= s;
    m._31 *= s; m._32 *= s; m._33 *= s;
}

// Sacred Gear: registers (or, with scale 1 and untargetable 0, removes) an object. Returns the number of pets.
extern "C" int __cdecl SacredBild_PetSet(unsigned id, float scale, int untargetable)
{
    AcquireSRWLockExclusive(&g_lock);
    if ((scale == 1.0f || !(scale > 0.0f)) && !untargetable)
    {
        g_pets.erase(id);
    }
    else
    {
        Entry& e = g_pets[id];
        e.scale = std::clamp(scale, 0.1f, 4.0f);
        e.untargetable = untargetable != 0;
    }
    recount();
    const int n = g_count.load();
    ReleaseSRWLockExclusive(&g_lock);
    return n;
}

// Sacred Gear: forgets every pet (a savegame was loaded: the object ids mean other objects now).
extern "C" void __cdecl SacredBild_PetClear()
{
    AcquireSRWLockExclusive(&g_lock);
    g_pets.clear();
    recount();
    ReleaseSRWLockExclusive(&g_lock);
}
