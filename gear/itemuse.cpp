#include "itemuse.h"
#include "log.h"
#include "lua_host.h"
#include "game/gear_addr.h"
#include "patch.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace
{
    using namespace Gear;

    decltype(Addr::cPotion3D_receiveEvent)::Ptr g_orig = nullptr;
    std::atomic<bool> g_active{false};

    int messageVirtual(void* message, int slot)
    {
        void** vtable = *reinterpret_cast<void***>(message);
        return reinterpret_cast<int(__thiscall*)(void*)>(vtable[slot])(message);
    }

    void __fastcall hookReceive(void* item, void* edx, void* message)
    {
        if (g_active.load(std::memory_order_relaxed) && item && message)
        {
            auto* words = static_cast<uint32_t*>(message);
            // the engine's own tests: kind 6, class 0x22, word +4 is 0x10 (a drink)
            if (messageVirtual(message, 1) == 6 && messageVirtual(message, 2) == 0x22 && words[1] == 0x10)
            {
                auto* bytes = static_cast<uint8_t*>(item);
                const uint32_t id = *reinterpret_cast<uint32_t*>(bytes + 0xC);
                const uint32_t type = *reinterpret_cast<uint32_t*>(bytes + 0x10);
                int out = 0;
                if (LuaHost::itemUse(id, type, words[2], out) == LuaHost::Answer::Value && out != 0)
                {
                    if (out == 1)
                    {
                        *reinterpret_cast<uint32_t*>(bytes + 0x14) |= 0x8000000;
                    }
                    return;
                }
            }
        }
        g_orig(item, edx, message);
    }

    // The empty wide string the message's list member holds (begin, end, capacity); the engine copies from it, never frees it.
    uint16_t g_emptyText[8] = {};
}

bool ItemUse::install()
{
    if (!Addr::cPotion3D_receiveEvent)
    {
        return false;
    }
    return Patch::hook(g_orig, Addr::cPotion3D_receiveEvent, hookReceive, "cPotion3D_receiveEvent");
}

void ItemUse::setActive(bool active)
{
    g_active.store(active);
}

bool ItemUse::openTeleporter(int sub, const char*& error)
{
    if (!Addr::kernel_instance || !Addr::kernel_send || !Addr::g_pObjectManager || !Addr::cObjectManager_hero)
    {
        error = "not available in this build";
        return false;
    }
    if (sub != 5 && sub != 6)
    {
        error = "sub must be 5 or 6";
        return false;
    }
    __try
    {
        void* hero = Addr::cObjectManager_hero(*static_cast<void**>(Addr::g_pObjectManager.get()));
        if (!hero)
        {
            error = "no hero";
            return false;
        }
        alignas(8) uint8_t message[0x100] = {};
        auto* words = reinterpret_cast<uint32_t*>(message);
        words[0] = 0x890DD0;                     // vtable of the UI message
        words[1] = 0x39;
        std::memcpy(message + 0x48, "DEFAULT_BUSYDLG", 16);   // the window the message is for (the busy dialog compares +0x48 with its name); +0x34 stays empty
        words[2] = 2;
        words[3] = static_cast<uint32_t>(sub);
        words[4] = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(hero) + 0x578);
        // list member at +0x68: an empty wide string (begin, end, capacity)
        words[0x68 / 4 + 0] = reinterpret_cast<uint32_t>(&g_emptyText[0]);
        words[0x68 / 4 + 1] = reinterpret_cast<uint32_t>(&g_emptyText[0]);
        words[0x68 / 4 + 2] = reinterpret_cast<uint32_t>(&g_emptyText[8]);
        Addr::kernel_send(Addr::kernel_instance(), message, 0, 0);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        error = "the engine refused the message";
        return false;
    }
}

bool ItemUse::portals(uint32_t& mask, const uint32_t* set, const char*& error)
{
    if (!Addr::g_pObjectManager || !Addr::cObjectManager_hero)
    {
        error = "not available in this build";
        return false;
    }
    __try
    {
        void* hero = Addr::cObjectManager_hero(*static_cast<void**>(Addr::g_pObjectManager.get()));
        if (!hero)
        {
            error = "no hero";
            return false;
        }
        auto* word = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(hero) + 0x578);
        if (set)
        {
            *word = *set;
        }
        mask = *word;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        error = "cannot read the hero";
        return false;
    }
}
