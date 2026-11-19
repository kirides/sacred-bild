#pragma once
#include "world.h"

#include <cstdint>
#include <string>
#include <vector>

// Party companions in single player (gear.party in docs/GearApi.md): more heroes of the local player at the hero ids 3..16,
// the way a network game holds the players' heroes, one of which the player controls at a time. Everything runs on
// the engine thread (the mods' tick).
namespace Party
{
    // Id 2 stays free: in single player the engine drives object 2 with the arrow keys every frame (a developer
    // feature at ENG 0x6107BC), which keeps a hero there from walking.
    constexpr uint32_t kFirstCompanionId = 3;
    constexpr uint32_t kLastHeroId = 16;

    // Hooks the engine's experience gain (for the party's shared experience).
    void install();
    // Once per frame: keeps the engine's mouse target in step after a savegame was loaded.
    void frame();

    // Whether the party works now: a single player game with a hero. `why` says why not.
    bool available(const char*& why);

    // The local player's heroes in the world (ids 1..16), the controlled one first (another player's hero is never one).
    std::vector<uint32_t> heroes();
    bool isHero(uint32_t id);
    // The mercenaries that follow the local player's heroes (their follower lists); orders, abilities and state work
    // for them as for the heroes.
    std::vector<uint32_t> mercenaries();

    // Creates a hero of class `heroClass` (1..9, the hero types) at `at` with the lowest free id from 3 on, as the
    // engine creates the player's hero. 0 and `error` if it cannot.
    uint32_t recruit(uint32_t heroClass, const World::Position& at, const char*& error);

    // A hero file (AMH: Save\HeroNN.pax of the network game, or one exportHero wrote): what its header (record 0xC3)
    // says. False if it is none.
    struct HeroFile
    {
        std::wstring path;
        std::wstring name;
        uint32_t heroClass = 0;
        uint32_t level = 0;
    };
    bool readHeroFile(const std::wstring& path, HeroFile& out);

    // Creates a companion from a hero file at `at`, as the engine's own LOADHERO does it: everything the hero had (stats,
    // skills, combat arts, equipment, backpack) comes along. 0 and `error` if it cannot.
    uint32_t importHero(const std::wstring& path, const World::Position& at, const char*& error);

    // Writes the controlled hero to a hero file (the network game's format, the engine's SAVEHERO).
    bool exportHero(const std::wstring& path, const char*& error);

    // Removes a companion (not the controlled hero) from the world, with what it wears.
    bool dismiss(uint32_t id, const char*& error);

    // Gives the player control of hero `id`: the players' local id, the owner of every object the old one owned, the
    // engine's mouse target, the mercenaries (they follow the controlled hero) and the gold (the party's purse) move
    // to it. False and `error` if it cannot.
    bool control(uint32_t id, const char*& error);

    enum class OrderKind
    {
        Move,
        Attack,
        Ability,
        Stop,
    };

    struct Order
    {
        OrderKind kind = OrderKind::Stop;
        uint32_t target = 0;    // Attack, Ability: a creature (0: Ability at the position)
        World::Position at;     // Move, Ability without target
        uint32_t ability = 0;   // Ability: the combat art (ids from 1000) or spell
    };

    // Hands an order to a party hero as the mouse does for the controlled one.
    bool order(uint32_t id, const Order& o, const char*& error);

    struct Ability
    {
        uint16_t id = 0;
        uint8_t level = 0;      // read runes
        uint8_t bonus = 0;      // from items
        float regen = 0;        // seconds until it can be used again
        float regenMax = 0;     // its regeneration time
    };
    bool abilities(uint32_t id, std::vector<Ability>& out);

    // What a hero wears: the item ids of its equipment slots (creature +0x1A4, 19 dwords), 0 for an empty slot.
    bool equipment(uint32_t id, uint32_t (&slots)[19]);

    // A combo slot (the combo master's): its id (0x200 + slot, what an Ability order takes), up to four combat arts with
    // their levels, the regeneration time and how far it has regenerated (ready when charge >= total).
    struct Combo
    {
        uint16_t id = 0;
        uint32_t slot = 0;
        uint16_t arts[4] = {};
        uint16_t levels[4] = {};
        float total = 0;
        float charge = 0;
    };
    // The combos with at least one art.
    bool combos(uint32_t id, std::vector<Combo>& out);
    // Writes combo slot `slot` (0..3) as the combo master does: up to four arts the hero knows (at the levels it has).
    bool setCombo(uint32_t id, uint32_t slot, const std::vector<uint16_t>& arts, const char*& error);

    struct State
    {
        uint16_t action = 0;    // +0xFC: 0 idle, 1 attack, 2 walk order, 4 approach to attack, 0xB ability, ...
        uint16_t movement = 0;  // +0x150: 0 standing, 1 walking
    };
    bool state(uint32_t id, State& out);

    // Brings a fallen party hero back with `health` hit points (1..max): the death states are cleared as for a living
    // hero. The engine removes a fallen companion from the world about 25 s after it fell (as a network game removes a
    // dead player's hero), so a mod revives it before that.
    bool revive(uint32_t id, int32_t health, const char*& error);

    // Lets a party hero drink a potion of `kind` (1 healing, as Space does; the game's potion kinds) from its own
    // backpack, as the potion keys do for the controlled hero. False and `error` if it has none or cannot.
    bool usePotion(uint32_t id, uint32_t kind, const char*& error);

    // Experience through the engine's own gain (levels up with skill points). Not shared with the party.
    bool addExperience(uint32_t id, uint32_t amount, const char*& error);

    // 1 hostile, 0 not, -1 unknown (no such creatures).
    int hostile(uint32_t id, uint32_t other);

    // Sets a hero's name (the text the game shows for it, 0x26FF + id): UTF-8, 1 to 30 characters. The game does not
    // save it for companions: a mod sets it again after loading.
    bool setName(uint32_t id, const std::string& name, const char*& error);

    // The UI image index of a creature type's portrait (the party window's), -1 if none.
    int portraitImage(uint32_t type);

    // Experience the party shares: when a hero gains experience from the game, every other party hero within
    // `radius` world units (0: anywhere) gains `share` times as much. 0 turns it off (the default).
    void setExperienceShare(double share, double radius);
}
