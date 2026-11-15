#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// The game world as mods see it: objects by id (the hero, creatures), read and changed through the game's own
// object manager. Every access checks that the object still exists and reads the game's memory with a fault guard,
// so a stale id or a layout that does not match this build gives "no such object" instead of a crash. Engine thread
// only (the Lua host's thread).
//
// Single player: the changes here are made locally. In a network game the peers would not see them (Gear refuses
// them there; see World::changesAllowed).
namespace World
{
    // A position in the world: world units (a tile is about 53.7 of them) and the layer (0 outdoors; dungeons and
    // other maps have their own). Objects keep it at +0x18 with the sector the game files it under.
    struct Position
    {
        int32_t x = 0;
        int32_t y = 0;
        uint8_t layer = 0;
    };

    struct Object
    {
        uint32_t id = 0;
        uint32_t type = 0;          // index into the 0x80-byte type records (data.object_types)
        uint32_t flags = 0;         // +0x14
        Position position;
        bool creature = false;
        // Creatures only:
        int32_t health = 0;
        int32_t maxHealth = 0;
        uint32_t gold = 0;          // heroes carry gold here; 0 for most creatures
        uint32_t experience = 0;
        uint32_t level = 0;
    };

    // False before a game ran, or if the addresses were not found.
    bool available();

    // Whether world changes are allowed now: a game runs and it is not a network game.
    bool changesAllowed();

    // The local hero's id, 0 if there is none.
    uint32_t hero();

    // The object with `id`; false if there is none.
    bool object(uint32_t id, Object& out);

    // Ids of the creatures within `radius` world units of (x, y) on `layer`, nearest first, at most `limit`.
    std::vector<uint32_t> creaturesNear(const Position& at, double radius, size_t limit);

    // Creates an object of type `type` at `at`; its id, or 0 with `error` set.
    uint32_t spawn(uint32_t type, const Position& at, const char*& error);

    // Places object `id` at `at`.
    bool teleport(uint32_t id, const Position& at, const char*& error);

    // Sets a creature's health (clamped to 1..max: a mod does not kill through this) or a hero's gold.
    bool setHealth(uint32_t id, int32_t health, const char*& error);
    bool setGold(uint32_t id, uint32_t gold, const char*& error);
    // Items (objects of class cItemBase: weapons, armour, rings and amulets, potions, runes ...).
    struct Item
    {
        uint32_t id = 0;
        uint32_t type = 0;
        uint8_t kind = 0;           // the type record's +0x2E (docs/Gear.md, "Items")
        uint32_t flags = 0;         // the object's +0x14
        uint32_t typeFlags = 0;     // the type record's +0
        bool onGround = false;      // lying in the world (not in an inventory, not being picked up)
        uint8_t rarity = 0;         // 0..15: 0-4 normal, 5-11 magic, 12-14 rare, 15 unique (the label colours)
        bool setItem = false;       // part of a set (green label)
        uint32_t classMask = 0;     // the classes that can use it (World::classBit); 0 or all: any
        bool gold = false;
        Position position;
    };

    // The item with `id`; false if there is none or it is not an item.
    bool item(uint32_t id, Item& out);

    // Ids of the items lying in the world within `radius` of (x, y) on `layer`, nearest first.
    std::vector<uint32_t> itemsNear(const Position& at, double radius, size_t limit);

    // The hero class of a hero (its type: 1 Seraphim, 2 Gladiator, 3 Battle Mage, 4 Dark Elf, 5 Wood Elf,
    // 6 Vampiress, 8 Dwarf, 9 Daemon), 0 if `id` is no hero; and the bit an item's class mask has for it.
    uint32_t heroClass(uint32_t id);
    uint32_t classBit(uint32_t heroClass);
    constexpr uint32_t kAllClasses = 0xC0003F;
}