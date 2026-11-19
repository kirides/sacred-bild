#include "party.h"
#include "inventory.h"
#include "log.h"
#include "patch.h"
#include "game/gear_addr.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

// Party companions (gear.party in docs/GearApi.md; RE notes in docs/RE_NOTES.md, "Party").
//
// A network game holds up to 16 heroes (object ids 1..16), each played on the computer whose id the players object
// keeps as the local one (+0x14); an object ticks (walks, fights, runs its AI) only on the computer whose id is its
// owner byte (+0x39). In single player the local id is 1 and the player owns everything. A companion is one more
// hero of the local player, made as the engine makes the player's own. Switching control moves the local id and
// the ownership of everything the old id owned to the new hero: the UI, the camera, the inventory (backpack
// container n is the hero with id n), the mouse and the keys then serve it, and the world goes on ticking.
namespace
{
    using namespace Gear;

    constexpr uintptr_t kId = 0x0C, kType = 0x10, kFlags = 0x14, kPos = 0x18, kOwner = 0x39;
    constexpr uintptr_t kAction = 0xFC, kMovement = 0x150;
    constexpr uintptr_t kMaster = 0x251;           // a mercenary's leader (int, unaligned)
    constexpr uintptr_t kHome = 0x2B9;             // the creature's home position (the engine copies +0x18 there)
    constexpr uintptr_t kFollowers = 0x39C;        // vector {begin, end, capacity} of 9-byte {u32 id, u32, u8 active}
    constexpr uintptr_t kStats = 0x3A8;            // the stats block every creature has
    constexpr uintptr_t kExperience = 0x3B4, kGold = 0x3EE, kLevel = 0x3FE, kHealth = 0x4D8;
    constexpr uintptr_t kAbilities = kStats + 0x102;   // vector {begin, end} of 0x16-byte entries
    constexpr uintptr_t kAbilityId = 4, kAbilityLevel = 6, kAbilityBonus = 7, kAbilityRegenMax = 0x0A,
                        kAbilityRegen = 0x12, kAbilitySize = 0x16;
    constexpr uintptr_t kCombos = kStats + 0x10E;      // vector {begin, end} of 0x26-byte combo slots
    constexpr uintptr_t kComboId = 0, kComboArts = 2, kComboLevels = 0x12, kComboTotal = 0x1A, kComboCharge = 0x22,
                        kComboSize = 0x26;
    constexpr uintptr_t kShadowCopy = 0x3B0;       // the stats words the anti-cheat shadow keeps (0x3B dwords + 1 word)
    constexpr uint32_t kFlagGone = 0x10;
    constexpr uintptr_t kEngineControlled = 0x13C; // the creature the world mouse handler commands
    constexpr uintptr_t kPlayersLocal = 0x14;

    // Creature event types (the 0x44-byte orders a creature's vtable +0x18 takes).
    constexpr uintptr_t kNameText = 0x3C;    // a hero's name: the text id 0x26FF + its id
    constexpr uint32_t kOrderMove = 4, kOrderAttack = 7, kOrderAbility = 0x105;

    bool safeCopy(void* dst, const void* src, size_t size)
    {
        __try
        {
            std::memcpy(dst, src, size);
            return true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            return false;
        }
    }

    template <class T>
    bool readAt(uintptr_t base, uintptr_t offset, T& out)
    {
        return base && safeCopy(&out, reinterpret_cast<const void*>(base + offset), sizeof(T));
    }

    template <class T>
    bool writeAt(uintptr_t base, uintptr_t offset, const T& value)
    {
        return base && safeCopy(reinterpret_cast<void*>(base + offset), &value, sizeof(T));
    }

    uintptr_t engine()
    {
        uintptr_t e = 0;
        return Addr::g_uiEventTarget && safeCopy(&e, Addr::g_uiEventTarget.get(), sizeof(e)) ? e : 0;
    }

    uintptr_t players()
    {
        uintptr_t p = 0;
        return Addr::g_pNetPlayers && safeCopy(&p, Addr::g_pNetPlayers.get(), sizeof(p)) ? p : 0;
    }

    // The object vector of the manager: [begin, end).
    bool objectRange(uintptr_t*& begin, uintptr_t*& end)
    {
        uintptr_t m = 0, range[2] = {};
        if (!Addr::g_pObjectManager || !safeCopy(&m, Addr::g_pObjectManager.get(), sizeof(m)) || !m ||
            !safeCopy(range, reinterpret_cast<const void*>(m + 4), sizeof(range)) || range[1] < range[0])
        {
            return false;
        }
        begin = reinterpret_cast<uintptr_t*>(range[0]);
        end = reinterpret_cast<uintptr_t*>(range[1]);
        return true;
    }

    // A hero object: a live creature with a hero type at id 1..16.
    uintptr_t heroObject(uint32_t id)
    {
        if (id < 1 || id > Party::kLastHeroId)
        {
            return 0;
        }
        const uintptr_t o = World::objectAddress(id);
        uint32_t type = 0;
        return o && readAt(o, kType, type) && type >= 1 && type <= 9 && World::heroClass(id) ? o : 0;
    }

    bool sameOwnerAsLocal(uintptr_t o);

    // A party member: a hero (heroObject), or a mercenary of the local player (a creature whose leader, +0x251, is
    // one of the local player's heroes) that the local computer runs.
    uintptr_t memberObject(uint32_t id)
    {
        if (const uintptr_t h = heroObject(id))
        {
            return h;
        }
        if (id <= Party::kLastHeroId)
        {
            return 0;
        }
        World::Object info;
        int32_t master = 0;
        const uintptr_t o = World::objectAddress(id);
        if (!o || !World::object(id, info) || !info.creature || !readAt(o, kMaster, master) || master < 1 ||
            master > static_cast<int32_t>(Party::kLastHeroId))
        {
            return 0;
        }
        const uintptr_t leader = heroObject(static_cast<uint32_t>(master));
        return leader && sameOwnerAsLocal(leader) && sameOwnerAsLocal(o) ? o : 0;
    }

    bool sameOwnerAsLocal(uintptr_t o)
    {
        uint8_t owner = 0;
        return readAt(o, kOwner, owner) && owner == World::localId();
    }

