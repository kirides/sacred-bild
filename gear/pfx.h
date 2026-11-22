#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Skill particle effects of Gear (gear.pfx in docs/GearApi.md): variants of the engine's own effects with other textures, colours and
// speed, and rules that pick a variant when a combat art or spell makes its effect.
//
// The engine has no effect table to extend. Every effect is an object type id (0x300..0x49D, the object type of a cObjectFX) that
// cObjectFX's setup (ENG 005A0C50) maps to one of ~180 hard-coded particle system classes, each with its colours, textures and
// timings in its own code. So Gear leaves the classes alone and changes how one instance looks and runs:
//   * textures: the constructors keep the handles that the texture lookup by file name (ENG 00657360) gave them in fields of the system; a
//     variant swaps the handle of a listed name for the replacement's, inside the object (its size per effect type: gear_pfx.inc);
//   * speed: the particle manager's update of that system (ENG 007CC080) gets dt * speed, which scales every particle's life and the
//     effect's duration with it;
//   * colour: the shared sprite drawing (ENG 00761B00) runs with the variant active, and the device's DrawPrimitive turns the diffuse
//     colour of the vertices it draws (hue shift, colorize, multiply, alpha).
// Which variant an effect gets is decided at its first update, when its cObjectFX has its owner (+0x30: the creature that cast it, or the
// effect that made it) and the cast level (+0xE4): the rules and the mods' `pfx.select` handlers are asked then. An effect made by a
// variant's effect (an impact, a trail) looks like its parent unless a rule says otherwise.
// Everything runs on the engine thread. Effects are local: peers see the engine's own.
namespace Pfx
{
    struct Variant
    {
        uint32_t id = 0;                // the mod's number (stable)
        std::vector<std::pair<std::string, std::string>> textures;  // file name -> replacement file name (both of the engine's texture list)
        bool hasHue = false;
        float hue = 0;                  // degrees added to the hue of every vertex colour
        bool hasColorize = false;
        uint32_t colorize = 0;          // 0xRRGGBB: the vertex keeps its brightness, takes this colour (white stays white)
        bool hasMultiply = false;
        uint32_t multiply = 0xFFFFFFFF; // 0xAARRGGBB: multiplied into the vertex colour
        float alpha = 1;                // multiplies the alpha
        float speed = 1;                // the effect runs this much faster (0.5: twice as long)
    };

    // Whether the hooks went in (false: no variant changes anything).
    bool install();

    bool define(const Variant& variant, const char*& error);
    bool undefine(uint32_t id);
    const Variant* find(uint32_t id);

    // The fx types a `pfx.select` question is asked for (a rule or a handler wants them); 0 as `fx` watches every type.
    void watch(const std::vector<uint32_t>& fxTypes, bool all);

    // For the tests: how many systems carry a variant now, and how often a variant was applied since the start.
    // Logs the first words of each new effect object at its first update (finding its fields).
    void setDebug(bool on);
    size_t liveCount();
    uint64_t appliedCount();

    // What the select question gets: the effect type, the creature that is making it (0: none: an engine effect of its own) and that
    // creature's ability in flight (+0x120). Set by the hook, read by the Lua host while it asks.
    struct Context
    {
        uint32_t fx = 0;
        uint32_t creature = 0;
        uint32_t ability = 0;
    };
}
