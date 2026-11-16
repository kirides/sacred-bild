#include "drops.h"
#include "log.h"
#include "lua_host.h"
#include "patch.h"
#include "world.h"
#include "game/gear_addr.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>

namespace
{
    struct RarityCallSpec
    {
        int kind;                       // 0: call rangeRand (1, 10000); 1: call rand, then `cdq; idiv 10000`
        const uintptr_t* function;
        uint32_t offset;
    };
}
#include "game/gear_drops.inc"

namespace
{
    namespace A = Gear::Addr;
    using RarityEntry = Drops::RarityEntry;

    // --- counters -------------------------------------------------------------------------------------------------------------

    std::atomic<uint64_t> g_rolls{0}, g_forced{0}, g_vetoed{0}, g_picks{0}, g_overridden{0}, g_rarityDraws{0}, g_randCalls{0}, g_luaDraws{0};
    std::atomic<uint64_t> g_quality[16], g_category[32];

    std::atomic<bool> g_rollEvent{false}, g_selectEvent{false}, g_randomEvent{false};
    std::atomic<float> g_chanceScale{1.0f}, g_findScale{1.0f};
    std::atomic<int> g_rngMode{0};

    // --- generator ------------------------------------------------------------------------------------------------------------

    // PCG32 (O'Neill): small, fast, passes the usual statistical batteries. Shared by all threads under a lock (a drop is a handful of
    // draws; nothing here is hot).
    std::mutex g_rngLock;
    uint64_t g_pcgState = 0, g_pcgInc = 0;
    bool g_pcgReady = false;

