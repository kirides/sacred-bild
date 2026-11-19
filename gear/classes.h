#pragma once
#include <cstdint>

// Custom classes (docs/Gear.md "Classes"): what the engine binds to a hero's creature type, made answerable by mods.
//
//   * classOf (0x5500D0) maps a creature type to the hero class whose action handler runs the creature's combat arts; 14 call
//     sites compare the creature's action word with the class's action id. The hook asks the `ability.route` event for a
//     hero that has an art in flight (+0x120), so a Seraphim body can run a Gladiator art with the Gladiator's code.
//   * the motion player (0x593320) takes the motion slot an action wants; `motion.select` may replace it, so a body without
//     a clip for a slot plays another one.
//   * the model manager's slot tables (Models.tmp: 256 motion record indices per model) can be read and written, so a body can
//     be given another model's clip for a slot.
//   * learn / forget edit a hero's abilities vector (stats +0x102) the way the engine does when a rune is read.
namespace Classes
{
    // Installs the two hooks (optional: without them the events never fire).
    bool install();

    // Whether some mod handles `ability.route` / `motion.select` (the hooks are cheap pass-throughs while not).
    void setRouteActive(bool active);
    void setMotionActive(bool active);

    // Adds the art or spell `ability` (kind 1: combat art / skill, 2: spell; by the id: from 1000 an art) at `level` to hero
    // object `id`'s abilities with the engine's own function; false (and `error`) when it is not a live hero or the id is
    // known already (then nothing changes).
    bool learn(uint32_t id, uint32_t ability, uint32_t level, const char*& error);

    // Removes the ability from hero `id`; false if it had none.
    bool forget(uint32_t id, uint32_t ability, const char*& error);

    // The models' slot tables. `type` is a creature type; slots are 0..255. motionRecord is the record index (0: none) or -1 if
    // the model was not found. hasMotion follows the engine's own lookup (fallbacks included): 1 yes, 0 no, -1 unknown.
    int motionRecord(uint32_t type, uint32_t slot);
    bool setMotionRecord(uint32_t type, uint32_t slot, uint32_t record);
    int hasMotion(uint32_t type, uint32_t slot);
}
