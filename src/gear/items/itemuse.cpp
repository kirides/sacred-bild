#include "itemuse.h"
#include "log.h"
#include "lua_host.h"
#include "engine/gear_addr.h"
#include "patch.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace
{
    using namespace Gear;

    decltype(Addr::cPotion3D_receiveEvent)::Ptr g_orig = nullptr;
    decltype(Addr::cCreature_receiveEvent)::Ptr g_creatureOrig = nullptr;
    std::atomic<bool> g_active{false};

    // While the engine's potion handler runs for a drink a mod answered ('consume'): the handler still sends the potion's effect to the
    // drinker (kind 6 messages of the creature event class) and sets the used-up flag; the creature hook drops the effects.
    bool g_dropEffects = false;

    // The no-effect item type the 'keep' answer shows the handler: it plays the drinking sound and does nothing else (no effect, no used-up flag).
    constexpr uint32_t kNoEffectType = 0;

    int messageVirtual(void* message, int slot)
    {
        void** vtable = *reinterpret_cast<void***>(message);
        return reinterpret_cast<int(__thiscall*)(void*)>(vtable[slot])(message);
    }

    void __fastcall hookCreatureReceive(void* creature, void* edx, void* message)
    {
        if (g_dropEffects && message && Addr::g_creatureEventVtable)
        {
            // the potion effects: kind 6 of the creature event class, word +5 the effect (6 .. 10)
            const auto* words = static_cast<const uint32_t*>(message);
            if (words[0] == static_cast<uint32_t>(Addr::g_creatureEventVtable.address) && words[1] == 6 && words[5] >= 6 && words[5] <= 10)
            {
                return;
            }
        }
        g_creatureOrig(creature, edx, message);
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
                        // 'consume': the engine's own handler with its effects dropped: sound, the used-up flag and the item's removal as ever
                        g_dropEffects = true;
                        __try
                        {
                            g_orig(item, edx, message);
                        }
                        __finally
                        {
                            g_dropEffects = false;
                        }
                    }
                    else
                    {
                        // 'keep': the handler runs for a type without an effect (the sound plays, nothing else happens), then the type is back
                        *reinterpret_cast<uint32_t*>(bytes + 0x10) = kNoEffectType;
                        __try
                        {
                            g_orig(item, edx, message);
                        }
                        __finally
                        {
                            *reinterpret_cast<uint32_t*>(bytes + 0x10) = type;
                        }
                    }
                    return;
                }
            }
        }
        g_orig(item, edx, message);
    }

    bool safeRead16(const void* at, uint16_t& out)
    {
        __try
        {
            out = *static_cast<const volatile uint16_t*>(at);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
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
    if (Addr::cCreature_receiveEvent)
    {
        Patch::hook(g_creatureOrig, Addr::cCreature_receiveEvent, hookCreatureReceive, "cCreature_receiveEvent");
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
    if (sub == 0)
    {
        // as the engine's script command and ReBorn's teleporter choose: 6 when the game kind word (0x182EE48) is 1
        uint16_t kind = 0;
        sub = Addr::g_portalListFlag && safeRead16(Addr::g_portalListFlag.get(), kind) && kind == 1 ? 6 : 5;
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

bool ItemUse::showText(uint32_t textId, uint32_t color, const char*& error)
{
    if (!Addr::kernel_instance || !Addr::kernel_send || !textId || textId >= 0x80000000u)
    {
        error = textId && textId < 0x80000000u ? "not available in this build" : "bad text id";
        return false;
    }
    __try
    {
        // the engine's text line message (class ENG 0x890B38, code 0xB, sub 0x10, word +0xC = 3: the id of a text, red in the HUD as the
        // game's own refusals): what a script's text command and the engine's "UI_NET_NO_AUTOPICKUP" line send
        uint32_t message[9] = {};
        message[0] = 0x890B38;
        message[1] = 0xB;
        message[2] = 0x10;
        message[3] = 3;
        message[4] = textId | 0x80000000u;
        message[5] = color;
        Addr::kernel_send(Addr::kernel_instance(), message, 0, 0);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        error = "the engine refused the message";
        return false;
    }
}

bool ItemUse::applyEffect(void* creature, uint32_t effect, uint32_t value, const char*& error)
{
    if (!creature || !Addr::g_creatureEventVtable)
    {
        error = "the game's event class was not found";
        return false;
    }
    __try
    {
        // the message the potion handler sends the drinker: kind 6 (word +4), the effect (+0x14: 6 health, 7 mana, 8 antidote, 9 and 10 the
        // rejuvenation and concentration potions) and its value (+0x18), 0x44 bytes, delivered through the creature's vtable slot 6
        uint32_t message[0x11] = {};
        message[0] = static_cast<uint32_t>(Addr::g_creatureEventVtable.address);
        message[1] = 6;
        message[5] = effect;
        message[6] = value;
        void** vtable = *reinterpret_cast<void***>(creature);
        reinterpret_cast<void(__thiscall*)(void*, void*)>(vtable[6])(creature, message);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        error = "the game faulted on the message";
        return false;
    }
}