    uint32_t pcgNext()
    {
        const uint64_t old = g_pcgState;
        g_pcgState = old * 6364136223846793005ULL + g_pcgInc;
        const uint32_t xorshifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
        const uint32_t rot = static_cast<uint32_t>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31u));
    }

    void pcgSeed(uint64_t seed, uint64_t stream)
    {
        g_pcgState = 0;
        g_pcgInc = (stream << 1u) | 1u;
        pcgNext();
        g_pcgState += seed;
        pcgNext();
        g_pcgReady = true;
    }

    void pcgEnsure()
    {
        if (!g_pcgReady)
        {
            LARGE_INTEGER t{};
            QueryPerformanceCounter(&t);
            pcgSeed(static_cast<uint64_t>(t.QuadPart) ^ (static_cast<uint64_t>(GetTickCount64()) << 20) ^ GetCurrentProcessId(), 54u);
        }
    }

    // Uniform in 0 .. n - 1 without modulo bias (rejection of the short last block).
    uint32_t pcgBelow(uint32_t n)
    {
        if (n <= 1)
        {
            return 0;
        }
        std::lock_guard lock(g_rngLock);
        pcgEnsure();
        const uint32_t threshold = (~n + 1u) % n;
        for (;;)
        {
            const uint32_t r = pcgNext();
            if (r >= threshold)
            {
                return r % n;
            }
        }
    }

    // The loot code takes `rand() % 1000` for its chances: a number uniform in 0 .. 31999 (32 whole thousands) is exact for 1000, 100,
    // 10, 2 ... where the CRT's 0 .. 32767 favours the first 768 values of every 1000 by 3%.
    constexpr uint32_t kFairRange = 32000;

    // A uniform draw in [0, 1) from the mods' Lua function, false if there is none (busy, error, nothing returned).
    bool luaUnit(double& u)
    {
        if (LuaHost::dropRandom(u) != LuaHost::Answer::Value || !(u >= 0.0) || !(u < 1.0))
        {
            return false;
        }
        g_luaDraws.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Uniform in 0 .. n - 1 from the selected generator (not the game's own: callers handle mode 0).
    uint32_t drawBelow(uint32_t n)
    {
        if (g_rngMode.load(std::memory_order_relaxed) == 2)
        {
            double u = 0;
            if (luaUnit(u))
            {
                return std::min<uint32_t>(static_cast<uint32_t>(u * n), n - 1);
            }
        }
        return pcgBelow(n);
    }

    // --- rarity table ---------------------------------------------------------------------------------------------------------

    struct RawEntry
    {
        int32_t id;
        uint16_t weight;
        uint16_t pad;
    };

    std::mutex g_rarityLock;
    std::map<int, double> g_multipliers;
    bool g_rarityDirty = false;
    std::vector<RarityEntry> g_snapshot;    // as the game loaded it: id, weight (the `vanilla` field)
    std::vector<uint16_t> g_applied;        // what we wrote, to tell a fresh load from our own values

    bool tableRange(uint8_t*& begin, size_t& count)
    {
        begin = nullptr;
        count = 0;
        if (!A::g_rarityTable)
        {
            return false;
        }
        uint8_t* range[2] = {};
        __try
        {
            std::memcpy(range, A::g_rarityTable.get(), sizeof(range));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        if (!range[0] || range[1] <= range[0] || (range[1] - range[0]) % sizeof(RawEntry) != 0 || (range[1] - range[0]) / sizeof(RawEntry) > 64)
        {
            return false;
        }
        begin = range[0];
        count = static_cast<size_t>(range[1] - range[0]) / sizeof(RawEntry);
        return true;
    }

    bool readTable(std::vector<RawEntry>& out)
    {
        uint8_t* begin = nullptr;
        size_t count = 0;
        if (!tableRange(begin, count))
        {
            return false;
        }
        out.resize(count);
        __try
        {
            std::memcpy(out.data(), begin, count * sizeof(RawEntry));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return true;
    }

    bool writeWeights(const std::vector<uint16_t>& weights)
    {
        uint8_t* begin = nullptr;
        size_t count = 0;
        if (!tableRange(begin, count) || count != weights.size())
        {
            return false;
        }
        __try
        {
            for (size_t i = 0; i < count; ++i)
            {
                std::memcpy(begin + i * sizeof(RawEntry) + 4, &weights[i], sizeof(uint16_t));
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return true;
    }

    // Brings the table in line with the multipliers: a table the game (re)loaded since we last wrote it is the new vanilla.
    void ensureRarity()
    {
        std::lock_guard lock(g_rarityLock);
        if (g_multipliers.empty() && g_snapshot.empty())
        {
            return;
        }
        std::vector<RawEntry> now;
        if (!readTable(now))
        {
            return;
        }
        bool same = !g_applied.empty() && g_applied.size() == now.size() && g_snapshot.size() == now.size();
        for (size_t i = 0; same && i < now.size(); ++i)
        {
            same = now[i].weight == g_applied[i] && now[i].id == g_snapshot[i].id;
        }
        if (same && !g_rarityDirty)
        {
            return;
        }
        if (!same)
        {
            g_snapshot.clear();
            for (const RawEntry& e : now)
            {
                g_snapshot.push_back({e.id, e.weight, e.weight});
            }
        }
        std::vector<uint16_t> target;
        for (const RarityEntry& e : g_snapshot)
        {
            const auto m = g_multipliers.find(e.id);
            double w = e.vanilla;
            if (m != g_multipliers.end())
            {
                w = std::floor(w * m->second + 0.5);
            }
            target.push_back(static_cast<uint16_t>(std::clamp(w, 0.0, 65535.0)));
        }
        if (writeWeights(target))
        {
            g_applied = target;
            g_rarityDirty = false;
        }
    }

    // --- redirected draws -----------------------------------------------------------------------------------------------------

    // Every `call rand` inside the loot function.
    int __cdecl wrapRand()
    {
        const int mode = g_rngMode.load(std::memory_order_relaxed);
        if (mode == 0)
        {
            return A::crtRand();
        }
        g_randCalls.fetch_add(1, std::memory_order_relaxed);
        return static_cast<int>(drawBelow(kFairRange));
    }

    // `call 00443B20` (lo, hi): the rarity roll's range draw.
    int __cdecl wrapRange(int lo, int hi)
    {
        g_rarityDraws.fetch_add(1, std::memory_order_relaxed);
        ensureRarity();
        const int mode = g_rngMode.load(std::memory_order_relaxed);
        if (mode == 0)
        {
            return A::rangeRand(lo, hi);
        }
        g_randCalls.fetch_add(1, std::memory_order_relaxed);
        if (lo < 0)
        {
            lo = 0;
        }
        if (hi < 0)
        {
            hi = 1;
        }
        if (lo == hi)
        {
            return hi;
        }
        if (hi < lo)
        {
            std::swap(lo, hi);
        }
        return lo + static_cast<int>(drawBelow(static_cast<uint32_t>(hi - lo) + 1u));
    }

    // `call rand; cdq; idiv 10000`: the rarity roll of the other two paths; the result is the roll itself (0 .. 9999).
    int __cdecl wrapMod10000()
    {
        g_rarityDraws.fetch_add(1, std::memory_order_relaxed);
        ensureRarity();
        if (g_rngMode.load(std::memory_order_relaxed) == 0)
        {
            return A::crtRand();
        }
        g_randCalls.fetch_add(1, std::memory_order_relaxed);
        return static_cast<int>(drawBelow(10000));
    }

    // --- detours --------------------------------------------------------------------------------------------------------------

    using LootFn = int*(__fastcall*)(void* self, void* edx, int forced, uint32_t bonus, uint32_t survival, uint32_t killer);
    LootFn g_origLoot = nullptr;
    decltype(A::cItem_pickType)::Ptr g_origPick = nullptr;
    thread_local int t_loot = 0;
    thread_local uint32_t t_lootType = 0;
    thread_local bool t_forced = false;     // inside a loot roll for an authored drop (a forced item type): the chance scale leaves it alone

    bool readU32(const void* base, uintptr_t offset, uint32_t& out)
    {
        __try
        {
            std::memcpy(&out, static_cast<const uint8_t*>(base) + offset, sizeof(out));
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    int* __fastcall hookLoot(void* self, void* edx, int forced, uint32_t bonus, uint32_t survival, uint32_t killer)
    {
        g_rolls.fetch_add(1, std::memory_order_relaxed);
        if (forced)
        {
            g_forced.fetch_add(1, std::memory_order_relaxed);
        }
        uint32_t type = 0;
        readU32(self, 0x10, type);
        if (g_rollEvent.load(std::memory_order_relaxed))
        {
            uint32_t levelWord = 0;
            readU32(self, 0x3FC, levelWord);        // the level word is at +0x3FE
            LuaHost::DropRoll info;
            info.creatureType = type;
            info.level = levelWord >> 16;
            info.forced = static_cast<uint32_t>(forced);
            info.bonus = bonus;
            std::memcpy(&info.survival, &survival, sizeof(float));
            info.killer = killer;
            info.network = World::networkGame();
            LuaHost::DropRollAnswer answer;
            if (LuaHost::dropRoll(info, answer) == LuaHost::Answer::Value)
            {
                if (answer.cancel)
                {
                    g_vetoed.fetch_add(1, std::memory_order_relaxed);
                    return nullptr;
                }
                if (answer.hasBonus)
                {
                    bonus = answer.bonus;
                }
            }
        }
        ++t_loot;
        const uint32_t outer = t_lootType;
        const bool outerForced = t_forced;
        t_lootType = type;
        t_forced = forced != 0;
        int* result = g_origLoot(self, edx, forced, bonus, survival, killer);
        t_forced = outerForced;
        t_lootType = outer;
        --t_loot;
        return result;
    }

    uint32_t __fastcall hookPick(void* typeManager, void* edx, uint32_t category, uint32_t level, uint32_t a3, uint32_t a4, uint32_t heroPref,
                                 uint32_t flags, uint32_t a7, uint32_t quality)
    {
        if (t_loot <= 0)
        {
            return g_origPick(typeManager, edx, category, level, a3, a4, heroPref, flags, a7, quality);
        }
        uint32_t replaced = 0;
        if (g_selectEvent.load(std::memory_order_relaxed))
        {
            LuaHost::DropSelect info;
            info.category = category;
            info.level = level & 0xFFFF;    // the loot code pushes `cx`: the upper half is whatever ecx held
            info.heroPref = heroPref;
            info.flags = flags;
            info.quality = quality;
            info.creatureType = t_lootType;
            LuaHost::DropSelectAnswer answer;
            if (LuaHost::dropSelect(info, answer) == LuaHost::Answer::Value)
            {
                if (answer.hasCategory)
                {
                    category = answer.category;
                }
                if (answer.hasLevel)
                {
                    level = answer.level & 0xFFFF;
                }
                if (answer.hasQuality)
                {
                    quality = answer.quality;
                }
                if (answer.type > 0 && answer.type < 0x2000)
                {
                    replaced = answer.type;
                }
            }
        }
        g_picks.fetch_add(1, std::memory_order_relaxed);
        g_quality[std::min<uint32_t>(quality, 15)].fetch_add(1, std::memory_order_relaxed);
        g_category[std::min<uint32_t>(category, 31)].fetch_add(1, std::memory_order_relaxed);
        if (replaced)
        {
            g_overridden.fetch_add(1, std::memory_order_relaxed);
            return replaced;
        }
        return g_origPick(typeManager, edx, category, level, a3, a4, heroPref, flags, a7, quality);
    }

    // --- self test ------------------------------------------------------------------------------------------------------------

    std::mutex g_testLock;
    uint32_t g_testObject = 0, g_testBonus = 0, g_testForced = 0;
    int g_testQueued = 0, g_testDone = 0;
}

bool Drops::install()
{
    if (!A::lootRollFn || !A::cItem_pickType)
    {
        LOG("Drops: the loot code was not found: drop hooks are off");
        return false;
    }
    bool ok = Patch::hook(g_origLoot, A::lootRollFn, hookLoot, "lootRoll");
    ok = Patch::hook(g_origPick, A::cItem_pickType, hookPick, "cItem_pickType") && ok;
    return ok;
}

size_t Drops::installSites()
{
    if (!A::crtRand || !A::rangeRand || !A::lootRollFn)
    {
        LOG("Drops: rand / range addresses not found: the drop generator stays the game's");
        return 0;
    }
    const auto retarget = [](uintptr_t site, uintptr_t expected, const void* replacement) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(site);
        if (!site || p[0] != 0xE8)
        {
            return false;
        }
        int32_t rel = 0;
        std::memcpy(&rel, p + 1, sizeof(rel));
        if (static_cast<uintptr_t>(site + 5 + rel) != expected)
        {
            return false;
        }
        return Patch::redirectCall(site, replacement);
    };
    size_t done = 0, total = 0;
    for (const uint32_t offset : kLootRandCalls)
    {
        ++total;
        done += retarget(A::lootRollFn + offset, A::crtRand, reinterpret_cast<const void*>(&wrapRand)) ? 1 : 0;
    }
    for (const RarityCallSpec& spec : kRarityCalls)
    {
        ++total;
        if (!*spec.function)
        {
            continue;
        }
        done += spec.kind == 0 ? (retarget(*spec.function + spec.offset, A::rangeRand, reinterpret_cast<const void*>(&wrapRange)) ? 1 : 0)
                               : (retarget(*spec.function + spec.offset, A::crtRand, reinterpret_cast<const void*>(&wrapMod10000)) ? 1 : 0);
    }
    LOG("Drops: {} of {} call sites redirected", done, total);
    return done;
}

void Drops::setHandlers(bool roll, bool select, bool random)
{
    g_rollEvent.store(roll);
    g_selectEvent.store(select);
    g_randomEvent.store(random);
}

void Drops::setScales(double chance, double find)
{
    g_chanceScale.store(static_cast<float>(std::clamp(chance, 0.0, 1000.0)));
    g_findScale.store(static_cast<float>(std::clamp(find, 0.0, 1000.0)));
}

double Drops::chanceScale()
{
    // An authored drop (forced item type) is not a chance: with a scale of 0 the loot function would end before it, with a big one it could run
    // more passes and make the item again.
    return t_forced ? 1.0 : g_chanceScale.load(std::memory_order_relaxed);
}

double Drops::findScale()
{
    return g_findScale.load(std::memory_order_relaxed);
}

void Drops::setRng(int mode, bool seeded, uint64_t seed)
{
    {
        std::lock_guard lock(g_rngLock);
        if (seeded)
        {
            pcgSeed(seed, 54u);
        }
    }
    g_rngMode.store(mode < 0 || mode > 2 ? 0 : mode);
}

void Drops::setRarityMultipliers(const std::vector<std::pair<int, double>>& multipliers)
{
    {
        std::lock_guard lock(g_rarityLock);
        g_multipliers.clear();
        for (const auto& [id, m] : multipliers)
        {
            if (m != 1.0)
            {
                g_multipliers[id] = m;
            }
        }
        g_rarityDirty = true;
    }
    ensureRarity();
}

std::vector<Drops::RarityEntry> Drops::rarityTable()
{
    std::lock_guard lock(g_rarityLock);
    std::vector<RawEntry> now;
    std::vector<RarityEntry> out;
    if (!readTable(now))
    {
        return out;
    }
    const bool known = g_snapshot.size() == now.size();
    for (size_t i = 0; i < now.size(); ++i)
    {
        out.push_back({now[i].id, now[i].weight, known ? g_snapshot[i].vanilla : now[i].weight});
    }
    return out;
}

Drops::Stats Drops::stats()
{
    Stats s;
    s.rolls = g_rolls;
    s.forcedRolls = g_forced;
    s.vetoed = g_vetoed;
    s.picks = g_picks;
    s.overridden = g_overridden;
    s.rarityDraws = g_rarityDraws;
    s.randCalls = g_randCalls;
    s.luaDraws = g_luaDraws;
    for (int i = 0; i < 16; ++i)
    {
        s.quality[i] = g_quality[i];
    }
    for (int i = 0; i < 32; ++i)
    {
        s.category[i] = g_category[i];
    }
    return s;
}

void Drops::resetStats()
{
    g_rolls = g_forced = g_vetoed = g_picks = g_overridden = g_rarityDraws = g_randCalls = g_luaDraws = 0;
    for (auto& q : g_quality)
    {
        q = 0;
    }
    for (auto& c : g_category)
    {
        c = 0;
    }
}

bool Drops::testRoll(uint32_t objectId, int count, uint32_t bonus, uint32_t forced)
{
    std::lock_guard lock(g_testLock);
    if (g_testDone < g_testQueued || !g_origLoot || count <= 0 || count > 100000 || !World::objectAddress(objectId))
    {
        return false;
    }
    g_testObject = objectId;
    g_testBonus = bonus;
    g_testForced = forced;
    g_testQueued = count;
    g_testDone = 0;
    return true;
}

std::pair<int, int> Drops::testProgress()
{
    std::lock_guard lock(g_testLock);
    return {g_testDone, g_testQueued};
}

void Drops::frame()
{
    uint32_t object = 0, bonus = 0, forced = 0;
    int todo = 0;
    {
        std::lock_guard lock(g_testLock);
        todo = std::min(g_testQueued - g_testDone, 40);
        object = g_testObject;
        bonus = g_testBonus;
        forced = g_testForced;
    }
    if (todo <= 0)
    {
        return;
    }
    const uintptr_t self = World::objectAddress(object);
    if (!self)
    {
        std::lock_guard lock(g_testLock);
        g_testQueued = g_testDone;
        return;
    }
    for (int i = 0; i < todo; ++i)
    {
        hookLoot(reinterpret_cast<void*>(self), nullptr, static_cast<int>(forced), bonus, 0, 0);
    }
    std::lock_guard lock(g_testLock);
    g_testDone += todo;
}

int Drops::randomFifteen()
{
    return static_cast<int>(pcgBelow(kFairRange));
}

uint32_t Drops::randomBelow(uint32_t n)
{
    return pcgBelow(n);
}
