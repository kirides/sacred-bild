#pragma once
#include <cstdint>

// Targets for the controller, from the game's own list of what can be picked on the screen (built by the world
// renderer every frame, scanned by the cursor's world pick): the enemy nearest the hero in a direction, or the
// nearest thing to talk to, open or pick up. The world pick itself is hooked so that a click goes where the
// controller means: at the chosen target whatever overlaps it, or at the ground while the hero only walks.
namespace AimAssist
{
    // Hooks the world pick; call inside a Patch transaction.
    void install();

    struct Target
    {
        uint32_t id = 0;
        float x = 0.0f, y = 0.0f;   // center of its rect, physical pixels
    };

    enum class Kind
    {
        Enemy,      // alive and hostile to the hero
        Interact,   // anything else that can be picked (people, items, containers, doors, stairs)
    };

    // The hero on the screen (center of its rect, else the screen center); false outside the game world.
    bool hero(float& x, float& y);
    // The hero's health and maximum (Sacred::Creature); false without a hero.
    bool heroHealth(int& health, int& maximum);

    // The best target of `kind` within `range` pixels of the hero. With a direction (screen, y down, not 0,0) those
    // within `cone` degrees around it come first, the nearest of them; else the nearest.
    bool find(Kind kind, float dirX, float dirY, float range, float cone, Target& out);

    // Where target `id` is now; false once it is gone (or, for an enemy, dead or no longer hostile).
    bool locate(uint32_t id, Kind kind, Target& out);

    // What the world pick returns: the game's own choice, nothing (clicks walk), or `id` while it is on the screen.
    void pickGame();
    void pickNothing();
    void pickTarget(uint32_t id);
}
