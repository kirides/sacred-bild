#pragma once
#include "game/sacred_addr.h"

#include <cstddef>
#include <cstdint>

// Game objects: everything in the world (creatures, items, buildings, ...) is a cObject, found by its id through
// the object manager.
namespace Sacred
{
    struct cObject
    {
        struct Vtable
        {
            void* _00[9];
            // thiscall (1, 0) -> the pick category, 0..3, ranked by the world pick in that order (with Alt held 2
            // (items), 1, 0, 3).
            uint16_t(__fastcall* pickCategory)(cObject* self, void* edx, int a, int b);
        };

        const Vtable* vtable;
        uint8_t _04[0x0C - 0x04];
        uint32_t id;                // ids with bit 31 set are not objects
        uint8_t _10[0x1C - 0x10];
        int32_t x, y;               // world position (the walk at ENG 004FB7E0 steps it)

        // This object as a cCreature, nullptr if it is none (MSVC's dynamic_cast on the game's type descriptors).
        cCreature* asCreature();
    };
    static_assert(offsetof(cObject::Vtable, pickCategory) == 0x24);
    static_assert(offsetof(cObject, id) == 0x0C);
    static_assert(offsetof(cObject, x) == 0x1C && offsetof(cObject, y) == 0x20);

    struct cCreature : cObject
    {
        // cCreature's vtable adds, at +0x18, the order handler that cEngine::sendOrder hands its orders to.

        // The hero's portrait (ENG 006D73D0) shows health / maxHealth and pulses below a quarter (the low-health
        // mark; the options object's +8, default 0.25, is the same mark). ENG 00549460 reads +0x4D0 + 4 * i.
        uint8_t _24[0x4D4 - sizeof(cObject)];
        int32_t maxHealth;
        int32_t health;

        // Whether `target` is alive and hostile to this creature (the cursor's attack symbol).
        bool isEnemy(cCreature* target) { return Addr::cCreature_isEnemy(this, target) & 0xFF; }
    };
    static_assert(offsetof(cCreature, maxHealth) == 0x4D4 && offsetof(cCreature, health) == 0x4D8);

    // The object manager (one instance).
    struct cObjectManager
    {
        // nullptr before a game ran.
        static cObjectManager* instance() { return *Addr::g_pObjectManager; }

        // The object with this id, nullptr if there is none (or the id is not an object's).
        cObject* object(uint32_t id) { return (id & 0x80000000u) ? nullptr : Addr::cObjectManager_getData(this, id); }

        cCreature* creature(uint32_t id)
        {
            cObject* o = object(id);
            return o ? o->asCreature() : nullptr;
        }

        // The local player's hero, nullptr if there is none.
        cCreature* hero() { return Addr::cObjectManager_hero(this); }
    };

    inline cCreature* cObject::asCreature()
    {
        return static_cast<cCreature*>(Addr::rtDynamicCast(this, 0, reinterpret_cast<void*>(Addr::cObject_typeDescriptor),
            reinterpret_cast<void*>(Addr::cCreature_typeDescriptor), 0));
    }
}
