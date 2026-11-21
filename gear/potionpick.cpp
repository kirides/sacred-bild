#include "potionpick.h"
#include "log.h"
#include "lua_host.h"
#include "game/gear_addr.h"
#include "patch.h"

#include <intrin.h>
#include <atomic>
#include <cstdint>

namespace
{
    using namespace Gear;

    using PickFn = uint32_t(__stdcall*)(int32_t mode, int32_t flag, int32_t tier);
    PickFn g_orig = nullptr;
    std::atomic<bool> g_active{false};

    // The return addresses of the engine's callers (ENG, the address after each call) and what they are.
    struct Site
    {
        uintptr_t address;
        const char* name;
    };
    constexpr Site kSites[] = {
        {0x6DB67B, "merchant.first"},       // a merchant opened that is not the one the window showed last: its first stock
        {0x6DB76B, "merchant.reopen"},      // the same merchant opened again: some more potions
        {0x6DB819, "merchant.refresh"},     // the merchant window's refresh message
        {0x6D9DD5, "merchant.wrapper"},     // the one-line function around the call (difficulty, 1, -1)
        {0x6DA188, "merchant.helper"},      // the potion loop of the function at 0x6DA110
        {0x53475B, "death.drop"},           // a creature's death: its potion drop
        {0x55A3BC, "loot"},                 // the loot function
        {0x55A4D9, "loot"},
        {0x486472, "loot.other"},
    };

    const char* siteName(uintptr_t returnAddress)
    {
        for (const Site& s : kSites)
        {
            if (s.address == returnAddress)
            {
                return s.name;
            }
        }
        return nullptr;
    }

    uint32_t __stdcall hookPick(int32_t mode, int32_t flag, int32_t tier)
    {
        if (g_active.load(std::memory_order_relaxed))
        {
            const uintptr_t returnAddress = reinterpret_cast<uintptr_t>(_ReturnAddress());
            int out = 0;
            if (LuaHost::potionPick(mode, flag & 0xFF, tier, returnAddress, siteName(returnAddress), out) == LuaHost::Answer::Value && out > 0)
            {
                return static_cast<uint32_t>(out);
            }
        }
        return g_orig(mode, flag, tier);
    }
}

bool PotionPick::install()
{
    if (!Addr::potionPick)
    {
        return false;
    }
    return Patch::hook(g_orig, Addr::potionPick, hookPick, "potionPick");
}

void PotionPick::setActive(bool active)
{
    g_active.store(active);
}