    double distance(uintptr_t a, uintptr_t b)
    {
        int32_t pa[2] = {}, pb[2] = {};
        if (!readAt(a, kPos + 4, pa) || !readAt(b, kPos + 4, pb))
        {
            return 1e18;
        }
        return std::hypot(static_cast<double>(pa[0]) - pb[0], static_cast<double>(pa[1]) - pb[1]);
    }

    // Rewrites the owner byte of every object from `from` to `to`; the count, -1 on a fault.
    int reown(uint8_t from, uint8_t to)
    {
        uintptr_t *begin = nullptr, *end = nullptr;
        if (!objectRange(begin, end))
        {
            return -1;
        }
        int n = 0;
        __try
        {
            for (uintptr_t* p = begin; p < end; ++p)
            {
                uint8_t* o = reinterpret_cast<uint8_t*>(*p);
                if (o && o[kOwner] == from)
                {
                    o[kOwner] = to;
                    ++n;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
        return n;
    }

    // The anti-cheat shadows of the local hero (ENG 0x590730 checks hero()'s experience, gold, level and stats words
    // against them and resets what differs to 1): two pages, the first kept read-protected, each [0] experience,
    // [1] gold, [2] level XOR a key (the hero's byte +0x38 four times; doubled for the second page), then a copy of
    // the creature's +0x3B0 (0x3B dwords and a word). Written as the engine writes them after a legitimate change
    // (the end of 0x55E1B0, 0x57E160, 0x580110).
    bool refreshShadow(uintptr_t hero)
    {
        if (!Addr::g_antiCheatShadow1 || !Addr::g_antiCheatShadow2)
        {
            return false;
        }
        uint32_t* s1 = nullptr;
        uint32_t* s2 = nullptr;
        if (!safeCopy(&s1, Addr::g_antiCheatShadow1.get(), sizeof(s1)) || !safeCopy(&s2, Addr::g_antiCheatShadow2.get(), sizeof(s2)) ||
            !s1 || !s2)
        {
            return false;
        }
        uint8_t keyByte = 0;
        uint32_t experience = 0, gold = 0;
        uint16_t level = 0;
        uint8_t words[0x3B * 4 + 2] = {};
        if (!readAt(hero, 0x38, keyByte) || !readAt(hero, kExperience, experience) || !readAt(hero, kGold, gold) ||
            !readAt(hero, kLevel, level) || !safeCopy(words, reinterpret_cast<const void*>(hero + kShadowCopy), sizeof(words)))
        {
            return false;
        }
        const uint32_t key = keyByte * 0x01010101u;
        DWORD old = 0;
        if (!VirtualProtect(s1, 0x1000, PAGE_READWRITE, &old))
        {
            return false;
        }
        bool ok = true;
        for (int page = 0; page < 2; ++page)
        {
            uint32_t* s = page ? s2 : s1;
            const uint32_t k = page ? key << 1 : key;
            const uint32_t head[3] = {experience ^ k, gold ^ k, static_cast<uint32_t>(level) ^ k};
            ok = safeCopy(s, head, sizeof(head)) && safeCopy(s + 3, words, sizeof(words)) && ok;
        }
        DWORD ignored = 0;
        VirtualProtect(s1, 0x1000, old, &ignored);
        return ok;
    }

    // The backpack tab of the inventory window keeps the hero id it was built for: find it under the UI manager's
    // in-game windows (their children and the tab panel a window keeps at +0x154) and point it at `id`.
    bool retargetTab(uintptr_t control, uint32_t id, int depth)
    {
        uintptr_t vtable = 0;
        if (!control || !readAt(control, 0, vtable))
        {
            return false;
        }
        if (vtable == Addr::cUI_InventoryTab_vtable)
        {
            return writeAt(control, 0x154, id) && writeAt(control, 0x158, static_cast<uint16_t>(id));
        }
        if (depth <= 0)
        {
            return false;
        }
        bool found = false;
        // The inventory window's tab panel (+0x154, a cUI_TabControl) keeps its pages in an inner window at +0x78.
        uintptr_t panel = 0, inner = 0;
        if (readAt(control, 0x154, panel) && panel && panel != control)
        {
            found = retargetTab(panel, id, depth - 1) || found;
        }
        if (readAt(control, 0x78, inner) && inner && inner != control && inner != panel)
        {
            found = retargetTab(inner, id, depth - 1) || found;
        }
        uintptr_t range[3] = {};
        if (readAt(control, 0x78, range) && range[0] && range[1] >= range[0] && range[2] >= range[1] &&
            (range[1] - range[0]) % 4 == 0 && (range[1] - range[0]) / 4 <= 256)
        {
            for (uintptr_t p = range[0]; p < range[1]; p += 4)
            {
                uintptr_t child = 0;
                if (readAt(p, 0, child) && child != control)
                {
                    found = retargetTab(child, id, depth - 1) || found;
                }
            }
        }
        return found;
    }

    bool callTaskbarSetHero(uintptr_t taskbar, uint32_t id)
    {
        __try
        {
            Addr::cUI_Taskbar2_setHero(reinterpret_cast<void*>(taskbar), id, 0);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The taskbar keeps the hero id it shows (weapon and combat art slots, belt): point it at `id` when it differs.
    bool retargetTaskbar(uint32_t id)
    {
        uintptr_t ui = 0, taskbar = 0, vtable = 0;
        uint32_t shown = 0;
        if (!Addr::cUI_Taskbar2_vtable || !Addr::cUI_Taskbar2_setHero || !Addr::g_pUiManager ||
            !safeCopy(&ui, Addr::g_pUiManager.get(), sizeof(ui)) || !ui || !readAt(ui, 0x80, taskbar) || !taskbar ||
            !readAt(taskbar, 0, vtable) || vtable != Addr::cUI_Taskbar2_vtable || !readAt(taskbar, 0x154, shown))
        {
            return false;
        }
        return shown == id || callTaskbarSetHero(taskbar, id);
    }

    bool retargetInventory(uint32_t id)
    {
        uintptr_t ui = 0;
        if (!Addr::cUI_InventoryTab_vtable || !Addr::g_pUiManager || !safeCopy(&ui, Addr::g_pUiManager.get(), sizeof(ui)) || !ui)
        {
            return false;
        }
        bool found = false;
        for (int i = 0; i < 23; ++i)
        {
            uintptr_t w = 0;
            if (readAt(ui, 0x80 + i * 4, w) && w)
            {
                found = retargetTab(w, id, 3) || found;
            }
        }
        return found;
    }

    // Moves the mercenaries (the follower vector, whose members follow and fight for its owner) from one hero to
    // another and tells them their new leader.
    void moveFollowers(uintptr_t from, uintptr_t to, uint32_t toId)
    {
        uintptr_t a[3] = {}, b[3] = {};
        if (!readAt(from, kFollowers, a) || !readAt(to, kFollowers, b))
        {
            return;
        }
        uint32_t fromId = 0;
        readAt(from, kId, fromId);
        // Swap the vectors (the same allocator made both), then point the members at their new leader.
        writeAt(from, kFollowers, b);
        writeAt(to, kFollowers, a);
        const uintptr_t begin = a[0], end = a[1];
        for (uintptr_t e = begin; e && e + 9 <= end; e += 9)
        {
            uint32_t id = 0;
            int32_t master = 0;
            const uintptr_t m = readAt(e, 0, id) ? World::objectAddress(id) : 0;
            if (m && readAt(m, kMaster, master) && static_cast<uint32_t>(master) == fromId)
            {
                writeAt(m, kMaster, static_cast<int32_t>(toId));
            }
        }
    }

    bool deliver(uintptr_t creature, const uint32_t (&event)[0x11])
    {
        __try
        {
            using Handler = uint32_t(__fastcall*)(uintptr_t self, void* edx, const void* event);
            const Handler h = (*reinterpret_cast<Handler* const*>(creature))[6];
            h(creature, nullptr, event);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool callAddExperience(uintptr_t stats, uint32_t amount);

    decltype(Addr::cStats_addExperience)::Ptr g_origAddExperience = nullptr;
    double g_share = 0;
    double g_shareRadius = 0;
    bool g_sharing = false;

    // The engine's experience gain (kills, quests): the party shares it.
    void __fastcall hookAddExperience(void* stats, void* edx, uint32_t amount)
    {
        g_origAddExperience(stats, edx, amount);
        if (g_sharing || g_share <= 0 || amount == 0 || amount >= 0x7FFFFFFF || World::networkGame())
        {
            return;
        }
        uintptr_t gainer = 0;
        uint32_t gainerId = 0;
        if (!readAt(reinterpret_cast<uintptr_t>(stats), 4, gainer) || !readAt(gainer, kId, gainerId) || !heroObject(gainerId))
        {
            return;
        }
        const uint32_t part = static_cast<uint32_t>(std::min(amount * g_share, 2.0e9));
        if (part == 0)
        {
            return;
        }
        g_sharing = true;
        for (uint32_t id : Party::heroes())
        {
            const uintptr_t o = heroObject(id);
            int32_t health = 0;
            if (id == gainerId || !o || !sameOwnerAsLocal(o) || !readAt(o, kHealth, health) || health <= 0 ||
                (g_shareRadius > 0 && distance(o, gainer) > g_shareRadius))
            {
                continue;
            }
            callAddExperience(o + kStats, part);
        }
        g_sharing = false;
    }

    bool callAddExperience(uintptr_t stats, uint32_t amount)
    {
        const auto fn = g_origAddExperience ? g_origAddExperience : Addr::cStats_addExperience.ptr();
        __try
        {
            fn(reinterpret_cast<void*>(stats), nullptr, amount);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool callSetCombo(uintptr_t stats, uint32_t slot, const uint32_t* ids, const uint16_t* levels)
    {
        __try
        {
            Addr::cStats_setCombo(reinterpret_cast<void*>(stats), slot, ids, levels, 0x200 + slot);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool callInitSpells(uintptr_t stats)
    {
        __try
        {
            Addr::cStats_initSpells(reinterpret_cast<void*>(stats), 0, -1);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    int callIsEnemy(uintptr_t a, uintptr_t b)
    {
        __try
        {
            return Addr::cCreature_isEnemy(reinterpret_cast<void*>(a), reinterpret_cast<void*>(b)) & 0xFF ? 1 : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    // The engine takes the buffer (its allocator: engine_alloc) and frees it.
    bool callSetText(uint32_t textId, const wchar_t* text, int length)
    {
        __try
        {
            Addr::textResources_instance();
            const uint32_t bytes = static_cast<uint32_t>((length + 1) * 2);
            auto* buffer = static_cast<wchar_t*>(Addr::engine_alloc(bytes));
            if (!buffer)
            {
                return false;
            }
            std::memcpy(buffer, text, length * 2);
            buffer[length] = 0;
            using SetFn = void(__stdcall*)(uint32_t id, wchar_t* begin, wchar_t* end, wchar_t* capacity);
            reinterpret_cast<SetFn>(Addr::textResources_set)(textId, buffer, buffer + length, buffer + length + 1);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    int callPortrait(uint32_t type)
    {
        __try
        {
            return static_cast<int>(reinterpret_cast<uint32_t(__stdcall*)(uint32_t)>(Addr::creaturePortraitImage)(type));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }
}

namespace
{
    uint32_t freeCompanionId();
}

void Party::install()
{
    if (!Addr::cStats_addExperience)
    {
        LOG("Party: the experience function was not found: no shared experience");
        return;
    }
    Patch::hook(g_origAddExperience, Addr::cStats_addExperience, hookAddExperience, "cStats_addExperience");
}

void Party::frame()
{
    // A savegame load sets the local id back to 1 but leaves the engine's mouse target on the hero that was controlled.
    if (World::networkGame())
    {
        return;
    }
    // The backpack page follows the local id (a load puts it back to 1; the UI may be rebuilt).
    static ULONGLONG nextTab = 0;
    const uint32_t localNow = World::localId();
    if (localNow && GetTickCount64() >= nextTab)
    {
        nextTab = GetTickCount64() + 2000;
        retargetInventory(localNow);
        retargetTaskbar(localNow);
    }
    const uintptr_t e = engine();
    const uint32_t local = World::localId();
    uint32_t controlled = 0;
    if (e && local && readAt(e, kEngineControlled, controlled) && controlled >= 1 && controlled <= kLastHeroId &&
        controlled != local && heroObject(local))
    {
        writeAt(e, kEngineControlled, local);
    }
}

bool Party::available(const char*& why)
{
    if (!World::available() || !World::hero())
    {
        why = "no game is running";
        return false;
    }
    if (World::networkGame())
    {
        why = "the party works in single player only";
        return false;
    }
    if (!engine() || !players())
    {
        why = "the game's engine objects were not found";
        return false;
    }
    return true;
}

std::vector<uint32_t> Party::heroes()
{
    // The local player's heroes only: in a network game the other players' heroes are owned by their computers.
    std::vector<uint32_t> out;
    const uint32_t local = World::localId();
    if (heroObject(local))
    {
        out.push_back(local);
    }
    for (uint32_t id = 1; id <= kLastHeroId; ++id)
    {
        const uintptr_t o = id != local ? heroObject(id) : 0;
        if (o && sameOwnerAsLocal(o))
        {
            out.push_back(id);
        }
    }
    return out;
}

std::vector<uint32_t> Party::mercenaries()
{
    std::vector<uint32_t> out;
    for (uint32_t hero : heroes())
    {
        const uintptr_t h = heroObject(hero);
        uintptr_t range[2] = {};
        if (!h || !readAt(h, kFollowers, range) || !range[0] || range[1] < range[0] || range[1] - range[0] > 9 * 64)
        {
            continue;
        }
        for (uintptr_t e = range[0]; e + 9 <= range[1]; e += 9)
        {
            uint32_t id = 0;
            if (readAt(e, 0, id) && memberObject(id) && id > kLastHeroId &&
                std::find(out.begin(), out.end(), id) == out.end())
            {
                out.push_back(id);
            }
        }
    }
    return out;
}

bool Party::isHero(uint32_t id)
{
    return heroObject(id) != 0;
}

uint32_t Party::recruit(uint32_t heroClass, const World::Position& at, const char*& error)
{
    if (!available(error))
    {
        return 0;
    }
    if (heroClass < 1 || heroClass > 9 || heroClass == 7 || !World::classBit(heroClass))
    {
        error = "no such hero class";
        return 0;
    }
    const uint32_t id = freeCompanionId();
    if (!id)
    {
        error = "the party is full";
        return 0;
    }
    const uint32_t made = World::createObject(heroClass, at, false, error, id);
    const uintptr_t o = made == id ? World::objectAddress(id) : 0;
    if (!o)
    {
        if (made)
        {
            World::destroyObject(made);
        }
        if (!error || !*error)
        {
            error = "the game did not create the hero";
        }
        return 0;
    }
    // What the engine's own hero creation (ENG 0x60D9F0) does after creating the hero: its home is where it stands,
    // and its stats block finishes setting up.
    uint8_t pos[13] = {};
    if (safeCopy(pos, reinterpret_cast<const void*>(o + kPos), sizeof(pos)))
    {
        safeCopy(reinterpret_cast<void*>(o + kHome), pos, sizeof(pos));
    }
    callInitSpells(o + kStats);
    LOG("Party: recruited a hero of class {} as id {}", heroClass, id);
    return id;
}

namespace
{
    std::string narrowPath(const std::wstring& path)
    {
        // The engine opens files with the ANSI functions: the short path survives characters the code page lacks.
        wchar_t shortPath[MAX_PATH * 2] = {};
        const DWORD n = GetShortPathNameW(path.c_str(), shortPath, static_cast<DWORD>(std::size(shortPath)));
        const std::wstring& use = n > 0 && n < std::size(shortPath) ? std::wstring(shortPath, n) : path;
        const int len = WideCharToMultiByte(CP_ACP, 0, use.c_str(), static_cast<int>(use.size()), nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(len > 0 ? len : 0), '\0');
        if (len > 0)
        {
            WideCharToMultiByte(CP_ACP, 0, use.c_str(), static_cast<int>(use.size()), out.data(), len, nullptr, nullptr);
        }
        return out;
    }

    bool callHeroLoad(const char* path, uint32_t id)
    {
        __try
        {
            return (Addr::heroLoad(path, id, 0) & 0xFF) != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    int callHeroSave(const char* path)
    {
        __try
        {
            return Addr::heroSave(path) ? 1 : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    uint32_t freeCompanionId()
    {
        for (uint32_t i = Party::kFirstCompanionId; i <= Party::kLastHeroId; ++i)
        {
            if (!World::objectAddress(i) && Inventory::items(i).empty())
            {
                return i;
            }
        }
        return 0;
    }
}

bool Party::readHeroFile(const std::wstring& path, HeroFile& out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    std::vector<uint8_t> head(0x100 + 16 * 12);
    DWORD got = 0;
    bool ok = ReadFile(h, head.data(), static_cast<DWORD>(head.size()), &got, nullptr) && got == head.size() &&
              head[0] == 'A' && head[1] == 'M' && head[2] == 'H' && head[3] == 0x1B;
    uint8_t c3[0x22C] = {};
    if (ok)
    {
        ok = false;
        for (int i = 0; i < 16 && !ok; ++i)
        {
            uint32_t rec[3];
            std::memcpy(rec, head.data() + 0x100 + i * 12, sizeof(rec));
            if (rec[0] == 0xC3 && rec[2] >= sizeof(c3))
            {
                LARGE_INTEGER at;
                at.QuadPart = rec[1];
                ok = SetFilePointerEx(h, at, nullptr, FILE_BEGIN) && ReadFile(h, c3, sizeof(c3), &got, nullptr) && got == sizeof(c3);
            }
        }
    }
    CloseHandle(h);
    if (!ok)
    {
        return false;
    }
    std::memcpy(&out.level, c3, 4);
    std::memcpy(&out.heroClass, c3 + 4, 4);
    const wchar_t* name = reinterpret_cast<const wchar_t*>(c3 + 8);
    size_t n = 0;
    while (n < 0x40 && name[n])
    {
        ++n;
    }
    out.name.assign(name, n);
    out.path = path;
    return out.heroClass >= 1 && out.heroClass <= 9 && out.heroClass != 7;
}

uint32_t Party::importHero(const std::wstring& path, const World::Position& at, const char*& error)
{
    if (!available(error))
    {
        return 0;
    }
    HeroFile file;
    if (!readHeroFile(path, file))
    {
        error = "not a hero file";
        return 0;
    }
    if (!Addr::heroLoad || !Addr::cObjectManager_unlink || !Addr::cObjectManager_pop)
    {
        error = "the game's hero file functions were not found";
        return 0;
    }
    const uint32_t id = freeCompanionId();
    if (!id)
    {
        error = "the party is full";
        return 0;
    }
    const uint32_t local = World::localId();
    const uint32_t made = World::createObject(file.heroClass, at, false, error, id);
    if (made != id || !World::objectAddress(id))
    {
        if (made)
        {
            World::destroyObject(made);
        }
        if (!error || !*error)
        {
            error = "the game did not create the hero";
        }
        return 0;
    }
    // As the engine's own developer command does it (0x614230): create the hero, load the file into it, then put it
    // back into the world where it should stand.
    const std::string ansi = narrowPath(path);
    uint8_t flag = 0;
    const bool keepFlag = Addr::g_heroFileFlag && safeCopy(&flag, Addr::g_heroFileFlag.get(), 1);
    const bool loaded = !ansi.empty() && callHeroLoad(ansi.c_str(), id);
    if (keepFlag)
    {
        safeCopy(Addr::g_heroFileFlag.get(), &flag, 1);
    }
    if (!loaded)
    {
        World::destroyObject(id);
        error = "the game could not read the hero file";
        return 0;
    }
    const uintptr_t o = World::objectAddress(id);
    if (!o)
    {
        error = "the hero file did not load";
        return 0;
    }
    // The file may carry another player's id: the companion is the local player's, at its own id.
    writeAt(o, kId, id);
    writeAt(o, kOwner, static_cast<uint8_t>(local));
    // and its name is the text of its own id (+0x3C, 0x26FF + id): the file's says the id it was saved with, so the
    // hero would show the name of the hero playing at that id (the overhead label, gear's name()).
    writeAt(o, kNameText, 0x26FFu + id);
    const char* placeError = nullptr;
    World::teleport(id, at, placeError);
    uint8_t pos[13] = {};
    if (safeCopy(pos, reinterpret_cast<const void*>(o + kPos), sizeof(pos)))
    {
        safeCopy(reinterpret_cast<void*>(o + kHome), pos, sizeof(pos));
    }
    LOG("Party: imported a hero of class {} level {} as id {}", file.heroClass, file.level, id);
    return id;
}

bool Party::exportHero(const std::wstring& path, const char*& error)
{
    if (!available(error))
    {
        return false;
    }
    if (!Addr::heroSave)
    {
        error = "the game's hero file function was not found";
        return false;
    }
    // The engine opens the file itself: make sure it can be created (folder exists).
    const std::wstring dir = path.substr(0, path.find_last_of(L"\\/"));
    CreateDirectoryW(dir.c_str(), nullptr);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        error = "cannot write the hero file";
        return false;
    }
    CloseHandle(h);
    const std::string ansi = narrowPath(path);
    if (ansi.empty() || callHeroSave(ansi.c_str()) <= 0)
    {
        error = "the game could not write the hero file";
        return false;
    }
    return true;
}

bool Party::dismiss(uint32_t id, const char*& error)
{
    if (!available(error))
    {
        return false;
    }
    if (!heroObject(id) || id < kFirstCompanionId)
    {
        error = "not a companion";
        return false;
    }
    if (id == World::localId())
    {
        error = "the controlled hero cannot be dismissed";
        return false;
    }
    if (!Inventory::items(id).empty())
    {
        error = "the companion still carries items in its backpack";
        return false;
    }
    if (!World::destroyObject(id))
    {
        error = "the game did not remove the hero";
        return false;
    }
    LOG("Party: dismissed hero {}", id);
    return true;
}

bool Party::control(uint32_t id, const char*& error)
{
    if (!available(error))
    {
        return false;
    }
    const uint32_t old = World::localId();
    const uintptr_t from = heroObject(old);
    const uintptr_t to = heroObject(id);
    if (!to)
    {
        error = "not a party hero";
        return false;
    }
    if (id == old)
    {
        return true;
    }
    if (!from)
    {
        error = "the controlled hero was not found";
        return false;
    }
    int32_t health = 0;
    if (!readAt(to, kHealth, health) || health <= 0)
    {
        error = "the hero is dead";
        return false;
    }
    uint32_t flags = 0;
    if (!readAt(to, kFlags, flags) || (flags & kFlagGone))
    {
        error = "the hero is not in the world";
        return false;
    }
    const uintptr_t p = players();
    const uintptr_t e = engine();
    // The gold is the party's: it goes with control.
    uint32_t goldFrom = 0, goldTo = 0;
    if (readAt(from, kGold, goldFrom) && readAt(to, kGold, goldTo))
    {
        const uint32_t sum = static_cast<uint32_t>(std::min<uint64_t>(static_cast<uint64_t>(goldFrom) + goldTo, 0x7FFFFFFE));
        writeAt(to, kGold, sum);
        writeAt(from, kGold, uint32_t{0});
    }
    const int moved = reown(static_cast<uint8_t>(old), static_cast<uint8_t>(id));
    if (moved < 0 || !writeAt(p, kPlayersLocal, id))
    {
        reown(static_cast<uint8_t>(id), static_cast<uint8_t>(old));
        error = "cannot write the players object";
        return false;
    }
    uint32_t controlled = 0;
    if (readAt(e, kEngineControlled, controlled) && (controlled == old || controlled <= kLastHeroId))
    {
        writeAt(e, kEngineControlled, id);
    }
    moveFollowers(from, to, id);
    if (!retargetInventory(id))
    {
        LOG("Party: the inventory window's backpack page was not found");
    }
    if (!retargetTaskbar(id))
    {
        LOG("Party: the taskbar was not found");
    }
    if (!refreshShadow(to))
    {
        LOG("Party: the anti-cheat shadow could not be refreshed");
    }
    LOG("Party: control {} -> {} ({} objects change owner)", old, id, moved);
    return true;
}

bool Party::order(uint32_t id, const Order& o, const char*& error)
{
    const char* why = nullptr;
    if (!available(why))
    {
        error = why;
        return false;
    }
    const uintptr_t c = memberObject(id);
    if (!c || !sameOwnerAsLocal(c))
    {
        error = "not a party hero";
        return false;
    }
    if (!Addr::g_creatureEventVtable)
    {
        error = "the game's order class was not found";
        return false;
    }
    uint32_t ev[0x11] = {};
    ev[0] = static_cast<uint32_t>(Addr::g_creatureEventVtable.address);
    auto targetPos = [&](uint32_t target, World::Position& out) {
        World::Object t;
        if (!World::object(target, t) || !t.creature)
        {
            return false;
        }
        out = t.position;
        return true;
    };
    switch (o.kind)
    {
    case OrderKind::Move:
        ev[1] = kOrderMove;
        ev[5] = 2;      // walk
        ev[6] = static_cast<uint32_t>(o.at.x);
        ev[7] = static_cast<uint32_t>(o.at.y);
        break;
    case OrderKind::Stop:
    {
        World::Object self;
        if (!World::object(id, self))
        {
            error = "not a party hero";
            return false;
        }
        ev[1] = kOrderMove;
        ev[5] = 0;      // stop
        ev[6] = static_cast<uint32_t>(self.position.x);
        ev[7] = static_cast<uint32_t>(self.position.y);
        break;
    }
    case OrderKind::Attack:
    {
        World::Position at;
        if (!targetPos(o.target, at))
        {
            error = "no such creature";
            return false;
        }
        ev[1] = kOrderAttack;
        ev[6] = o.target;
        ev[7] = static_cast<uint32_t>(at.x);
        ev[8] = static_cast<uint32_t>(at.y);
        break;
    }
    case OrderKind::Ability:
    {
        World::Position at = o.at;
        if (o.target && !targetPos(o.target, at))
        {
            error = "no such creature";
            return false;
        }
        std::vector<Ability> known;
        abilities(id, known);
        std::vector<Combo> combo;
        combos(id, combo);
        if (std::none_of(known.begin(), known.end(), [&](const Ability& a) { return a.id == o.ability; }) &&
            std::none_of(combo.begin(), combo.end(), [&](const Combo& c) { return c.id == o.ability; }))
        {
            error = "the hero does not know that combat art or spell";
            return false;
        }
        ev[1] = kOrderAbility;
        ev[4] = o.target;
        ev[5] = at.sector;
        ev[6] = static_cast<uint32_t>(at.x);
        ev[7] = static_cast<uint32_t>(at.y);
        ev[10] = 100;
        ev[11] = o.ability >= 1000 ? 1 : o.ability >= 0x200 && o.ability < 0x204 ? 2 : 0;    // a combat art (ids from 1000), a combo (0x200 + slot) or a spell
        ev[12] = o.ability;
        ev[13] = at.layer;
        break;
    }
    }
    if (!deliver(c, ev))
    {
        error = "the game faulted on the order";
        return false;
    }
    return true;
}

namespace
{
    constexpr uintptr_t kContainerSize = 0x1180, kWeaponSlots = 0xC18, kWeaponActive = 0xD58, kContainerLocked = 0x1162;

    uintptr_t heroContainer(uint32_t id)
    {
        uintptr_t inv = 0;
        if (!Addr::g_pInventory || id >= 32 || !safeCopy(&inv, Addr::g_pInventory.get(), sizeof(inv)) || !inv)
        {
            return 0;
        }
        return inv + id * kContainerSize;
    }

    bool callWeaponSwitch(uintptr_t c, uintptr_t container, uint32_t slot, uint32_t main, uint32_t off)
    {
        __try
        {
            void* cr = reinterpret_cast<void*>(c);
            void* co = reinterpret_cast<void*>(container);
            const int16_t kind = Addr::cCreature_weaponKind(cr, 0);
            uint8_t locked = 0;
            safeCopy(&locked, reinterpret_cast<const void*>(container + kContainerLocked), 1);
            if (!(locked & 1))
            {
                Addr::cCreature_unequip(cr, 0xC, 1);
                Addr::cCreature_unequip(cr, 0xD, 1);
            }
            Addr::cContainer_selectWeaponSlot(co, slot, 0, 0, 1);
            safeCopy(&locked, reinterpret_cast<const void*>(container + kContainerLocked), 1);
            if (!(locked & 1))
            {
                if (main)
                {
                    Addr::cCreature_equip(cr, 0xC, main, 1);
                }
                if (off)
                {
                    uint32_t twoWeapons = 0, keep = 0;
                    safeCopy(&twoWeapons, reinterpret_cast<const void*>(c + 0x1D8), 4);
                    safeCopy(&keep, reinterpret_cast<const void*>(c + 0x1D4), 4);
                    if (twoWeapons)
                    {
                        Addr::cCreature_equip(cr, 0xC, off, 1);
                        if (!keep)
                        {
                            Addr::cContainer_takeWeapon(co, slot, 1, 0, 1);
                            uintptr_t m = 0;
                            if (Addr::g_pObjectManager && safeCopy(&m, Addr::g_pObjectManager.get(), sizeof(m)) && m)
                            {
                                Addr::cObjectManager_pop(reinterpret_cast<void*>(m), off, reinterpret_cast<void*>(c + 0x18));
                            }
                        }
                    }
                    else
                    {
                        Addr::cCreature_equip(cr, 0xD, off, 1);
                    }
                }
            }
            if (Addr::cCreature_weaponKind(cr, 0) != kind)
            {
                Addr::cCreature_refreshMotions(cr, 1, 0);
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}

bool Party::weaponSlots(uint32_t id, WeaponSlots& out)
{
    const uintptr_t container = heroObject(id) ? heroContainer(id) : 0;
    if (!container)
    {
        return false;
    }
    uint16_t active = 0;
    if (!readAt(container, kWeaponActive, active))
    {
        return false;
    }
    out.active = active;
    for (uint32_t s = 0; s < 5; ++s)
    {
        readAt(container, kWeaponSlots + s * 0x18, out.main[s]);
        readAt(container, kWeaponSlots + s * 0x18 + 0xC, out.off[s]);
    }
    return true;
}

bool Party::useWeaponSlot(uint32_t id, uint32_t slot, const char*& error)
{
    const char* why = nullptr;
    if (!available(why))
    {
        error = why;
        return false;
    }
    const uintptr_t c = heroObject(id);
    const uintptr_t container = c ? heroContainer(id) : 0;
    WeaponSlots slots;
    if (!c || !container || !sameOwnerAsLocal(c) || !weaponSlots(id, slots))
    {
        error = "not a party hero";
        return false;
    }
    if (slot >= 5 || (!slots.main[slot] && !slots.off[slot]))
    {
        error = "that weapon slot is empty";
        return false;
    }
    if (slots.active == slot)
    {
        return true;
    }
    if (!Addr::cCreature_unequip || !Addr::cCreature_equip || !Addr::cContainer_selectWeaponSlot ||
        !Addr::cContainer_takeWeapon || !Addr::cCreature_weaponKind || !Addr::cCreature_refreshMotions ||
        !Addr::g_creatureEventVtable)
    {
        error = "the game's weapon slot functions were not found";
        return false;
    }
    // the order the keys send first (the hero's weapon change)
    uint32_t ev[0x11] = {};
    ev[0] = static_cast<uint32_t>(Addr::g_creatureEventVtable.address);
    ev[1] = 0x114;
    ev[4] = 1;
    ev[5] = slot;
    ev[6] = slots.off[slot];
    ev[7] = slots.main[slot];
    if (!deliver(c, ev) || !callWeaponSwitch(c, container, slot, slots.main[slot], slots.off[slot]))
    {
        error = "the game faulted on the weapon switch";
        return false;
    }
    return true;
}

namespace
{
    bool callLevelOnExp(uintptr_t stats, uint32_t experience, uint32_t& level)
    {
        __try
        {
            level = Addr::cStats_levelOnExp(reinterpret_cast<void*>(stats), experience) & 0xFFFF;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The least experience that gives at least `level`.
    bool experienceFor(uintptr_t stats, uint32_t level, uint32_t& out)
    {
        // an upper bound by doubling (the game's function complains about the largest values)
        uint32_t lo = 0, hi = 1000, got = 0;
        for (;;)
        {
            if (!callLevelOnExp(stats, hi, got))
            {
                return false;
            }
            if (got >= level)
            {
                break;
            }
            if (hi >= 0x40000000u)
            {
                return false;
            }
            lo = hi;
            hi *= 2;
        }
        while (lo < hi)
        {
            const uint32_t mid = lo + (hi - lo) / 2;
            if (!callLevelOnExp(stats, mid, got))
            {
                return false;
            }
            if (got >= level)
            {
                hi = mid;
            }
            else
            {
                lo = mid + 1;
            }
        }
        out = lo;
        return true;
    }
}

bool Party::experienceRange(uint32_t id, uint32_t& now, uint32_t& from, uint32_t& to)
{
    const uintptr_t c = heroObject(id);
    uint16_t level = 0;
    if (!c || !Addr::cStats_levelOnExp || !readAt(c, kExperience, now) || !readAt(c, kLevel, level) || level == 0)
    {
        return false;
    }
    from = 0;
    if (level > 1 && !experienceFor(c + kStats, level, from))
    {
        return false;
    }
    return experienceFor(c + kStats, level + 1u, to);
}

bool Party::abilities(uint32_t id, std::vector<Ability>& out)
{
    out.clear();
    const uintptr_t c = memberObject(id);
    uintptr_t range[2] = {};
    if (!c || !readAt(c, kAbilities, range) || !range[0] || range[1] < range[0] || range[1] - range[0] > kAbilitySize * 512)
    {
        return false;
    }
    for (uintptr_t e = range[0]; e + kAbilitySize <= range[1]; e += kAbilitySize)
    {
        Ability a;
        if (readAt(e, kAbilityId, a.id) && readAt(e, kAbilityLevel, a.level) && readAt(e, kAbilityBonus, a.bonus) &&
            readAt(e, kAbilityRegenMax, a.regenMax) && readAt(e, kAbilityRegen, a.regen) && a.id)
        {
            if (!std::isfinite(a.regen) || a.regen < 0)
            {
                a.regen = 0;
            }
            if (!std::isfinite(a.regenMax) || a.regenMax < 0)
            {
                a.regenMax = 0;
            }
            out.push_back(a);
        }
    }
    return true;
}

bool Party::equipment(uint32_t id, uint32_t (&slots)[19])
{
    const uintptr_t c = memberObject(id);
    return c && readAt(c, 0x1A4, slots);
}

bool Party::combos(uint32_t id, std::vector<Combo>& out)
{
    out.clear();
    const uintptr_t c = heroObject(id);
    uintptr_t range[2] = {};
    if (!c || !readAt(c, kCombos, range) || !range[0] || range[1] < range[0] || range[1] - range[0] > kComboSize * 16)
    {
        return false;
    }
    uint32_t slot = 0;
    for (uintptr_t e = range[0]; e + kComboSize <= range[1]; e += kComboSize, ++slot)
    {
        Combo k;
        k.slot = slot;
        uint32_t arts[4] = {};
        if (!readAt(e, kComboId, k.id) || !readAt(e, kComboArts, arts) || !readAt(e, kComboLevels, k.levels) ||
            !readAt(e, kComboTotal, k.total) || !readAt(e, kComboCharge, k.charge))
        {
            continue;
        }
        bool any = false;
        for (int i = 0; i < 4; ++i)
        {
            k.arts[i] = static_cast<uint16_t>(arts[i]);
            any = any || k.arts[i] != 0;
        }
        if (any && k.id)
        {
            out.push_back(k);
        }
    }
    return true;
}

bool Party::setCombo(uint32_t id, uint32_t slot, const std::vector<uint16_t>& arts, const char*& error)
{
    if (!available(error))
    {
        return false;
    }
    const uintptr_t c = heroObject(id);
    uintptr_t range[2] = {};
    if (!c || !sameOwnerAsLocal(c) || !readAt(c, kCombos, range) || !range[0])
    {
        error = "not a party hero";
        return false;
    }
    const uint32_t slots = static_cast<uint32_t>((range[1] - range[0]) / kComboSize);
    if (slot >= slots || slot > 3 || arts.size() > 4)
    {
        error = "a combo has slots 0 to 3 and at most four combat arts";
        return false;
    }
    std::vector<Ability> known;
    abilities(id, known);
    uint32_t ids[4] = {};
    uint16_t levels[4] = {};
    for (size_t i = 0; i < arts.size(); ++i)
    {
        const auto a = std::find_if(known.begin(), known.end(), [&](const Ability& k) { return k.id == arts[i]; });
        if (a == known.end() || arts[i] < 1000)
        {
            error = "a combo takes combat arts the hero knows";
            return false;
        }
        ids[i] = arts[i];
        levels[i] = static_cast<uint16_t>(a->level + a->bonus);
    }
    if (!Addr::cStats_setCombo || !callSetCombo(c + kStats, slot, ids, levels))
    {
        error = "the game's combo function was not found or faulted";
        return false;
    }
    return true;
}

bool Party::state(uint32_t id, State& out)
{
    const uintptr_t c = memberObject(id);
    return c && readAt(c, kAction, out.action) && readAt(c, kMovement, out.movement);
}

namespace
{
    // The potion keys' steps (0x612FA0) for any party hero: 0 none, 1 drunk, -1 fault.
    int callUsePotion(uintptr_t creature, uintptr_t container, uint32_t kind)
    {
        __try
        {
            const uint32_t item = Addr::cInventory_findPotion(reinterpret_cast<void*>(container), kind, 0);
            if (!item)
            {
                return 0;
            }
            if (!(Addr::cCreature_useItem(reinterpret_cast<void*>(creature), item) & 0xFF))
            {
                return 0;
            }
            Addr::cInventory_remove(reinterpret_cast<void*>(container), item, 1);
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }
}

bool Party::usePotion(uint32_t id, uint32_t kind, const char*& error)
{
    if (!available(error))
    {
        return false;
    }
    const uintptr_t c = heroObject(id);
    uintptr_t base = 0;
    if (!c || !sameOwnerAsLocal(c))
    {
        error = "not a party hero";
        return false;
    }
    if (!Addr::cInventory_findPotion || !Addr::cCreature_useItem || !Addr::cInventory_remove || !Addr::g_pInventory ||
        !safeCopy(&base, Addr::g_pInventory.get(), sizeof(base)) || !base || id >= Inventory::kContainers)
    {
        error = "the game's potion functions were not found";
        return false;
    }
    const int r = callUsePotion(c, base + id * 0x1180, kind);
    if (r <= 0)
    {
        error = r < 0 ? "the game faulted on the potion" : "no such potion in the backpack";
        return false;
    }
    return true;
}

bool Party::revive(uint32_t id, int32_t health, const char*& error)
{
    if (!available(error))
    {
        return false;
    }
    const uintptr_t c = heroObject(id);
    int32_t hp = 0, maxHp = 0;
    if (!c || !sameOwnerAsLocal(c) || !readAt(c, kHealth, hp) || !readAt(c, 0x4D4, maxHp))
    {
        error = "not a party hero";
        return false;
    }
    if (hp > 0)
    {
        error = "the hero is not fallen";
        return false;
    }
    health = std::clamp<int32_t>(health, 1, std::max<int32_t>(maxHp, 1));
    // Dead: action 9 (+0xFC), movement 6 (+0x150); their sub-states and the timer the dead state keeps (+0xFE, +0x152,
    // +0x104) start over.
    const uint16_t zero = 0;
    const bool ok = writeAt(c, kAction, zero) && writeAt(c, 0xFE, zero) && writeAt(c, kMovement, zero) &&
                    writeAt(c, 0x152, zero) && writeAt(c, 0x104, uint32_t{0}) && writeAt(c, kHealth, health);
    if (!ok)
    {
        error = "cannot write the hero";
    }
    return ok;
}

bool Party::addExperience(uint32_t id, uint32_t amount, const char*& error)
{
    if (!available(error))
    {
        return false;
    }
    const uintptr_t c = heroObject(id);
    if (!c || !sameOwnerAsLocal(c))
    {
        error = "not a party hero";
        return false;
    }
    if (!Addr::cStats_addExperience)
    {
        error = "the game's experience function was not found";
        return false;
    }
    const bool was = g_sharing;
    g_sharing = true;   // what a mod gives is not shared
    const bool ok = callAddExperience(c + kStats, amount);
    g_sharing = was;
    if (!ok)
    {
        error = "the game faulted while adding experience";
    }
    return ok;
}

int Party::hostile(uint32_t id, uint32_t other)
{
    const uintptr_t a = World::objectAddress(id);
    const uintptr_t b = World::objectAddress(other);
    World::Object oa, ob;
    if (!a || !b || !Addr::cCreature_isEnemy || !World::object(id, oa) || !World::object(other, ob) || !oa.creature ||
        !ob.creature)
    {
        return -1;
    }
    return callIsEnemy(a, b);
}

bool Party::setName(uint32_t id, const std::string& name, const char*& error)
{
    if (!heroObject(id))
    {
        error = "not a hero";
        return false;
    }
    if (!Addr::textResources_set || !Addr::textResources_instance || !Addr::engine_alloc)
    {
        error = "the game's text functions were not found";
        return false;
    }
    wchar_t wide[32] = {};
    const int n = name.empty() ? 0 : MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), wide, 31);
    if (n < 1 || n > 30)
    {
        error = "the name must be 1 to 30 characters";
        return false;
    }
    if (const uintptr_t o = World::objectAddress(id))
    {
        writeAt(o, kNameText, 0x26FFu + id);
    }
    if (!callSetText(0x26FF + id, wide, n))
    {
        error = "the game faulted while setting the name";
        return false;
    }
    return true;
}

int Party::portraitImage(uint32_t type)
{
    if (!Addr::creaturePortraitImage)
    {
        return -1;
    }
    const int r = callPortrait(type);
    return r > 0 ? r : -1;
}

void Party::setExperienceShare(double share, double radius)
{
    g_share = std::clamp(share, 0.0, 10.0);
    g_shareRadius = std::max(radius, 0.0);
}
