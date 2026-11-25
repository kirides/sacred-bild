#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

// The LuaJIT state that runs the mods. In game, everything Lua happens on the engine's thread (the in-game loop,
// whose per-frame update calls frame()): the state is created there on the first frame, the mods load then, and
// every event is dispatched from there, so mods never race the game's update. With automation on (automation.h),
// idle() also runs it from the automation thread while no in-game frame runs (the menus); the two never overlap. The trusted side (lua/kernel.lua, embedded as a resource) does the mod handling in Lua; this file
// gives it the few native functions it needs and keeps errors and runaway handlers away from the engine.
namespace LuaHost
{
    // Before the first frame: the module (for the embedded kernel) and the game folder (mods\ is in it).
    void configure(HMODULE module, const std::wstring& gameDir);

    // A file next to the mod manager's: mods\<file> (the run's own folder in an automation run).
    std::wstring stateFilePath(const wchar_t* file);

    // One in-game frame on the engine thread; `dt` in seconds. Starts the state and loads the mods the first time.
    void frame(float dt);

    // Automation only: a tick while no in-game frame runs (the menus). Starts the state the first time.
    void idle();
    void tickIdle();       // the tick itself (idle() and menuFrame() call it; only idle() marks the automation thread as ticking)

    // The result of asking the mods for a value from a site hook (sitehook.h): Value (the integer is set), None (no mod
    // answered or the mod's answer was no number), Busy (Lua is running on this or another thread: the engine goes on with its
    // own value; this is also how a hook that Lua itself triggers stays out of a loop).
    enum class Answer { None, Value, Busy };

    // event `level.factor`: the factor for `level` from the mods; `site` is the hook site's index (tools/sigs/gear_sites.py).
    Answer levelFactor(double level, int site, int& out);

    // event `combat.constant`: the value for the engine's constant `name` (`index` among its sites) from the mods.
    Answer combatConstant(const char* name, double vanilla, int index, double& out);

    // event `creature.ability`: the ability id for a creature of `type` (the AI's vanilla id, -1 where the site has none), at site `index` / `address`.
    Answer creatureAbility(int type, int vanilla, int index, int address, int& out);

    // event `ability.route`: which hero class (1..9) runs the combat art `art` in flight for a hero of `type` (the engine's class
    // `vanilla`, the action word `action`, object `id`).
    Answer abilityRoute(int type, int art, int vanilla, int action, uint32_t id, int& out);

    // event `rune.usable`: whether a hero of `type` may learn `ability` (the engine's answer `vanilla`, 0/1); `out` 0/1.
    Answer runeUsable(int type, int ability, int vanilla, int& out);

    // event `pfx.select`: a new effect object of type `fx` (0x300..0x49D) made by `creature` (0: none known) with `ability` in flight, at cast
    // `level` (the effect object's level word), whose parent effect has variant `parent` (0: none); `out` the id of the gear.pfx variant to
    // use (Answer::Value), else the engine's own look.
    Answer pfxSelect(uint32_t fx, uint32_t creature, uint32_t ability, uint32_t level, uint32_t parent, uint32_t& out);

    // event `item.use`: a potion is drunk by `creature` (item object `item`, item type `type`); `out` 0 = the engine's effect, 1 = the potion is
    // used up and has no effect, 2 = no effect and the potion stays.
    Answer itemUse(uint32_t item, uint32_t type, uint32_t creature, int& out);
    // events of the combo master's rune exchange tab (src/gear/items/master.cpp): `master.price` (the gold the exchange costs: `rune` the item type of the
    // chosen rune, `ability` the art or spell it teaches, `count` the runes in the slots), `master.layout` (true: the tab takes no runes, its slots are hidden)
    // and `master.exchange` (true: the mod did the exchange, the engine's rune exchange is skipped). `out`: the number the handler returned (a boolean as 0/1).
    Answer masterEvent(const char* event, int rune, int ability, int count, double& out);
    // event `kill.recorded`: the engine recorded a kill of the local hero or a creature it owns (the statistics record `id`). Skipped
    // while Lua is busy (the engine's own code calls this, never nested in a kernel call).
    void killRecorded(uint32_t id);
    // event `shop.stock`: the engine stocked the merchant window (`reason` 'new', 'restock' or 'refresh'; see src/gear/items/shop.cpp). Skipped while Lua is busy.
    void shopStock(const char* reason, uint32_t merchant, uint32_t container, uint32_t heroLevel);
    // event `potion.pick`: the engine's potion picker is called (`mode`, `flag`, `tier` as it takes them; `address` the caller's return address, `site` its
    // name or nullptr); `out` the item type a mod chose (Answer::Value), else the engine picks.
    Answer potionPick(int mode, int flag, int tier, uintptr_t address, const char* site, int& out);

