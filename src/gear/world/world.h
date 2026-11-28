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
        uint16_t sector = 0;    // the sector word of the position (the game's sector table slot), 0: not filed
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
        uint32_t casting = 0;       // the art or spell the creature has in flight (word at +0x120), 0 if none
        bool inCombat = false;      // bit 0x8000 of the state word at +0x200: the engine's own "enemies about" flag
    };

    // False before a game ran, or if the addresses were not found.
    bool available();

    // Whether a network game (hosting, joined, LAN, any) is running: the engine's flag byte at 0x0182EBEC (ENG), the
    // byte after the players pointer, which its own object and inventory code test before it tells the peers (0x005FBDB0,
    // 0x005D9F00 ...). Single player: 0.
    bool networkGame();

    // The kind of game: single player, or a network game by how it was joined. The engine keeps two flag bytes in its
    // net object (`*g_pNetPlayers`, the one with the local hero id at +0x14): +0x3818 is set when the multiplayer menu's
    // Internet entries (Closed or Open) were used (the menu's mode 4 or 6, 007D8560), and +0x3819 when the game's
    // announcement has bit 8 set, which is the server's "lobby identity checked" flag (Closed Net: the heroes belong to
    // lobby accounts; GameServer reply byte bit 3). Default-deny: an Internet game counts as Closed Net unless Open Net is
    // proven, by the hero's source: in Closed Net the server delivers the hero (message 0x132, NetMsg::heroFromServer) and
    // the client keeps no local one; in Open Net nothing comes, so after the hero has been in the game for 10 s without one
    // (and with the receive hook in) the game reads as Open. LAN: only when entered through the LAN menu.
    enum class NetworkMode { Single, Lan, Open, Closed };
    NetworkMode networkMode();

    // Whether world changes are allowed now: a game runs, it is not a network game (not even a host nobody has joined:
    // the engine would announce the changes to peers that do not know the objects) and the local hero is the only one.
    bool changesAllowed();

    // Whether the local hero's inventory may be changed through the engine's own item calls: single player as
    // changesAllowed(), and in a network game too (LAN, open net) because Gear's inventory code then takes the engine's
    // networked variants, which tell the peers (src/gear/items/inventory.cpp). False without a hero.
    bool inventoryChangesAllowed();

    // Creates an object of `type` at `at` with the engine's own create call, whatever the game kind: `broadcast` makes the
    // engine announce it to the peers in a network game (message 0x16F). The caller has checked that it may.
    uint32_t createObject(uint32_t type, const Position& at, bool broadcast, const char*& error, uint32_t want = 0);

    // Automation self tests only: sets or clears the network flag (nothing else runs in between: the mod code runs on
    // the engine's thread), to test that the guards refuse in a network game.
    bool setNetworkFlagForTest(bool on);
    // The same for the kind of game (the net object's two bytes too); the caller restores what it read with networkMode().
    // `proven`: Open Net counts as proven (without it an Internet game reads as Closed Net: the default).
    bool setNetworkModeForTest(NetworkMode mode, bool proven = true);

    // The local hero's id, 0 if there is none.
    uint32_t hero();

    // The object with `id`; false if there is none.
    bool object(uint32_t id, Object& out);

    // Ids of the creatures within `radius` world units of (x, y) on `layer`, nearest first, at most `limit`.
    std::vector<uint32_t> creaturesNear(const Position& at, double radius, size_t limit);

    // Objects the engine's own code created (sector fills, spawn tables, scripts): not Gear's own spawns, not objects read from a
    // savegame or the network (those ask for an id), not heroes. Recorded only while setCreatedActive(true); the caller checks
    // that the object is still a live creature when it drains the list (the hook runs inside the create call).
    struct Created
    {
        uint32_t id;
        uint32_t type;
        int32_t x;
        int32_t y;
        uint8_t layer;
    };
    bool installCreateHook();
    void setCreatedActive(bool on);
    // While on, creations are not recorded (the handlers of the event run: a creature they make must not raise the event again).
    void setCreatedSuppress(bool on);
    // The creations since the last call (at most 2048 are kept; `dropped` is the running count of the ones that did not fit).
    std::vector<Created> takeCreated(uint64_t* dropped = nullptr);

    // Creates an object of type `type` at `at`; its id, or 0 with `error` set.
    uint32_t spawn(uint32_t type, const Position& at, const char*& error);

    // Places object `id` at `at`.
    bool teleport(uint32_t id, const Position& at, const char*& error);

    // Removes creature `id` (not a hero) from the world through the engine's own object destroy: gone at once, no corpse, no loot, no experience.
    bool remove(uint32_t id, const char*& error);

    // Sets a creature's health (clamped to 1..max: a mod does not kill through this) or a hero's gold.
    bool setHealth(uint32_t id, int32_t health, const char*& error);
    // Gives creature `id` the allegiance of creature `from`: the faction (the class index of the game's 16x16 relation table, +0x1F0)
    // and the low word of the behavior flags (+0x1F4: 1 = aggressive). A plain create takes the faction of the type; the game's
    // sector fill and scripts set it per creature (a soldier can be hostile to the hero by its spawn, neutral by its type).
    bool copyAllegiance(uint32_t id, uint32_t from, const char*& error);
    // A creature's faction (0..15, the class of the relation table) and the low word of its behavior flags (bit 0: aggressive); false if there is no
    // such creature. setAllegiance writes them (not for heroes); `behavior` is the low word only.
    bool allegiance(uint32_t id, uint32_t& faction, uint32_t& behavior);
    bool setAllegiance(uint32_t id, uint32_t faction, uint32_t behavior, const char*& error);

    // The creature flag words, by name: 0 the object word (+0x14, any object), 1 the behavior word (+0x1F4), 2 the state word (+0x200),
    // 3 the rank word (+0xB0: bit 0x400 marks a champion, whose ring is drawn). The last three are creatures'. setFlagWord writes the
    // whole word (single player; the bits that the object manager owns, 0x2 and 0x10 of the object word, stay as they are).
    bool flagWord(uint32_t id, uint32_t which, uint32_t& value);
    bool setFlagWord(uint32_t id, uint32_t which, uint32_t value, const char*& error);
    // Gives creature `id` (not a hero) the level of creature `from` through the engine's own setLevel (ENG 00564D60, which the sector
    // fill calls for the creatures it places: the experience the level starts at, the derived stats and the health come from the
    // creature's type and that level), then its health in the same share of the maximum as the donor's. A plain create is level 1
    // with 100 health whatever the type.
    bool copyStats(uint32_t id, uint32_t from, const char*& error);
    // Sets a creature's (not a hero's) level through the engine's own setLevel (ENG 00564D60, as copyStats does): the experience the level
    // starts at, the derived stats and the maximum health come from the creature's type and that level (a plain create is level 1). The
    // creature keeps its share of the health.
    bool setLevel(uint32_t id, uint32_t level, const char*& error);

    // A pet (SacredBild): the creature's model is drawn `scale` times as large (0.1 to 4) and, if `untargetable`, it is not in the
    // world pick (no click, hover cursor or controller aim reaches it). Scale 1 and not untargetable removes it. Needs SacredBild's
    // ddraw.dll; the object id is only valid until the game ends or another savegame loads (clearPets).
    bool setPet(uint32_t id, double scale, bool untargetable, const char*& error);
    void clearPets();     // also forgets every tint
    // Tints an object's model (SacredBild): the colour (0xAARRGGBB; 0 or 0xFFFFFFFF: none) multiplied over the whole model (`part` 0) or over
    // one of its meshes (`part` 1..8, in the order the engine draws them). The object id is only valid until the game ends or another
    // savegame loads (clearPets).
    bool setTint(uint32_t id, uint32_t part, uint32_t argb, const char*& error);
    // Other textures for an item type's inventory icon (SacredBild), and the brackets the icon draw hook puts around the game's draw of one.
    bool setTypeTexture(uint32_t type, uint32_t part, uint32_t image, const char*& error);
    void iconBegin(uint32_t type);
    void iconEnd();
    // Marks a creature as one that talks (the game's hover cursor over it becomes the talk cursor): its speech id at +0x245, above 0.
    bool setTalk(uint32_t id, int32_t value, const char*& error);
    // Draws one mesh (`part` 1..8) or the whole model (0) with another texture: `image` is a Texture.pak image (Ui::textureImage) or a picture
    // (Ui::pictureCreate*; its sides must be powers of two so the model's texture coordinates fit), 0 takes it away. SacredBild.
    bool setTexture(uint32_t id, uint32_t part, uint32_t image, const char*& error);
    // Sets a creature's maximum health (1 and up, not a hero); its current health becomes `health` when `setCurrent`, else keeps its
    // share of the maximum (at least 1). For the boss modifiers (gear.world, creature:set_modifiers).
    bool setMaxHealth(uint32_t id, int32_t maxHealth, bool setCurrent, int32_t health, const char*& error);
    bool setGold(uint32_t id, uint32_t gold, const char*& error);
    // Items (objects of class cItemBase: weapons, armour, rings and amulets, potions, runes ...).
    struct Item
    {
        uint32_t id = 0;
        uint32_t type = 0;
        uint8_t kind = 0;           // the type record's +0x2E (docs/RE_NOTES.md, "Items, inventories and pickup")
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

    // The address of the live item object `id` (an item, not any object), 0 if there is none: for the code that
    // reads and rewrites an item in place (src/gear/items/inventory.cpp). Valid until the item is destroyed.
    uintptr_t itemObject(uint32_t id);

    // The address of the live object id of any kind (creature, item ...), 0 if there is none: for Gear's own native code (src/gear/items/drops.cpp's
    // loot self test calls the engine's loot function with a creature). Valid until the object is destroyed.
    uintptr_t objectAddress(uint32_t id);

    // The gait of a creature (its field at +0x1F8: 1 walk, 2 run): reads it, or writes it when `set` is true. False when the creature is not found.
    bool gait(uint32_t id, int32_t& value, bool set);

    // The 19 item ids a live creature carries (its equipment words at +0x1A4; heroes, monsters and mercenaries share the layout), 0 for an empty
    // slot. False if there is no such creature.
    bool carried(uint32_t id, uint32_t (&slots)[19]);
    // Puts item object `item` (made with spawn, lying in the world) into equipment word `slot` (0..18) of a creature that is no hero: the item
    // leaves the world (the engine's unlink) and the creature carries it there, as the creatures the game fills a sector with do. Single player.
    bool carry(uint32_t id, uint32_t slot, uint32_t item, const char*& error);

    // The local player's hero id as the players object keeps it (+0x14; 1 in single player unless gear.party switched
    // control), 0 if there is none. Objects tick only on the computer whose id is their owner byte (+0x39).
    uint32_t localId();

    // The text id of a creature's own name (+0x3C; 0: it shows its type's name) and setting it (not for heroes: their name is
    // 0x26FF + id, see Party::setName). The name shown over the creature, in its hover label and the game's lists comes from this
    // text. The field is part of the savegame, so a text id of Gear's own (Classes::keyText) comes back after a load.
    bool nameText(uint32_t id, uint32_t& text);
    bool setNameText(uint32_t id, uint32_t text, const char*& error);

    // The game's clock: minutes since midnight (0..1439) of the calendar that runs the day and night; false if not found.
    bool clockMinutes(uint32_t& minutes);

    // Destroys an object through the engine (heroes too: the caller checked that it may). False if there was none.
    bool destroyObject(uint32_t id);
}
