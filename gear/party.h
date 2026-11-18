#pragma once
#include "world.h"

#include <cstdint>
#include <vector>

// Party companions in single player (docs/Gear.md, "Party"): more heroes of the local player at the hero ids 3..16,
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

    // The heroes in the world (ids 1..16), the controlled one first.
    std::vector<uint32_t> heroes();
    bool isHero(uint32_t id);

    // Creates a hero of class `heroClass` (1..9, the hero types) at `at` with the lowest free id from 3 on, as the
    // engine creates the player's hero. 0 and `error` if it cannot.
    uint32_t recruit(uint32_t heroClass, const World::Position& at, const char*& error);

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

    struct State
    {
        uint16_t action = 0;    // +0xFC: 0 idle, 1 attack, 2 walk order, 4 approach to attack, 0xB ability, ...
        uint16_t movement = 0;  // +0x150: 0 standing, 1 walking
    };
    bool state(uint32_t id, State& out);

    // Experience through the engine's own gain (levels up with skill points). Not shared with the party.
    bool addExperience(uint32_t id, uint32_t amount, const char*& error);

    // 1 hostile, 0 not, -1 unknown (no such creatures).
    int hostile(uint32_t id, uint32_t other);

    // The UI image index of a creature type's portrait (the party window's), -1 if none.
    int portraitImage(uint32_t type);

    // Experience the party shares: when a hero gains experience from the game, every other party hero within
    // `radius` world units (0: anywhere) gains `share` times as much. 0 turns it off (the default).
    void setExperienceShare(double share, double radius);
}