    // event `motion.select`: the motion slot to play for a hero of `type` that is about to play `slot`.
    Answer motionSelect(int type, int slot, uint32_t id, int& out);

    // event `effect.add`: the engine adds a value to a creature's effect entry. `info` is what it is about to do, `answer` what the mod wants.
    struct EffectInfo
    {
        int creature = 0, ability = 0, effect = 0, flags = 0, counter = 0;
        double value = 0;
        float max = 0, current = 0;
    };
    struct EffectAnswer
    {
        bool hasValue = false, clamp = true, finish = false;
        double value = 0;
        int effect = -1;        // -1: the entry the engine is at
        int stack = 0;          // 1 the creature's effect counter becomes 1, 2 it grows by one
    };
    Answer effectAdd(const EffectInfo& info, EffectAnswer& answer);

    // event `bonus.constant`: the number the item bonus code pushes at site `index` (`vanilla` the engine's own).
    Answer bonusConstant(int index, double vanilla, int address, double& out);

    // events of the drop code (src/gear/items/drops.cpp). `drops.roll`: a creature (or other source) is about to run the loot function. The answer may
    // cancel the roll or change the rarity bonus.
    struct DropRoll
    {
        uint32_t creatureType = 0, level = 0, forced = 0, bonus = 0, killer = 0, objectId = 0;
        float survival = 0;
        bool network = false;
    };
    struct DropRollAnswer
    {
        bool cancel = false, hasBonus = false;
        uint32_t bonus = 0;
    };
    Answer dropRoll(const DropRoll& info, DropRollAnswer& answer);

    // `drops.select`: the loot function asks the equipment type picker for an item. The answer may change the category, level and rarity or
    // name the item type (> 0) to use instead.
    struct DropSelect
    {
        uint32_t category = 0, level = 0, heroPref = 0, flags = 0, quality = 0, creatureType = 0;
    };
    struct DropSelectAnswer
    {
        bool hasCategory = false, hasLevel = false, hasQuality = false;
        uint32_t category = 0, level = 0, quality = 0, type = 0;
    };
    Answer dropSelect(const DropSelect& info, DropSelectAnswer& answer);

    // `drops.random`: a number in [0, 1) for the loot rolls from the mod that chose a Lua generator.
    Answer dropRandom(double& out);

    // event `skillslot.select`: a digit key selected a skill slot (`index` the place in the handler); Value when a mod ran.
    Answer skillSlot(int index);

    // event `skillslot.key`: the routing of a character key: slot 0 .. 4 and mode 0 (the whole case: page, COMBINE_SLOTS check, selection) or 1 (the
    // selection only), slot -1 ignores the key. Value when a mod answered.
    Answer skillSlotKey(int character, int& slot, int& mode);

    // gear.game.cursor_click(down): the UI gets a mouse event at the cursor (ReBorn's quick cast sends a button down). False if the UI or the cursor is not there.
    bool cursorClick(bool down);

    // event `combat.branch`: whether a conditional jump is taken (`vanilla`: the engine's own condition).
    Answer branch(const char* name, int index, bool vanilla, int eax, bool& take);

    // event `damage`: the final damage of a hit; `victimType` / `attackerType` -1 for none. The answer is not yet clamped.
    Answer damage(int damage, int victimType, int attackerType, int index, int victimId, int attackerId, int& out);

    // GetTickCount64() of the last in-game frame, 0 before the first.
    unsigned long long lastFrameTime();

    // From the UI pass while a menu is up (no in-game frame runs there): ticks the mods so their menu UI draws,
    // like idle() does for automation. A no-op in game and while automation ticks.
    void menuFrame();
}
