#include "sitehook.h"
#include "drops.h"
#include "log.h"
#include "lua_host.h"
#include "patch.h"
#include "game/gear_addr.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>
#include <string>
#include <vector>

namespace
{
    struct SiteSpec
    {
        const char* hook;
        const uintptr_t* function;      // the function that holds the site
        uint32_t offset;                // of the site in it
        uint8_t length;                 // the block's length
        uint8_t prefix;                 // the block's leading instruction that loads the level (4 or 0)
    };
    struct ConstSiteSpec
    {
        const char* name;
        int index;
        const uintptr_t* function;
        uint32_t offset;
        uint8_t multiply;           // 1: fmul (D8 0D), 0: fld (D9 05)
        uint32_t constant;          // the pooled constant's address the instruction loads
    };

    struct AbilitySiteSpec
    {
        int index;
        const uintptr_t* function;
        uint32_t offset;
        uint8_t kind;               // 0 store, 1 add, 2 edx
    };
    struct DamageSiteSpec
    {
        int index;
        const uintptr_t* function;
        uint32_t offset;
    };
    struct PushSiteSpec
    {
        int index;
        const uintptr_t* function;
        uint32_t offset;
        uint8_t kind;               // 0 push imm8, 1 push imm32 (int), 2 push imm32 (float)
        uint32_t vanilla;           // the immediate the site is expected to push (its bits)
    };
    struct BranchSiteSpec
    {
        const char* name;
        int index;
        const uintptr_t* function;
        uint32_t offset;
        uint8_t extra;              // bytes after a `jcc rel8` that are copied into the stub (0 for a `jcc rel32`)
    };
#include "game/gear_sites.inc"

    constexpr int kNoAnswer = static_cast<int>(0x80000000);   // the handler's "no mod answered"

    std::atomic<bool> g_levelActive{false};
    std::atomic<unsigned> g_calls{0}, g_answered{0}, g_busy{0};
    std::atomic<unsigned> g_levelSiteCalls[std::size(kSites)];

    // Called by the stubs: the level as a double and the site's index. Any thread the game calls from.
    extern "C" int __cdecl levelFactorHandler(double level, int site)
    {
        g_calls.fetch_add(1, std::memory_order_relaxed);
        if (site >= 0 && site < static_cast<int>(std::size(kSites)))
        {
            g_levelSiteCalls[site].fetch_add(1, std::memory_order_relaxed);
        }
        if (!g_levelActive.load(std::memory_order_relaxed))
        {
            return kNoAnswer;
        }
        int result = 0;
        switch (LuaHost::levelFactor(level, site, result))
        {
        case LuaHost::Answer::Value:
            g_answered.fetch_add(1, std::memory_order_relaxed);
            return result;
        case LuaHost::Answer::Busy:
            g_busy.fetch_add(1, std::memory_order_relaxed);
            return kNoAnswer;
        default:
            return kNoAnswer;
        }
    }

    // The block the stub displaces, with -1 for the bytes that differ between sites: [fild dword [esp+x]] fld st(0);
    // fmul [34.0]; fadd [8.0]; fxch st(1); fmul; fadd; fdivp st(1); call __ftol.
    std::vector<int> expectedBlock(uint8_t prefix)
    {
        std::vector<int> e;
        auto put = [&](std::initializer_list<int> b) { e.insert(e.end(), b); };
        if (prefix == 4)
        {
            put({0xDB, 0x44, 0x24, -1});                            // fild dword ptr [esp + x]
        }
        put({0xD9, 0xC0});                                          // fld st(0)
        put({0xD8, 0x0D, 0xB4, 0x10, 0x89, 0x00});                  // fmul dword ptr [0x8910B4]
        put({0xD8, 0x05, 0x18, 0x09, 0x89, 0x00});                  // fadd dword ptr [0x890918]
        put({0xD9, 0xC9});                                          // fxch st(1)
        put({0xD8, 0x0D, 0xEC, 0x09, 0x89, 0x00});                  // fmul dword ptr [0x8909EC]
        put({0xD8, 0x05, 0xB0, 0x10, 0x89, 0x00});                  // fadd dword ptr [0x8910B0]
        put({0xDE, 0xF9});                                          // fdivp st(1)
        put({0xE8, -1, -1, -1, -1});                                // call __ftol
        return e;
    }

    void emit(std::vector<uint8_t>& code, std::initializer_list<uint8_t> bytes)
    {
        code.insert(code.end(), bytes);
    }

    void emit32(std::vector<uint8_t>& code, uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
        {
            code.push_back(static_cast<uint8_t>(v >> (8 * i)));
        }
    }

    // --- constant sites: one `fmul dword [abs]` / `fld dword [abs]` that loads a pooled float -------------------------------

    // (the generated table is included once, with the level sites')

    ConstSiteSpec g_testSpecs[2] = {};      // the self test's two sites (after the generated ones)
    uintptr_t g_testBlob[2] = {};           // their code blobs
    float g_testConstant = 0.2f;            // the pooled constant they load
    std::atomic<bool> g_constantActive{false};
    std::atomic<unsigned> g_constCalls{0}, g_constAnswered{0}, g_constBusy{0};
    std::atomic<unsigned> g_constSiteCalls[std::size(kConstSites)];

    // The drop tuning (gear/drops.h) rides on three of the loot function's 0.01 constants: the chance scale (00559F08, site 0 of
    // `scale.559BF0`) and the two rarity bonus scales (0055A149, 0055A248, sites 1 and 2). A multiplier of 1 (no mod) is exactly the
    // engine's own value.
    float dropScale(int site)
    {
        static int first = -2;
        if (first == -2)
        {
            first = -1;
            for (size_t i = 0; i < std::size(kConstSites); ++i)
            {
                if (std::strcmp(kConstSites[i].name, "scale.559BF0") == 0)
                {
                    first = static_cast<int>(i);
                    break;
                }
            }
        }
        if (first < 0 || site < first || site > first + 2)
        {
            return 1.0f;
        }
        return static_cast<float>(site == first ? Drops::chanceScale() : Drops::findScale());
    }

    // Called by the stubs: the float the instruction should load, in st0 (cdecl float result). Any thread the game calls from.
    extern "C" float __cdecl constantHandler(int site)
    {
        const ConstSiteSpec& spec = site < static_cast<int>(std::size(kConstSites)) ? kConstSites[site] : g_testSpecs[site - std::size(kConstSites)];
        const float vanilla = *reinterpret_cast<const float*>(static_cast<uintptr_t>(spec.constant));
        g_constCalls.fetch_add(1, std::memory_order_relaxed);
        if (site >= 0 && site < static_cast<int>(std::size(kConstSites)))
        {
            g_constSiteCalls[site].fetch_add(1, std::memory_order_relaxed);
        }
        if (!g_constantActive.load(std::memory_order_relaxed))
        {
            return vanilla * dropScale(site);
        }
        double result = 0;
        float value = vanilla;
        switch (LuaHost::combatConstant(spec.name, vanilla, spec.index, result))
        {
        case LuaHost::Answer::Value:
            g_constAnswered.fetch_add(1, std::memory_order_relaxed);
            value = static_cast<float>(result);
            break;
        case LuaHost::Answer::Busy:
            g_constBusy.fetch_add(1, std::memory_order_relaxed);
            break;
        default:
            break;
        }
        return value * dropScale(site);
    }

    // The stub of a constant site: keeps the registers and flags the engine code may still use, gets the float from the
    // handler (it arrives in st0) and does what the displaced instruction did with it.
    uintptr_t buildConstantStub(uintptr_t site, const ConstSiteSpec& spec, int index)
    {
        std::vector<uint8_t> code;
        emit(code, {0x9C});                                         // pushfd
        emit(code, {0x50});                                         // push eax
        emit(code, {0x51});                                         // push ecx
        emit(code, {0x52});                                         // push edx
        emit(code, {0x68});                                         // push site index
        emit32(code, static_cast<uint32_t>(index));
        emit(code, {0xB8});                                         // mov eax, handler
        emit32(code, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&constantHandler)));
        emit(code, {0xFF, 0xD0});                                   // call eax: the float is in st0
        emit(code, {0x8D, 0x64, 0x24, 0x04});                       // lea esp, [esp + 4]
        emit(code, {0x5A});                                         // pop edx
        emit(code, {0x59});                                         // pop ecx
        emit(code, {0x58});                                         // pop eax
        emit(code, {0x9D});                                         // popfd
        if (spec.multiply)
        {
            emit(code, {0xDE, 0xC9});                               // fmulp st(1), st(0)
        }
        emit(code, {0xE9});                                         // jmp after the instruction
        const size_t fixup = code.size();
        emit32(code, 0);
        void* memory = VirtualAlloc(nullptr, code.size() + 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!memory)
        {
            return 0;
        }
        const uintptr_t base = reinterpret_cast<uintptr_t>(memory);
        const uint32_t v = static_cast<uint32_t>((site + 6) - (base + fixup + 4));
        std::memcpy(&code[fixup], &v, 4);
        std::memcpy(memory, code.data(), code.size());
        FlushInstructionCache(GetCurrentProcess(), memory, code.size());
        return base;
    }

    // --- creature.ability sites -----------------------------------------------------------------------------------------

    AbilitySiteSpec g_abilityTestSpecs[3] = {};     // the self test's three sites (after the generated ones)
    std::atomic<bool> g_abilityActive{false};
    std::atomic<unsigned> g_abilityCalls{0}, g_abilityAnswered{0}, g_abilityBusy{0};
    std::atomic<unsigned> g_abilitySiteCalls[std::size(kAbilitySites)];
    int g_abilityVanilla[std::size(kAbilitySites)];     // the immediate each site stores / adds (the original bytes)

    int creatureType(uintptr_t creature)
    {
        __try
        {
            return *reinterpret_cast<const int*>(creature + 0x10);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    // Called by the stubs: the ability id for the creature (kNoAnswer: the site's own value).
    extern "C" int __cdecl abilityHandler(int site, int vanilla, uintptr_t creature)
    {
        g_abilityCalls.fetch_add(1, std::memory_order_relaxed);
        if (site >= 0 && site < static_cast<int>(std::size(kAbilitySites)))
        {
            g_abilitySiteCalls[site].fetch_add(1, std::memory_order_relaxed);
        }
        const AbilitySiteSpec& spec = site < static_cast<int>(std::size(kAbilitySites)) ? kAbilitySites[site] : g_abilityTestSpecs[site - std::size(kAbilitySites)];
        const bool unknownVanilla = spec.kind == 2;
        if (!g_abilityActive.load(std::memory_order_relaxed))
        {
            return unknownVanilla ? kNoAnswer : vanilla;
        }
        int result = 0;
        const uintptr_t address = *spec.function + spec.offset;
        switch (LuaHost::creatureAbility(creatureType(creature), unknownVanilla ? -1 : vanilla, site, static_cast<int>(address), result))
        {
        case LuaHost::Answer::Value:
            g_abilityAnswered.fetch_add(1, std::memory_order_relaxed);
            return result;
        case LuaHost::Answer::Busy:
            g_abilityBusy.fetch_add(1, std::memory_order_relaxed);
            break;
        default:
            break;
        }
        return unknownVanilla ? kNoAnswer : vanilla;
    }

    // The stub of an ability site: registers and flags kept, the handler asked, the result stored the way the instruction would have.
    uintptr_t buildAbilityStub(uintptr_t site, const AbilitySiteSpec& spec, int vanilla, size_t length)
    {
        std::vector<uint8_t> code;
        const uint8_t* block = reinterpret_cast<const uint8_t*>(site);
        // the temp cell: after the code, patched in below
        const size_t tempCells = 1;
        emit(code, {0x9C, 0x50, 0x51, 0x52});                       // pushfd; push eax; push ecx; push edx
        emit(code, {0x56});                                         // push esi (the creature)
        emit(code, {0x68});                                         // push vanilla
        emit32(code, static_cast<uint32_t>(vanilla));
        emit(code, {0x68});                                         // push site index
        emit32(code, static_cast<uint32_t>(spec.index));
        emit(code, {0xB8});                                         // mov eax, handler
        emit32(code, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&abilityHandler)));
        emit(code, {0xFF, 0xD0});                                   // call eax
        emit(code, {0x83, 0xC4, 0x0C});                             // add esp, 12
        std::vector<size_t> tempFixups;
        auto tempRef = [&](std::initializer_list<uint8_t> opcode) {
            emit(code, opcode);
            tempFixups.push_back(code.size());
            emit32(code, 0);
        };
        size_t jeFixup = 0, endFixup1 = 0, endFixup2 = 0;
        if (spec.kind == 0)
        {
            emit(code, {0x89, 0x86, 0x20, 0x01, 0x00, 0x00});       // mov [esi + 0x120], eax
            emit(code, {0x5A, 0x59, 0x58, 0x9D});                   // pop edx; pop ecx; pop eax; popfd
            emit(code, {0xE9});
            endFixup1 = code.size();
            emit32(code, 0);
        }
        else if (spec.kind == 1)
        {
            tempRef({0xA3});                                        // mov [temp], eax
            emit(code, {0x5A, 0x59, 0x58, 0x9D});                   // pop edx; pop ecx; pop eax; popfd
            tempRef({0x03, 0x05});                                  // add eax, [temp]
            emit(code, {0xE9});
            endFixup1 = code.size();
            emit32(code, 0);
        }
        else
        {
            tempRef({0xA3});                                        // mov [temp], eax
            emit(code, {0x5A, 0x59, 0x58, 0x9D});                   // pop edx; pop ecx; pop eax; popfd
            tempRef({0x81, 0x3D});                                  // cmp dword [temp], kNoAnswer
            emit32(code, static_cast<uint32_t>(kNoAnswer));
            emit(code, {0x0F, 0x84});                               // je fallback
            jeFixup = code.size();
            emit32(code, 0);
            emit(code, {0x33, 0xC9});                               // xor ecx, ecx
            tempRef({0x8B, 0x15});                                  // mov edx, [temp]
            emit(code, {0xE9});
            endFixup1 = code.size();
            emit32(code, 0);
            const size_t fallback = code.size();
            code.insert(code.end(), block, block + length);        // the displaced block
            emit(code, {0xE9});
            endFixup2 = code.size();
            emit32(code, 0);
            const uint32_t rel = static_cast<uint32_t>(fallback - (jeFixup + 4));
            std::memcpy(&code[jeFixup], &rel, 4);
        }
        const size_t tempOffset = (code.size() + 3) & ~size_t(3);
        void* memory = VirtualAlloc(nullptr, tempOffset + tempCells * 4 + 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!memory)
        {
            return 0;
        }
        const uintptr_t base = reinterpret_cast<uintptr_t>(memory);
        const uintptr_t end = site + length;
        for (size_t at : tempFixups)
        {
            const uint32_t v = static_cast<uint32_t>(base + tempOffset);
            std::memcpy(&code[at], &v, 4);
        }
        auto rel = [&](size_t at, uintptr_t target) {
            const uint32_t v = static_cast<uint32_t>(target - (base + at + 4));
            std::memcpy(&code[at], &v, 4);
        };
        rel(endFixup1, end);
        if (endFixup2)
        {
            rel(endFixup2, end);
        }
        std::memcpy(memory, code.data(), code.size());
        FlushInstructionCache(GetCurrentProcess(), memory, code.size());
        return base;
    }

    // --- damage sites ---------------------------------------------------------------------------------------------------

    std::atomic<bool> g_damageActive{false};
    std::atomic<unsigned> g_damageCalls{0}, g_damageAnswered{0}, g_damageBusy{0};
    std::atomic<unsigned> g_damageSiteCalls[std::size(kDamageSites)];

    // The creature type of a creature (a pointer, 0 for none) read safely: +0x10.
    int typeOfCreature(uintptr_t creature)
    {
        return creature ? creatureType(creature) : -1;
    }

    // The victim is a creature's combat object: [victim + 4] is the creature (the stub passes edi), the attacker is the creature (ebp).
    int typeOfVictim(uintptr_t victim)
    {
        __try
        {
            return victim ? creatureType(*reinterpret_cast<const uintptr_t*>(victim + 4)) : -1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    // The object id of a creature pointer (+0x0C), 0 for none.
    int idOfCreature(uintptr_t creature)
    {
        __try
        {
            return creature ? *reinterpret_cast<const int*>(creature + 0x0C) : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    int idOfVictim(uintptr_t victim)
    {
        __try
        {
            return victim ? idOfCreature(*reinterpret_cast<const uintptr_t*>(victim + 4)) : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    extern "C" int __cdecl damageHandler(int site, int damage, uintptr_t victim, uintptr_t attacker)
    {
        g_damageCalls.fetch_add(1, std::memory_order_relaxed);
        if (site >= 0 && site < static_cast<int>(std::size(kDamageSites)))
        {
            g_damageSiteCalls[site].fetch_add(1, std::memory_order_relaxed);
        }
        // never above the 16 bit maximum, whoever computed it (no wrap around, the value saturates)
        constexpr int kMaxDamage = 65534;
        if (!g_damageActive.load(std::memory_order_relaxed))
        {
            return damage > kMaxDamage ? kMaxDamage : damage;
        }
        int result = 0;
        switch (LuaHost::damage(damage, typeOfVictim(victim), typeOfCreature(attacker), site, idOfVictim(victim), idOfCreature(attacker), result))
        {
        case LuaHost::Answer::Value:
            g_damageAnswered.fetch_add(1, std::memory_order_relaxed);
            return std::clamp(result, 0, kMaxDamage);
        case LuaHost::Answer::Busy:
            g_damageBusy.fetch_add(1, std::memory_order_relaxed);
            break;
        default:
            break;
        }
        return damage > kMaxDamage ? kMaxDamage : damage;
    }

    // The stub of a damage site: the displaced `call __ftol` runs (the damage is in eax afterwards), the handler may replace it.
    uintptr_t buildDamageStub(uintptr_t site, uintptr_t ftol, int index)
    {
        std::vector<uint8_t> code;
        const uintptr_t base0 = 0;
        (void)base0;
        void* memory = VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!memory)
        {
            return 0;
        }
        const uintptr_t base = reinterpret_cast<uintptr_t>(memory);
        emit(code, {0xE8});                                         // call __ftol
        emit32(code, static_cast<uint32_t>(ftol - (base + code.size() + 4)));
        emit(code, {0x51, 0x52});                                   // push ecx; push edx
        emit(code, {0x55, 0x57, 0x50});                             // push ebp (attacker); push edi (victim); push eax (damage)
        emit(code, {0x68});                                         // push site index
        emit32(code, static_cast<uint32_t>(index));
        emit(code, {0xB9});                                         // mov ecx, handler
        emit32(code, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&damageHandler)));
        emit(code, {0xFF, 0xD1});                                   // call ecx
        emit(code, {0x83, 0xC4, 0x10});                             // add esp, 16
        emit(code, {0x5A, 0x59});                                   // pop edx; pop ecx
        emit(code, {0xE9});                                         // jmp after the call
        emit32(code, static_cast<uint32_t>((site + 5) - (base + code.size() + 4)));
        std::memcpy(memory, code.data(), code.size());
        FlushInstructionCache(GetCurrentProcess(), memory, code.size());
        return base;
    }

    // --- effect.add sites -----------------------------------------------------------------------------------------------

    std::atomic<bool> g_effectActive{false};
    std::atomic<unsigned> g_effectCalls{0}, g_effectAnswered{0}, g_effectBusy{0};
    std::atomic<unsigned> g_effectSiteCalls[std::size(kEffectSites)];

    bool readMem(uintptr_t at, void* out, size_t n)
    {
        __try
        {
            std::memcpy(out, reinterpret_cast<const void*>(at), n);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool writeMem(uintptr_t at, const void* in, size_t n)
    {
        __try
        {
            std::memcpy(reinterpret_cast<void*>(at), in, n);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The engine's "value below this becomes 0" constant of the effect function (ENG 00890668).
    float effectFloor()
    {
        float v = 0;
        return readMem(0x890668, &v, sizeof(v)) ? v : 0.0f;
    }

    // Called by the stubs with `list` = the effect list object (esi: its creature's fields are at fixed offsets below it), `entry` the
    // entry the loop is at, `sum` the new value (old + added), `caller` the return address of the function. 0: the engine's own store,
    // 1: the mod handled it, continue with the next entry, 2: the mod handled it, leave the function.
    extern "C" int __cdecl effectHandler(int site, uintptr_t list, uintptr_t entry, const double* sum, uintptr_t caller)
    {
        g_effectCalls.fetch_add(1, std::memory_order_relaxed);
        if (site >= 0 && site < static_cast<int>(std::size(kEffectSites)))
        {
            g_effectSiteCalls[site].fetch_add(1, std::memory_order_relaxed);
        }
        if (!g_effectActive.load(std::memory_order_relaxed) || (caller >= kEffectAiLow && caller <= kEffectAiHigh))
        {
            return 0;
        }
        LuaHost::EffectInfo info;
        int32_t id = 0, counter = 0, flags = 0;
        uint16_t ability = 0, effect = 0;
        float maxValue = 0, current = 0;
        readMem(list - 0x39C, &id, 4);
        readMem(list - 0x288, &ability, 2);
        readMem(list + 0x148, &flags, 4);
        readMem(entry + 4, &effect, 2);
        readMem(entry + 0xA, &maxValue, 4);
        readMem(entry + 0x12, &current, 4);
        readMem(list - 0x214, &counter, 4);
        info.creature = id;
        info.ability = ability;
        info.effect = effect;
        info.flags = flags;
        info.value = *sum;
        info.max = maxValue;
        info.current = current;
        info.counter = counter;
        LuaHost::EffectAnswer answer;
        switch (LuaHost::effectAdd(info, answer))
        {
        case LuaHost::Answer::Value:
            break;
        case LuaHost::Answer::Busy:
            g_effectBusy.fetch_add(1, std::memory_order_relaxed);
            return 0;
        default:
            return 0;
        }
        g_effectAnswered.fetch_add(1, std::memory_order_relaxed);
        // the entry to change: the loop's, or the next one (from there on) with the effect id the mod named
        uintptr_t target = entry;
        if (answer.effect >= 0)
        {
            uintptr_t end = 0;
            int32_t endValue = 0;
            if (readMem(list + 0x106, &endValue, 4))
            {
                end = static_cast<uintptr_t>(endValue);
            }
            for (uintptr_t at = entry; at + 0x16 <= end; at += 0x16)
            {
                uint16_t id16 = 0;
                if (readMem(at + 4, &id16, 2) && id16 == answer.effect)
                {
                    target = at;
                    break;
                }
            }
        }
        float maxOfTarget = 0;
        readMem(target + 0xA, &maxOfTarget, 4);
        if (answer.hasValue)
        {
            const float v = static_cast<float>(answer.value);
            writeMem(target + 0x12, &v, 4);
        }
        if (answer.clamp)
        {
            // the engine's clamps: a sum above the maximum becomes the maximum, a value below its floor becomes 0
            if (!answer.hasValue && static_cast<float>(*sum) > maxOfTarget)
            {
                writeMem(target + 0x12, &maxOfTarget, 4);
            }
            else if (answer.hasValue)
            {
                float v = 0;
                readMem(target + 0x12, &v, 4);
                if (v > maxOfTarget)
                {
                    writeMem(target + 0x12, &maxOfTarget, 4);
                }
            }
            float now = 0;
            readMem(target + 0x12, &now, 4);
            if (now < effectFloor())
            {
                const float zero = 0;
                writeMem(target + 0x12, &zero, 4);
            }
        }
        if (answer.stack == 1 || answer.stack == 2)
        {
            int32_t c = 1;
            if (answer.stack == 2)
            {
                readMem(list - 0x214, &c, 4);
                ++c;
            }
            writeMem(list - 0x214, &c, 4);
        }
        return answer.finish ? 2 : 1;
    }

    // The stub: the sum (st0) is kept as an 80 bit value and as a double, the handler decides; 0 runs the displaced fstp / fld (the 80 bit
    // sum, so the engine's own result is bit for bit the same) and continues at the clamp, 1 jumps to the loop's next entry, 2 leaves.
    uintptr_t buildEffectStub(uintptr_t site, int index, uintptr_t nextEntry, uintptr_t leave)
    {
        std::vector<uint8_t> code;
        void* memory = VirtualAlloc(nullptr, 160, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!memory)
        {
            return 0;
        }
        const uintptr_t base = reinterpret_cast<uintptr_t>(memory);
        auto jmpTo = [&](uintptr_t target, std::initializer_list<uint8_t> opcode) {
            emit(code, opcode);
            emit32(code, static_cast<uint32_t>(target - (base + code.size() + 4)));
        };
        emit(code, {0x83, 0xEC, 0x18});                             // sub esp, 24      [0..9] tbyte sum, [12..19] double sum, [20] ecx
        emit(code, {0x89, 0x4C, 0x24, 0x14});                       // mov [esp+20], ecx
        emit(code, {0xD9, 0xC0});                                   // fld st0
        emit(code, {0xDD, 0x5C, 0x24, 0x0C});                       // fstp qword [esp+12]
        emit(code, {0xDB, 0x3C, 0x24});                             // fstp tbyte [esp]
        emit(code, {0x8B, 0x54, 0x24, 0x40});                       // mov edx, [esp+0x40]   (the function's return address)
        emit(code, {0x8D, 0x44, 0x24, 0x0C});                       // lea eax, [esp+12]
        emit(code, {0x52, 0x50, 0x51, 0x56});                       // push edx; push eax; push ecx; push esi
        emit(code, {0x68});                                         // push site index
        emit32(code, static_cast<uint32_t>(index));
        emit(code, {0xB8});                                         // mov eax, handler
        emit32(code, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&effectHandler)));
        emit(code, {0xFF, 0xD0});                                   // call eax
        emit(code, {0x83, 0xC4, 0x14});                             // add esp, 20
        emit(code, {0x8B, 0x4C, 0x24, 0x14});                       // mov ecx, [esp+20]
        emit(code, {0x83, 0xF8, 0x01});                             // cmp eax, 1
        emit(code, {0x74});                                         // je next
        const size_t jeNext = code.size();
        emit(code, {0x00});
        emit(code, {0x83, 0xF8, 0x02});                             // cmp eax, 2
        emit(code, {0x74});                                         // je leave
        const size_t jeLeave = code.size();
        emit(code, {0x00});
        // engine path
        emit(code, {0xDB, 0x2C, 0x24});                             // fld tbyte [esp]
        emit(code, {0x83, 0xC4, 0x18});                             // add esp, 24
        emit(code, {0xD9, 0x59, 0x12});                             // fstp dword [ecx+0x12]
        emit(code, {0xD9, 0x41, 0x12});                             // fld dword [ecx+0x12]
        jmpTo(site + 6, {0xE9});
        code[jeNext] = static_cast<uint8_t>(code.size() - (jeNext + 1));
        emit(code, {0x83, 0xC4, 0x18});                             // next: add esp, 24
        jmpTo(nextEntry, {0xE9});
        code[jeLeave] = static_cast<uint8_t>(code.size() - (jeLeave + 1));
        emit(code, {0x83, 0xC4, 0x18});                             // leave: add esp, 24
        jmpTo(leave, {0xE9});
        std::memcpy(memory, code.data(), code.size());
        FlushInstructionCache(GetCurrentProcess(), memory, code.size());
        return base;
    }

    // --- bonus.constant sites -------------------------------------------------------------------------------------------

    PushSiteSpec g_bonusTestSpecs[2] = {};      // the self test's two sites (an int8 and a float push) after the generated ones
    std::atomic<bool> g_bonusActive{false};
    std::atomic<unsigned> g_bonusCalls{0}, g_bonusAnswered{0}, g_bonusBusy{0};
    std::atomic<unsigned> g_bonusSiteCalls[std::size(kBonusSites)];

    // The value the pushed argument takes: the bits of an int or a float (the stub pushes them).
    extern "C" uint32_t __cdecl bonusHandler(int site, uint32_t vanillaBits)
    {
        g_bonusCalls.fetch_add(1, std::memory_order_relaxed);
        if (site >= 0 && site < static_cast<int>(std::size(kBonusSites)))
        {
            g_bonusSiteCalls[site].fetch_add(1, std::memory_order_relaxed);
        }
        if (!g_bonusActive.load(std::memory_order_relaxed) || site < 0 || site >= static_cast<int>(std::size(kBonusSites) + 2))
        {
            return vanillaBits;
        }
        const bool test = site >= static_cast<int>(std::size(kBonusSites));
        const PushSiteSpec& spec = test ? g_bonusTestSpecs[site - std::size(kBonusSites)] : kBonusSites[site];
        double vanilla;
        if (spec.kind == 2)
        {
            float f;
            std::memcpy(&f, &vanillaBits, 4);
            vanilla = f;
        }
        else
        {
            vanilla = static_cast<int32_t>(vanillaBits);
        }
        double result = 0;
        switch (LuaHost::bonusConstant(site, vanilla, test ? 0 : static_cast<int>(*spec.function + spec.offset), result))
        {
        case LuaHost::Answer::Value:
            g_bonusAnswered.fetch_add(1, std::memory_order_relaxed);
            if (spec.kind == 2)
            {
                const float f = static_cast<float>(result);
                uint32_t bits;
                std::memcpy(&bits, &f, 4);
                return bits;
            }
            return static_cast<uint32_t>(static_cast<int32_t>(std::lround(result)));
        case LuaHost::Answer::Busy:
            g_bonusBusy.fetch_add(1, std::memory_order_relaxed);
            break;
        default:
            break;
        }
        return vanillaBits;
    }

    // The stub of a push site: registers and flags kept, the handler gives the bits that are pushed instead of the immediate.
    uintptr_t buildBonusStub(uintptr_t site, int index, uint32_t vanillaBits, size_t length, uintptr_t callTarget)
    {
        std::vector<uint8_t> code;
        void* memory = VirtualAlloc(nullptr, 96, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!memory)
        {
            return 0;
        }
        const uintptr_t base = reinterpret_cast<uintptr_t>(memory);
        const uintptr_t temp = base + 80;
        emit(code, {0x9C, 0x50, 0x51, 0x52});                       // pushfd; push eax; push ecx; push edx
        emit(code, {0x68});                                         // push vanilla bits
        emit32(code, vanillaBits);
        emit(code, {0x68});                                         // push site index
        emit32(code, static_cast<uint32_t>(index));
        emit(code, {0xB8});                                         // mov eax, handler
        emit32(code, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&bonusHandler)));
        emit(code, {0xFF, 0xD0});                                   // call eax
        emit(code, {0x83, 0xC4, 0x08});                             // add esp, 8
        emit(code, {0xA3});                                         // mov [temp], eax
        emit32(code, static_cast<uint32_t>(temp));
        emit(code, {0x5A, 0x59, 0x58, 0x9D});                       // pop edx; pop ecx; pop eax; popfd
        emit(code, {0xFF, 0x35});                                   // push dword [temp]
        emit32(code, static_cast<uint32_t>(temp));
        if (callTarget)
        {
            // the displaced `call rel32` that follows a push imm8
            emit(code, {0xE8});
            emit32(code, static_cast<uint32_t>(callTarget - (base + code.size() + 4)));
        }
        emit(code, {0xE9});                                         // jmp after the displaced instructions
        emit32(code, static_cast<uint32_t>((site + length) - (base + code.size() + 4)));
        std::memcpy(memory, code.data(), code.size());
        FlushInstructionCache(GetCurrentProcess(), memory, code.size());
        return base;
    }

    // --- skillslot.select sites -----------------------------------------------------------------------------------------

    std::atomic<bool> g_slotActive{false};
    std::atomic<unsigned> g_slotCalls{0}, g_slotAnswered{0}, g_slotBusy{0};
    std::atomic<unsigned> g_slotSiteCalls[std::size(kSlotSites)];

    extern "C" void __cdecl slotHandler(int site)
    {
        g_slotCalls.fetch_add(1, std::memory_order_relaxed);
        if (site >= 0 && site < static_cast<int>(std::size(kSlotSites)))
        {
            g_slotSiteCalls[site].fetch_add(1, std::memory_order_relaxed);
        }
        if (!g_slotActive.load(std::memory_order_relaxed))
        {
            return;
        }
        switch (LuaHost::skillSlot(site))
        {
        case LuaHost::Answer::Value:
            g_slotAnswered.fetch_add(1, std::memory_order_relaxed);
            break;
        case LuaHost::Answer::Busy:
            g_slotBusy.fetch_add(1, std::memory_order_relaxed);
            break;
        default:
            break;
        }
    }

    // The stub: the displaced instruction (copied, or the call re-made), then the registers and flags kept around the handler.
    uintptr_t buildSlotStub(uintptr_t site, const AbilitySiteSpec& spec)
    {
        std::vector<uint8_t> code;
        void* memory = VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!memory)
        {
            return 0;
        }
        const uintptr_t base = reinterpret_cast<uintptr_t>(memory);
        const uint8_t* b = reinterpret_cast<const uint8_t*>(site);
        if (spec.kind == 0)
        {
            code.insert(code.end(), b, b + 5);
        }
        else
        {
            int32_t rel = 0;
            std::memcpy(&rel, b + 1, 4);
            emit(code, {0xE8});
            emit32(code, static_cast<uint32_t>((site + 5 + rel) - (base + code.size() + 4)));
        }
        emit(code, {0x9C, 0x60});                                   // pushfd; pushad
        emit(code, {0x68});                                         // push site index
        emit32(code, static_cast<uint32_t>(spec.index));
        emit(code, {0xB8});                                         // mov eax, handler
        emit32(code, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&slotHandler)));
        emit(code, {0xFF, 0xD0});                                   // call eax
        emit(code, {0x83, 0xC4, 0x04});                             // add esp, 4
        emit(code, {0x61, 0x9D});                                   // popad; popfd
        emit(code, {0xE9});
        emit32(code, static_cast<uint32_t>((site + 5) - (base + code.size() + 4)));
        std::memcpy(memory, code.data(), code.size());
        FlushInstructionCache(GetCurrentProcess(), memory, code.size());
        return base;
    }

    // --- combat.branch sites --------------------------------------------------------------------------------------------

    std::atomic<bool> g_branchActive{false};
    std::atomic<unsigned> g_branchCalls{0}, g_branchAnswered{0}, g_branchBusy{0};
    std::atomic<unsigned> g_branchSiteCalls[std::size(kBranchSites)];
    BranchSiteSpec g_branchTestSpecs[2] = {};    // the self test's sites (after the generated ones)

    // Where the stub goes on: `target` when the branch is taken, else `fall`. `vanilla` says whether the engine's condition took it.
    extern "C" uintptr_t __cdecl branchHandler(int site, int vanilla, uint32_t eax, uintptr_t target, uintptr_t fall)
    {
        g_branchCalls.fetch_add(1, std::memory_order_relaxed);
        if (site >= 0 && site < static_cast<int>(std::size(kBranchSites)))
        {
            g_branchSiteCalls[site].fetch_add(1, std::memory_order_relaxed);
        }
        if (!g_branchActive.load(std::memory_order_relaxed))
        {
            return vanilla ? target : fall;
        }
        const BranchSiteSpec& spec = site < static_cast<int>(std::size(kBranchSites)) ? kBranchSites[site] : g_branchTestSpecs[site - std::size(kBranchSites)];
        bool take = false;
        switch (LuaHost::branch(spec.name, spec.index, vanilla != 0, static_cast<int>(eax), take))
        {
        case LuaHost::Answer::Value:
            g_branchAnswered.fetch_add(1, std::memory_order_relaxed);
            return take ? target : fall;
        case LuaHost::Answer::Busy:
            g_branchBusy.fetch_add(1, std::memory_order_relaxed);
            break;
        default:
            break;
        }
        return vanilla ? target : fall;
    }

    // The stub: the displaced `jcc rel32` is evaluated on the real flags (it only records whether it would jump), the handler decides, and
    // the stub jumps to the target or the next instruction with the registers and flags exactly as they were.
    uintptr_t buildBranchStub(uintptr_t site, int index, uint8_t extra)
    {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(site);
        const bool wide = b[0] == 0x0F;
        const size_t length = (wide ? 6 : 2) + extra;
        uintptr_t target;
        if (wide)
        {
            int32_t rel = 0;
            std::memcpy(&rel, b + 2, 4);
            target = site + 6 + rel;
        }
        else
        {
            target = site + 2 + static_cast<int8_t>(b[1]);
        }
        std::vector<uint8_t> code;
        void* memory = VirtualAlloc(nullptr, 192, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!memory)
        {
            return 0;
        }
        const uintptr_t base = reinterpret_cast<uintptr_t>(memory);
        const uintptr_t vanillaCell = base + 160, destCell = base + 164;
        emit(code, {0x0F, static_cast<uint8_t>(wide ? b[1] : b[0] + 0x10)});   // the original jcc (as rel32), to `taken`
        const size_t jccRel = code.size();
        emit32(code, 0);
        emit(code, {0xC7, 0x05});                                   // mov dword [vanilla], 0
        emit32(code, static_cast<uint32_t>(vanillaCell));
        emit32(code, 0);
        emit(code, {0xEB, 0x0A});                                   // jmp ask (over the next 10 bytes)
        const size_t taken = code.size();
        emit(code, {0xC7, 0x05});                                   // taken: mov dword [vanilla], 1
        emit32(code, static_cast<uint32_t>(vanillaCell));
        emit32(code, 1);
        const uint32_t jr = static_cast<uint32_t>(taken - (jccRel + 4));
        std::memcpy(&code[jccRel], &jr, 4);
        emit(code, {0x9C, 0x60});                                   // ask: pushfd; pushad
        emit(code, {0x68});
        const size_t fallPatch = code.size();
        emit32(code, 0);                                            // push fall (patched below)
        emit(code, {0x68});
        emit32(code, static_cast<uint32_t>(target));                // push target
        emit(code, {0xFF, 0x74, 0x24, 0x24});                       // push dword [esp + 0x24]: eax of the pushad block (two pushes since)
        emit(code, {0xFF, 0x35});                                   // push dword [vanilla]
        emit32(code, static_cast<uint32_t>(vanillaCell));
        emit(code, {0x68});
        emit32(code, static_cast<uint32_t>(index));                 // push site index
        emit(code, {0xB8});
        emit32(code, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&branchHandler)));
        emit(code, {0xFF, 0xD0});                                   // call eax
        emit(code, {0x83, 0xC4, 0x14});                             // add esp, 20
        emit(code, {0xA3});                                         // mov [dest], eax
        emit32(code, static_cast<uint32_t>(destCell));
        emit(code, {0x61, 0x9D});                                   // popad; popfd
        emit(code, {0xFF, 0x25});                                   // jmp dword [dest]
        emit32(code, static_cast<uint32_t>(destCell));
        uintptr_t fall = site + length;
        if (extra)
        {
            // the fall-through path: the copied instruction(s), then back
            fall = base + code.size();
            code.insert(code.end(), b + 2, b + 2 + extra);
            emit(code, {0xE9});
            emit32(code, static_cast<uint32_t>((site + length) - (base + code.size() + 4)));
        }
        const uint32_t fallValue = static_cast<uint32_t>(fall);
        std::memcpy(&code[fallPatch], &fallValue, 4);
        std::memcpy(memory, code.data(), code.size());
        FlushInstructionCache(GetCurrentProcess(), memory, code.size());
        return base;
    }

    // --- skillslot.key: the routing of a character key in the skill slot window --------------------------------------------

    std::atomic<bool> g_keyActive{false};
    std::atomic<unsigned> g_keyCalls{0}, g_keyAnswered{0}, g_keyBusy{0};

    // Returns where the stub goes on: `orig` (the engine's own jb) without an answer, else the block the mod chose.
    extern "C" uintptr_t __cdecl keyHandler(uint32_t ax, uintptr_t orig)
    {
        g_keyCalls.fetch_add(1, std::memory_order_relaxed);
        if (!g_keyActive.load(std::memory_order_relaxed) || !Gear::Addr::skillSlotFn)
        {
            return orig;
        }
        int slot = -1, mode = 0;
        switch (LuaHost::skillSlotKey(static_cast<int>(ax & 0xFFFF), slot, mode))
        {
        case LuaHost::Answer::Value:
            g_keyAnswered.fetch_add(1, std::memory_order_relaxed);
            if (slot < 0 || slot > 4)
            {
                return Gear::Addr::skillSlotFn + kKeyEnd;
            }
            return Gear::Addr::skillSlotFn + (mode == 1 ? kKeySelect[slot] : kKeyFull[slot]);
        case LuaHost::Answer::Busy:
            g_keyBusy.fetch_add(1, std::memory_order_relaxed);
            break;
        default:
            break;
        }
        return orig;
    }

    uintptr_t buildKeyStub(uintptr_t site)
    {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(site);
        int32_t rel = 0;
        std::memcpy(&rel, b + 2, 4);
        const uintptr_t target = site + 6 + rel;
        std::vector<uint8_t> code;
        void* memory = VirtualAlloc(nullptr, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!memory)
        {
            return 0;
        }
        const uintptr_t base = reinterpret_cast<uintptr_t>(memory);
        const uintptr_t destCell = base + 120;
        emit(code, {0x9C, 0x60});                                   // pushfd; pushad
        emit(code, {0x68});
        const size_t origPatch = code.size();
        emit32(code, 0);                                            // push orig (patched below)
        emit(code, {0x0F, 0xB7, 0x44, 0x24, 0x20});                 // movzx eax, word [esp + 0x20]   (ax of the pushad block, one push since)
        emit(code, {0x50});                                         // push eax
        emit(code, {0xB8});
        emit32(code, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&keyHandler)));
        emit(code, {0xFF, 0xD0});                                   // call eax
        emit(code, {0x83, 0xC4, 0x08});                             // add esp, 8
        emit(code, {0xA3});                                         // mov [dest], eax
        emit32(code, static_cast<uint32_t>(destCell));
        emit(code, {0x61, 0x9D});                                   // popad; popfd
        emit(code, {0xFF, 0x25});                                   // jmp dword [dest]
        emit32(code, static_cast<uint32_t>(destCell));
        const uintptr_t orig = base + code.size();                  // the engine's own `jb`, then the instruction after it
        emit(code, {0x0F, 0x82});
        emit32(code, static_cast<uint32_t>(target - (base + code.size() + 4)));
        emit(code, {0xE9});
        emit32(code, static_cast<uint32_t>((site + 6) - (base + code.size() + 4)));
        const uint32_t origValue = static_cast<uint32_t>(orig);
        std::memcpy(&code[origPatch], &origValue, 4);
        std::memcpy(memory, code.data(), code.size());
        FlushInstructionCache(GetCurrentProcess(), memory, code.size());
        return base;
    }

    // The stub for one site (see the header). Returns its address, 0 if no memory could be had.
    uintptr_t buildStub(uintptr_t site, const SiteSpec& spec, int index)
    {
        const uint8_t* block = reinterpret_cast<const uint8_t*>(site);
        std::vector<uint8_t> code;
        // the block's first instruction loads the level
        if (spec.prefix)
        {
            code.insert(code.end(), block, block + spec.prefix);
        }
        emit(code, {0x83, 0xEC, 0x08});                             // sub esp, 8
        emit(code, {0xDD, 0x1C, 0x24});                             // fstp qword ptr [esp]
        emit(code, {0x51});                                         // push ecx
        emit(code, {0x52});                                         // push edx
        emit(code, {0x68});                                         // push site index
        emit32(code, static_cast<uint32_t>(index));
        emit(code, {0xFF, 0x74, 0x24, 0x10});                       // push [esp + 0x10]: the double's high half
        emit(code, {0xFF, 0x74, 0x24, 0x10});                       // push [esp + 0x10]: and its low half
        emit(code, {0xB8});                                         // mov eax, handler
        emit32(code, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&levelFactorHandler)));
        emit(code, {0xFF, 0xD0});                                   // call eax
        emit(code, {0x83, 0xC4, 0x0C});                             // add esp, 12
        emit(code, {0x5A});                                         // pop edx
        emit(code, {0x59});                                         // pop ecx
        emit(code, {0x3D});                                         // cmp eax, kNoAnswer
        emit32(code, static_cast<uint32_t>(kNoAnswer));
        emit(code, {0x0F, 0x84});                                   // je fallback
        const size_t fallbackFixup = code.size();
        emit32(code, 0);
        emit(code, {0x83, 0xC4, 0x08});                             // add esp, 8
        emit(code, {0x99});                                         // cdq (the original leaves edx:eax from __ftol)
        emit(code, {0xE9});                                         // jmp end
        const size_t endFixup1 = code.size();
        emit32(code, 0);
        const size_t fallback = code.size();
        emit(code, {0xDD, 0x04, 0x24});                             // fld qword ptr [esp]
        emit(code, {0x83, 0xC4, 0x08});                             // add esp, 8
        const size_t rest = code.size();
        code.insert(code.end(), block + spec.prefix, block + spec.length);   // the displaced instructions
        const size_t callRel = rest + (spec.length - spec.prefix) - 4;       // the call's rel32 in the copy
        emit(code, {0xE9});                                         // jmp end
        const size_t endFixup2 = code.size();
        emit32(code, 0);

        void* memory = VirtualAlloc(nullptr, code.size() + 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!memory)
        {
            return 0;
        }
        const uintptr_t base = reinterpret_cast<uintptr_t>(memory);
        const uintptr_t end = site + spec.length;
        auto rel = [&](size_t at, uintptr_t target) {
            const uint32_t v = static_cast<uint32_t>(target - (base + at + 4));
            std::memcpy(&code[at], &v, 4);
        };
        rel(fallbackFixup, base + fallback);
        rel(endFixup1, end);
        rel(endFixup2, end);
        // the copied `call __ftol` keeps its target
        int32_t original;
        std::memcpy(&original, block + spec.length - 4, 4);
        rel(callRel, end + original);
        std::memcpy(memory, code.data(), code.size());
        FlushInstructionCache(GetCurrentProcess(), memory, code.size());
        return base;
    }
}

size_t SiteHook::install()
{
    size_t installed = 0;
    int index = 0;
    for (const SiteSpec& spec : kSites)
    {
        const int site = index++;
        if (!*spec.function)
        {
            LOG("Site hook {}: its function was not found: not installed", spec.hook);
            continue;
        }
        const uintptr_t address = *spec.function + spec.offset;
        const std::vector<int> expect = expectedBlock(spec.prefix);
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(address);
        bool ok = expect.size() == spec.length;
        for (size_t i = 0; ok && i < expect.size(); ++i)
        {
            ok = expect[i] < 0 || bytes[i] == expect[i];
        }
        if (!ok)
        {
            LOG("Site hook {} at {:08x}: the code is not the expected block: not installed", spec.hook, address);
            continue;
        }
        const uintptr_t stub = buildStub(address, spec, site);
        if (!stub)
        {
            LOG("Site hook {} at {:08x}: no memory for the stub", spec.hook, address);
            continue;
        }
        std::vector<uint8_t> patch(spec.length, 0x90);
        patch[0] = 0xE9;
        const uint32_t rel = static_cast<uint32_t>(stub - (address + 5));
        std::memcpy(&patch[1], &rel, 4);
        if (!Patch::write(address, patch.data(), patch.size()))
        {
            LOG("Site hook {} at {:08x}: the write failed", spec.hook, address);
            continue;
        }
        ++installed;
    }
    size_t constants = 0;
    for (int i = 0; i < static_cast<int>(std::size(kConstSites)); ++i)
    {
        const ConstSiteSpec& spec = kConstSites[i];
        if (!*spec.function)
        {
            LOG("Site hook {}[{}]: its function was not found: not installed", spec.name, spec.index);
            continue;
        }
        const uintptr_t address = *spec.function + spec.offset;
        const uint8_t* b = reinterpret_cast<const uint8_t*>(address);
        uint32_t operand;
        std::memcpy(&operand, b + 2, 4);
        const bool ok = b[0] == (spec.multiply ? 0xD8 : 0xD9) && b[1] == (spec.multiply ? 0x0D : 0x05) && operand == spec.constant;
        if (!ok)
        {
            LOG("Site hook {}[{}] at {:08x}: the code is not the expected load: not installed", spec.name, spec.index, address);
            continue;
        }
        const uintptr_t stub = buildConstantStub(address, spec, i);
        if (!stub)
        {
            continue;
        }
        uint8_t bytes[6] = {0xE9, 0, 0, 0, 0, 0x90};
        const uint32_t rel = static_cast<uint32_t>(stub - (address + 5));
        std::memcpy(bytes + 1, &rel, 4);
        if (Patch::write(address, bytes, 6))
        {
            ++constants;
        }
    }
    size_t abilities = 0;
    for (const AbilitySiteSpec& spec : kAbilitySites)
    {
        if (!*spec.function)
        {
            continue;
        }
        const uintptr_t address = *spec.function + spec.offset;
        const uint8_t* b = reinterpret_cast<const uint8_t*>(address);
        size_t length = 0;
        int vanilla = 0;
        bool ok = false;
        if (spec.kind == 0)
        {
            length = 10;
            ok = b[0] == 0xC7 && b[1] == 0x86 && b[2] == 0x20 && b[3] == 0x01 && b[4] == 0 && b[5] == 0;
            std::memcpy(&vanilla, b + 6, 4);
        }
        else if (spec.kind == 1)
        {
            length = 5;
            ok = b[0] == 0x05;
            std::memcpy(&vanilla, b + 1, 4);
        }
        else
        {
            static const uint8_t expect[9] = {0xF7, 0xDA, 0x1B, 0xD2, 0x33, 0xC9, 0x83, 0xC2, 0x32};
            length = 9;
            ok = std::memcmp(b, expect, 9) == 0;
        }
        if (!ok)
        {
            LOG("Site hook creature.ability[{}] at {:08x}: the code is not the expected instruction: not installed", spec.index, address);
            continue;
        }
        g_abilityVanilla[spec.index] = vanilla;
        const uintptr_t stub = buildAbilityStub(address, spec, vanilla, length);
        if (!stub)
        {
            continue;
        }
        std::vector<uint8_t> bytes(length, 0x90);
        bytes[0] = 0xE9;
        const uint32_t rel = static_cast<uint32_t>(stub - (address + 5));
        std::memcpy(&bytes[1], &rel, 4);
        if (Patch::write(address, bytes.data(), bytes.size()))
        {
            ++abilities;
        }
    }
    size_t damages = 0;
    for (const DamageSiteSpec& spec : kDamageSites)
    {
        if (!*spec.function)
        {
            continue;
        }
        const uintptr_t address = *spec.function + spec.offset;
        const uint8_t* b = reinterpret_cast<const uint8_t*>(address);
        int32_t rel = 0;
        std::memcpy(&rel, b + 1, 4);
        const uintptr_t ftol = address + 5 + rel;
        // `call __ftol; mov esi, eax`
        if (b[0] != 0xE8 || b[5] != 0x8B || b[6] != 0xF0)
        {
            LOG("Site hook damage[{}] at {:08x}: the code is not the expected call: not installed", spec.index, address);
            continue;
        }
        const uintptr_t stub = buildDamageStub(address, ftol, spec.index);
        if (!stub)
        {
            continue;
        }
        uint8_t bytes[5] = {0xE9, 0, 0, 0, 0};
        const uint32_t jump = static_cast<uint32_t>(stub - (address + 5));
        std::memcpy(bytes + 1, &jump, 4);
        if (Patch::write(address, bytes, sizeof(bytes)))
        {
            ++damages;
        }
    }
    size_t effects = 0;
    for (const DamageSiteSpec& spec : kEffectSites)
    {
        if (!*spec.function)
        {
            continue;
        }
        const uintptr_t address = *spec.function + spec.offset;
        const uint8_t* b = reinterpret_cast<const uint8_t*>(address);
        static const uint8_t expect[6] = {0xD9, 0x59, 0x12, 0xD9, 0x41, 0x12};
        if (std::memcmp(b, expect, sizeof(expect)) != 0)
        {
            LOG("Site hook effect.add[{}] at {:08x}: the code is not the expected fstp / fld: not installed", spec.index, address);
            continue;
        }
        // the loop's next entry (after the clamps: the code that recomputes the end of the list) and the function's epilogue (pop edi ...)
        const uintptr_t next = *spec.function + 0x12A;      // ENG 0056259A
        const uintptr_t leave = *spec.function + 0x155;     // ENG 005625C5
        const uint8_t* nb = reinterpret_cast<const uint8_t*>(next);
        const uint8_t* lb = reinterpret_cast<const uint8_t*>(leave);
        if (nb[0] != 0x8B || nb[1] != 0x8E || lb[0] != 0x5F || lb[1] != 0x5E)
        {
            LOG("Site hook effect.add[{}]: the loop's next entry / epilogue are not where expected: not installed", spec.index);
            continue;
        }
        const uintptr_t stub = buildEffectStub(address, spec.index, next, leave);
        if (!stub)
        {
            continue;
        }
        uint8_t bytes[6] = {0xE9, 0, 0, 0, 0, 0x90};
        const uint32_t jump = static_cast<uint32_t>(stub - (address + 5));
        std::memcpy(bytes + 1, &jump, 4);
        if (Patch::write(address, bytes, sizeof(bytes)))
        {
            ++effects;
        }
    }
    size_t bonuses = 0;
    for (const PushSiteSpec& spec : kBonusSites)
    {
        if (!*spec.function)
        {
            continue;
        }
        const uintptr_t address = *spec.function + spec.offset;
        const uint8_t* b = reinterpret_cast<const uint8_t*>(address);
        // a push imm8 is two bytes: the `call rel32` after it is displaced with it (7 bytes), the stub runs both
        const size_t length = spec.kind == 0 ? 7 : 5;
        uint32_t current = 0;
        bool ok = false;
        if (spec.kind == 0)
        {
            ok = b[0] == 0x6A && b[2] == 0xE8;
            current = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(b[1])));
        }
        else
        {
            ok = b[0] == 0x68;
            std::memcpy(&current, b + 1, 4);
        }
        if (!ok || current != spec.vanilla)
        {
            LOG("Site hook bonus.constant[{}] at {:08x}: the code is not the expected push: not installed", spec.index, address);
            continue;
        }
        uintptr_t callTarget = 0;
        if (spec.kind == 0)
        {
            int32_t rel = 0;
            std::memcpy(&rel, b + 3, 4);
            callTarget = address + 7 + rel;
        }
        const uintptr_t stub = buildBonusStub(address, spec.index, current, length, callTarget);
        if (!stub)
        {
            continue;
        }
        std::vector<uint8_t> bytes(length, 0x90);
        bytes[0] = 0xE9;
        const uint32_t jump = static_cast<uint32_t>(stub - (address + 5));
        std::memcpy(&bytes[1], &jump, 4);
        if (Patch::write(address, bytes.data(), bytes.size()))
        {
            ++bonuses;
        }
    }
    size_t slots = 0;
    for (const AbilitySiteSpec& spec : kSlotSites)
    {
        if (!*spec.function)
        {
            continue;
        }
        const uintptr_t address = *spec.function + spec.offset;
        const uint8_t* b = reinterpret_cast<const uint8_t*>(address);
        static const uint8_t expectCopy[5] = {0xC6, 0x44, 0x24, 0x13, 0x01};
        const bool ok = spec.kind == 0 ? std::memcmp(b, expectCopy, 5) == 0 : b[0] == 0xE8;
        if (!ok)
        {
            LOG("Site hook skillslot.select[{}] at {:08x}: the code is not the expected instruction: not installed", spec.index, address);
            continue;
        }
        const uintptr_t stub = buildSlotStub(address, spec);
        if (!stub)
        {
            continue;
        }
        uint8_t bytes[5] = {0xE9, 0, 0, 0, 0};
        const uint32_t jump = static_cast<uint32_t>(stub - (address + 5));
        std::memcpy(bytes + 1, &jump, 4);
        if (Patch::write(address, bytes, sizeof(bytes)))
        {
            ++slots;
        }
    }
    size_t branches = 0;
    for (const BranchSiteSpec& spec : kBranchSites)
    {
        if (!*spec.function)
        {
            continue;
        }
        const uintptr_t address = *spec.function + spec.offset;
        const uint8_t* b = reinterpret_cast<const uint8_t*>(address);
        const bool wide = b[0] == 0x0F && b[1] >= 0x80 && b[1] <= 0x8F;
        const bool narrow = b[0] >= 0x70 && b[0] <= 0x7F;
        const size_t length = (wide ? 6 : 2) + spec.extra;
        if (!(wide || narrow) || length < 5 || (wide && spec.extra))
        {
            LOG("Site hook {}[{}] at {:08x}: the code is not the expected jcc: not installed", spec.name, spec.index, address);
            continue;
        }
        const uintptr_t stub = buildBranchStub(address, static_cast<int>(&spec - kBranchSites), spec.extra);
        if (!stub)
        {
            continue;
        }
        std::vector<uint8_t> bytes(length, 0x90);
        bytes[0] = 0xE9;
        const uint32_t jump = static_cast<uint32_t>(stub - (address + 5));
        std::memcpy(&bytes[1], &jump, 4);
        if (Patch::write(address, bytes.data(), bytes.size()))
        {
            ++branches;
        }
    }
    size_t keys = 0;
    if (Gear::Addr::skillSlotFn)
    {
        const uintptr_t address = Gear::Addr::skillSlotFn + kKeySite;
        const uint8_t* b = reinterpret_cast<const uint8_t*>(address);
        // `jb rel32`
        if (b[0] == 0x0F && b[1] == 0x82)
        {
            const uintptr_t stub = buildKeyStub(address);
            uint8_t bytes[6] = {0xE9, 0, 0, 0, 0, 0x90};
            const uint32_t jump = static_cast<uint32_t>(stub - (address + 5));
            std::memcpy(bytes + 1, &jump, 4);
            if (stub && Patch::write(address, bytes, sizeof(bytes)))
            {
                ++keys;
            }
        }
        else
        {
            LOG("Site hook skillslot.key at {:08x}: the code is not the expected jb: not installed", address);
        }
    }
    LOG("Site hooks: {} of {} level sites, {} of {} constant sites, {} of {} creature.ability sites, {} of {} damage sites, {} of {} effect.add sites, "
        "{} of {} bonus.constant sites, {} of {} skillslot.select sites, {} of {} combat.branch sites, {} of 1 skillslot.key sites installed",
        installed, std::size(kSites), constants, std::size(kConstSites), abilities, std::size(kAbilitySites), damages, std::size(kDamageSites), effects,
        std::size(kEffectSites), bonuses, std::size(kBonusSites), slots, std::size(kSlotSites), branches, std::size(kBranchSites), keys);
    return installed + constants + abilities + damages + effects + bonuses + slots + branches + keys;
}

void SiteHook::setLevelFactorActive(bool active)
{
    g_levelActive.store(active);
}

void SiteHook::setConstantActive(bool active)
{
    g_constantActive.store(active);
}

SiteHook::Stats SiteHook::constantStats()
{
    return {g_constCalls.load(), g_constAnswered.load(), g_constBusy.load()};
}

namespace
{
    std::atomic<int> g_selfState{0};            // 0 idle, 1 running, 2 done
    SiteHook::SelfTest g_selfResult;
    uint32_t g_regs[3];

    // A function that loads 1.0 (fmul) or nothing (fld), the constant, stores eax, ecx and edx and returns st0 as a double:
    //   mov eax, 11111111; mov ecx, 22222222; mov edx, 33333333; [fld1;] fmul|fld dword [c]; mov [r0], eax; mov [r1], ecx;
    //   mov [r2], edx; ret
    uintptr_t buildSelfTestBlob(bool multiply, uint32_t& siteOffset)
    {
        std::vector<uint8_t> code;
        emit(code, {0xB8});
        emit32(code, 0x11111111);
        emit(code, {0xB9});
        emit32(code, 0x22222222);
        emit(code, {0xBA});
        emit32(code, 0x33333333);
        if (multiply)
        {
            emit(code, {0xD9, 0xE8});               // fld1
        }
        siteOffset = static_cast<uint32_t>(code.size());
        emit(code, {multiply ? uint8_t(0xD8) : uint8_t(0xD9), multiply ? uint8_t(0x0D) : uint8_t(0x05)});
        emit32(code, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_testConstant)));
        emit(code, {0xA3});
        emit32(code, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_regs[0])));
        emit(code, {0x89, 0x0D});
        emit32(code, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_regs[1])));
        emit(code, {0x89, 0x15});
        emit32(code, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_regs[2])));
        emit(code, {0xC3});
        void* memory = VirtualAlloc(nullptr, code.size() + 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (memory)
        {
            std::memcpy(memory, code.data(), code.size());
        }
        return reinterpret_cast<uintptr_t>(memory);
    }

    double runBlob(uintptr_t blob)
    {
        g_regs[0] = g_regs[1] = g_regs[2] = 0;
        return reinterpret_cast<double(__cdecl*)()>(blob)();
    }

    uint32_t g_fakeCreature[0x80];          // what the ability self test functions treat as the creature (type at +0x10, ability at +0x120)
    uintptr_t g_abilityBlob[3] = {};
    uintptr_t g_abilityHolders[3] = {};

    // kind 0 store: mov dword [esi+0x120], 100;  kind 1 add: add eax, 1000;  kind 2: the 9 byte computed block.
    // Each: push esi; mov esi, fake; mov eax/ecx/edx, 11111111/22222222/33333333; <site>; store eax, ecx, edx; pop esi; ret.
    uintptr_t buildAbilityBlob(int kind, uint32_t& siteOffset)
    {
        std::vector<uint8_t> code;
        emit(code, {0x56, 0xBE});
        emit32(code, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_fakeCreature[0])));
        emit(code, {0xB8});
        emit32(code, 0x11111111);
        emit(code, {0xB9});
        emit32(code, 0x22222222);
        emit(code, {0xBA});
        emit32(code, 0x33333333);
        siteOffset = static_cast<uint32_t>(code.size());
        if (kind == 0)
        {
            emit(code, {0xC7, 0x86, 0x20, 0x01, 0x00, 0x00});
            emit32(code, 100);
        }
        else if (kind == 1)
        {
            emit(code, {0x05});
            emit32(code, 1000);
        }
        else
        {
            emit(code, {0xF7, 0xDA, 0x1B, 0xD2, 0x33, 0xC9, 0x83, 0xC2, 0x32});
        }
        emit(code, {0xA3});
        emit32(code, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_regs[0])));
        emit(code, {0x89, 0x0D});
        emit32(code, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_regs[1])));
        emit(code, {0x89, 0x15});
        emit32(code, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_regs[2])));
        emit(code, {0x5E, 0xC3});
        void* memory = VirtualAlloc(nullptr, code.size() + 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (memory)
        {
            std::memcpy(memory, code.data(), code.size());
        }
        return reinterpret_cast<uintptr_t>(memory);
    }

    void runAbilityTests(SiteHook::SelfTest& r)
    {
        if (!g_abilityBlob[0])
        {
            const size_t base = std::size(kAbilitySites);
            const size_t lengths[3] = {10, 5, 9};
            for (int k = 0; k < 3; ++k)
            {
                uint32_t offset = 0;
                g_abilityBlob[k] = buildAbilityBlob(k, offset);
                g_abilityHolders[k] = g_abilityBlob[k];
                g_abilityTestSpecs[k] = {static_cast<int>(base + k), &g_abilityHolders[k], offset, static_cast<uint8_t>(k)};
                const int vanilla = k == 0 ? 100 : k == 1 ? 1000 : 0;
                const uintptr_t stub = buildAbilityStub(g_abilityBlob[k] + offset, g_abilityTestSpecs[k], vanilla, lengths[k]);
                std::vector<uint8_t> bytes(lengths[k], 0x90);
                bytes[0] = 0xE9;
                const uint32_t rel = static_cast<uint32_t>(stub - (g_abilityBlob[k] + offset + 5));
                std::memcpy(&bytes[1], &rel, 4);
                std::memcpy(reinterpret_cast<void*>(g_abilityBlob[k] + offset), bytes.data(), bytes.size());
            }
        }
        std::memset(g_fakeCreature, 0, sizeof(g_fakeCreature));
        g_fakeCreature[4] = 77;                     // the type
        runBlob(g_abilityBlob[0]);
        r.abilityStore = static_cast<int>(g_fakeCreature[0x120 / 4]);
        bool kept = g_regs[1] == 0x22222222 && g_regs[2] == 0x33333333;
        runBlob(g_abilityBlob[1]);
        r.abilityAdd = static_cast<int>(g_regs[0] - 0x11111111);
        kept = kept && g_regs[1] == 0x22222222 && g_regs[2] == 0x33333333;
        runBlob(g_abilityBlob[2]);
        r.abilityEdx = static_cast<int>(g_regs[2]);
        kept = kept && g_regs[1] == 0;               // the block ends with xor ecx, ecx
        r.abilityKept = kept;
    }

    uintptr_t g_damageBlob = 0;
    uintptr_t g_ftolThunk = 0;
    float g_damageConstant = 12.4f;
    uint32_t g_fakeVictim[2];               // [0] unused, [1] = the fake creature (type at +0x10)

    // fld dword [c]; mov edi, victim; mov ebp, attacker(= fake creature); call thunk (fistp); mov esi, eax; mov [r0], eax; ret.
    // The thunk is `sub esp, 4; fistp dword [esp]; pop eax; ret`: the displaced __ftol. The site is the call, followed by mov esi, eax.
    void runDamageTest(SiteHook::SelfTest& r)
    {
        if (!g_damageBlob)
        {
            std::vector<uint8_t> thunk{0x83, 0xEC, 0x04, 0xDB, 0x1C, 0x24, 0x58, 0xC3};
            void* t = VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            std::memcpy(t, thunk.data(), thunk.size());
            g_ftolThunk = reinterpret_cast<uintptr_t>(t);
            std::vector<uint8_t> code;
            emit(code, {0x57, 0x55, 0x56});                         // push edi; push ebp; push esi
            emit(code, {0xD9, 0x05});                               // fld dword [c]
            emit32(code, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_damageConstant)));
            emit(code, {0xBF});                                     // mov edi, victim
            emit32(code, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_fakeVictim[0])));
            emit(code, {0xBD});                                     // mov ebp, attacker
            emit32(code, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_fakeCreature[0])));
            const size_t site = code.size();
            emit(code, {0xE8});
            emit32(code, 0);
            emit(code, {0x8B, 0xF0});                               // mov esi, eax
            emit(code, {0xA3});
            emit32(code, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_regs[0])));
            emit(code, {0x5E, 0x5D, 0x5F, 0xC3});                   // pop esi; pop ebp; pop edi; ret
            void* memory = VirtualAlloc(nullptr, code.size() + 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            const uintptr_t at = reinterpret_cast<uintptr_t>(memory) + site;
            const uint32_t rel = static_cast<uint32_t>(g_ftolThunk - (at + 5));
            std::memcpy(&code[site + 1], &rel, 4);
            std::memcpy(memory, code.data(), code.size());
            g_damageBlob = reinterpret_cast<uintptr_t>(memory);
            // the stub, like the installer does it
            const uintptr_t stub = buildDamageStub(at, g_ftolThunk, static_cast<int>(std::size(kDamageSites)));
            uint8_t bytes[5] = {0xE9, 0, 0, 0, 0};
            const uint32_t jump = static_cast<uint32_t>(stub - (at + 5));
            std::memcpy(bytes + 1, &jump, 4);
            std::memcpy(reinterpret_cast<void*>(at), bytes, 5);
        }
        std::memset(g_fakeCreature, 0, sizeof(g_fakeCreature));
        g_fakeCreature[4] = 77;                                     // the attacker's type
        g_fakeVictim[1] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_fakeCreature[0]));
        runBlob(g_damageBlob);
        r.damage = static_cast<int>(g_regs[0]);
    }

    // The effect.add self test: a function with the engine's frame (sub esp, 0x18; four pushes), a fake effect list in memory and the six byte
    // site; the three exits record which one the stub took (1: the clamp code (engine path), 2: next entry, 3: leave).
    alignas(16) uint8_t g_fakeList[0x800];
    uintptr_t g_effectBlob = 0;
    float g_effectSum = 3.5f;
    uint32_t g_effectExit = 0;

    void setupFakeList()
    {
        std::memset(g_fakeList, 0, sizeof(g_fakeList));
        const uintptr_t list = reinterpret_cast<uintptr_t>(&g_fakeList[0x400]);
        const uintptr_t entries = reinterpret_cast<uintptr_t>(&g_fakeList[0x600]);
        const int32_t id = 5, flags = 0x4000000, counter = 4;
        const uint16_t ability = 7;
        std::memcpy(reinterpret_cast<void*>(list - 0x39C), &id, 4);
        std::memcpy(reinterpret_cast<void*>(list - 0x288), &ability, 2);
        std::memcpy(reinterpret_cast<void*>(list + 0x148), &flags, 4);
        std::memcpy(reinterpret_cast<void*>(list - 0x214), &counter, 4);
        const int32_t end = static_cast<int32_t>(entries + 2 * 0x16);
        std::memcpy(reinterpret_cast<void*>(list + 0x106), &end, 4);
        for (int k = 0; k < 2; ++k)
        {
            const uintptr_t e = entries + k * 0x16;
            const uint16_t effect = static_cast<uint16_t>(10 + k);
            const float maxValue = 100.0f, value = 1.0f;
            std::memcpy(reinterpret_cast<void*>(e + 4), &effect, 2);
            std::memcpy(reinterpret_cast<void*>(e + 0xA), &maxValue, 4);
            std::memcpy(reinterpret_cast<void*>(e + 0x12), &value, 4);
        }
    }

    void runEffectTest(SiteHook::SelfTest& r)
    {
        if (!g_effectBlob)
        {
            std::vector<uint8_t> code;
            emit(code, {0x83, 0xEC, 0x18, 0x53, 0x55, 0x56, 0x57});    // sub esp, 0x18; push ebx; push ebp; push esi; push edi
            emit(code, {0xBE});                                         // mov esi, list
            emit32(code, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_fakeList[0x400])));
            emit(code, {0xB9});                                         // mov ecx, entry 0
            emit32(code, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_fakeList[0x600])));
            emit(code, {0xD9, 0x05});                                   // fld dword [sum]
            emit32(code, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_effectSum)));
            const size_t site = code.size();
            emit(code, {0xD9, 0x59, 0x12, 0xD9, 0x41, 0x12});          // the site
            emit(code, {0xDD, 0xD8});                                   // engine path: fstp st(0)
            emit(code, {0xC7, 0x05});
            emit32(code, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_effectExit)));
            emit32(code, 1);
            emit(code, {0xEB, 0x00});
            const size_t jmp1 = code.size() - 1;
            const size_t nextAt = code.size();
            emit(code, {0xC7, 0x05});
            emit32(code, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_effectExit)));
            emit32(code, 2);
            emit(code, {0xEB, 0x00});
            const size_t jmp2 = code.size() - 1;
            const size_t leaveAt = code.size();
            emit(code, {0xC7, 0x05});
            emit32(code, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_effectExit)));
            emit32(code, 3);
            const size_t endAt = code.size();
            emit(code, {0x5F, 0x5E, 0x5D, 0x5B, 0x83, 0xC4, 0x18, 0xC3});
            code[jmp1] = static_cast<uint8_t>(endAt - (jmp1 + 1));
            code[jmp2] = static_cast<uint8_t>(endAt - (jmp2 + 1));
            void* memory = VirtualAlloc(nullptr, code.size() + 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            const uintptr_t base = reinterpret_cast<uintptr_t>(memory);
            std::memcpy(memory, code.data(), code.size());
            g_effectBlob = base;
            const uintptr_t stub = buildEffectStub(base + site, static_cast<int>(std::size(kEffectSites)), base + nextAt, base + leaveAt);
            uint8_t bytes[6] = {0xE9, 0, 0, 0, 0, 0x90};
            const uint32_t jump = static_cast<uint32_t>(stub - (base + site + 5));
            std::memcpy(bytes + 1, &jump, 4);
            std::memcpy(reinterpret_cast<void*>(base + site), bytes, 6);
        }
        setupFakeList();
        g_effectExit = 0;
        runBlob(g_effectBlob);
        r.effectExit = static_cast<int>(g_effectExit);
        float v0 = 0, v1 = 0;
        int32_t counter = 0;
        std::memcpy(&v0, &g_fakeList[0x600 + 0x12], 4);
        std::memcpy(&v1, &g_fakeList[0x600 + 0x16 + 0x12], 4);
        std::memcpy(&counter, &g_fakeList[0x400 - 0x214], 4);
        r.effectEntry0 = static_cast<int>(v0 * 100 + 0.5f);
        r.effectEntry1 = static_cast<int>(v1 * 100 + 0.5f);
        r.effectCounter = counter;
    }

    uintptr_t g_bonusBlob[2] = {};
    size_t g_bonusCallFixup = 0;

    // push imm8 5 / push imm32 3.5f; pop eax; mov [r0], eax; ret  -- the site is the push
    void runBonusTest(SiteHook::SelfTest& r)
    {
        if (!g_bonusBlob[0])
        {
            for (int k = 0; k < 2; ++k)
            {
                std::vector<uint8_t> code;
                size_t length;
                uint32_t vanilla;
                uintptr_t callTarget = 0;
                if (k == 0)
                {
                    static uint8_t* thunk = nullptr;
                    if (!thunk)
                    {
                        thunk = static_cast<uint8_t*>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
                        thunk[0] = 0xC3;
                    }
                    callTarget = reinterpret_cast<uintptr_t>(thunk);
                    emit(code, {0x6A, 0x05, 0xE8});
                    const size_t relAt = code.size();
                    emit32(code, 0);
                    length = 7;
                    vanilla = 5;
                    g_bonusCallFixup = relAt;
                }
                else
                {
                    const float f = 3.5f;
                    std::memcpy(&vanilla, &f, 4);
                    emit(code, {0x68});
                    emit32(code, vanilla);
                    length = 5;
                }
                if (k == 0)
                {
                    const uintptr_t at = 0;     // patched below once the blob has an address
                    (void)at;
                }
                emit(code, {0x58, 0xA3});                           // pop eax; mov [r0], eax
                emit32(code, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_regs[0])));
                emit(code, {0xC3});
                void* memory = VirtualAlloc(nullptr, code.size() + 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                std::memcpy(memory, code.data(), code.size());
                g_bonusBlob[k] = reinterpret_cast<uintptr_t>(memory);
                if (k == 0)
                {
                    const uint32_t rel = static_cast<uint32_t>(callTarget - (g_bonusBlob[k] + g_bonusCallFixup + 4));
                    std::memcpy(reinterpret_cast<uint8_t*>(memory) + g_bonusCallFixup, &rel, 4);
                }
                static uintptr_t holders[2];
                holders[k] = g_bonusBlob[k];
                g_bonusTestSpecs[k] = {static_cast<int>(std::size(kBonusSites)) + k, &holders[k], 0, static_cast<uint8_t>(k == 0 ? 0 : 2), vanilla};
                const uintptr_t stub = buildBonusStub(g_bonusBlob[k], g_bonusTestSpecs[k].index, vanilla, length, callTarget);
                std::vector<uint8_t> bytes(length, 0x90);
                bytes[0] = 0xE9;
                const uint32_t jump = static_cast<uint32_t>(stub - (g_bonusBlob[k] + 5));
                std::memcpy(&bytes[1], &jump, 4);
                std::memcpy(reinterpret_cast<void*>(g_bonusBlob[k]), bytes.data(), length);
            }
        }
        runBlob(g_bonusBlob[0]);
        r.bonusInt = static_cast<int>(g_regs[0]);
        runBlob(g_bonusBlob[1]);
        float f;
        std::memcpy(&f, &g_regs[0], 4);
        r.bonusFloat = f;
    }

    uintptr_t g_slotBlob = 0;
    AbilitySiteSpec g_slotTestSpec = {};

    // sub esp, 0x20; <mov byte [esp+0x13], 1>; movzx eax, byte [esp+0x13]; mov [r0], eax; add esp, 0x20; ret   -- the site is the mov
    void runSlotTest(SiteHook::SelfTest& r)
    {
        if (!g_slotBlob)
        {
            std::vector<uint8_t> code;
            emit(code, {0x83, 0xEC, 0x20});
            const size_t site = code.size();
            emit(code, {0xC6, 0x44, 0x24, 0x13, 0x01});
            emit(code, {0x0F, 0xB6, 0x44, 0x24, 0x13, 0xA3});
            emit32(code, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_regs[0])));
            emit(code, {0x83, 0xC4, 0x20, 0xC3});
            void* memory = VirtualAlloc(nullptr, code.size() + 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            std::memcpy(memory, code.data(), code.size());
            g_slotBlob = reinterpret_cast<uintptr_t>(memory);
            static uintptr_t holder;
            holder = g_slotBlob;
            g_slotTestSpec = {static_cast<int>(std::size(kSlotSites)), &holder, 0, 0};
            const uintptr_t stub = buildSlotStub(g_slotBlob + site, g_slotTestSpec);
            uint8_t bytes[5] = {0xE9, 0, 0, 0, 0};
            const uint32_t jump = static_cast<uint32_t>(stub - (g_slotBlob + site + 5));
            std::memcpy(bytes + 1, &jump, 4);
            std::memcpy(reinterpret_cast<void*>(g_slotBlob + site), bytes, 5);
        }
        runBlob(g_slotBlob);
        r.slotByte = static_cast<int>(g_regs[0]);
    }

    uintptr_t g_branchBlob = 0, g_branchBlob2 = 0;

    // xor eax, eax; cmp eax, 0 (flags: equal); je L; mov [r0], 1; ret; L: mov [r0], 2; ret   -- the site is the je (the engine's branch is taken)
    void runBranchTest(SiteHook::SelfTest& r)
    {
        if (!g_branchBlob)
        {
            std::vector<uint8_t> code;
            emit(code, {0x33, 0xC0, 0x83, 0xF8, 0x00});
            const size_t site = code.size();
            emit(code, {0x0F, 0x84});
            emit32(code, 11);                                       // over `mov [r0], 1; ret` (10 + 1 bytes)
            emit(code, {0xC7, 0x05});
            emit32(code, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_regs[0])));
            emit32(code, 1);
            emit(code, {0xC3});
            emit(code, {0xC7, 0x05});
            emit32(code, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_regs[0])));
            emit32(code, 2);
            emit(code, {0xC3});
            void* memory = VirtualAlloc(nullptr, code.size() + 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            std::memcpy(memory, code.data(), code.size());
            g_branchBlob = reinterpret_cast<uintptr_t>(memory);
            static uintptr_t holder;
            holder = g_branchBlob;
            g_branchTestSpecs[0] = {"selftest.branch", 0, &holder, static_cast<uint32_t>(site), 0};
            const uintptr_t stub = buildBranchStub(g_branchBlob + site, static_cast<int>(std::size(kBranchSites)), 0);
            uint8_t bytes[6] = {0xE9, 0, 0, 0, 0, 0x90};
            const uint32_t jump = static_cast<uint32_t>(stub - (g_branchBlob + site + 5));
            std::memcpy(bytes + 1, &jump, 4);
            std::memcpy(reinterpret_cast<void*>(g_branchBlob + site), bytes, 6);
        }
        runBlob(g_branchBlob);
        r.branchTaken = static_cast<int>(g_regs[0]);

        // a `jcc rel8` with a displaced instruction: mov eax, 0x28; cmp eax, 0x28; jge T; add eax, -2; mov [r1], eax; mov [r0], 1; ret; T: mov [r0], 2; ret
        if (!g_branchBlob2)
        {
            std::vector<uint8_t> code;
            emit(code, {0xB8, 0x28, 0x00, 0x00, 0x00, 0x83, 0xF8, 0x28});
            const size_t site = code.size();
            emit(code, {0x7D, 0x13});
            emit(code, {0x83, 0xC0, 0xFE});
            emit(code, {0xA3});
            emit32(code, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_regs[1])));
            emit(code, {0xC7, 0x05});
            emit32(code, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_regs[0])));
            emit32(code, 1);
            emit(code, {0xC3});
            emit(code, {0xC7, 0x05});
            emit32(code, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_regs[0])));
            emit32(code, 2);
            emit(code, {0xC3});
            void* memory = VirtualAlloc(nullptr, code.size() + 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            std::memcpy(memory, code.data(), code.size());
            g_branchBlob2 = reinterpret_cast<uintptr_t>(memory);
            static uintptr_t holder2;
            holder2 = g_branchBlob2;
            g_branchTestSpecs[1] = {"selftest.branch2", 0, &holder2, static_cast<uint32_t>(site), 3};
            const uintptr_t stub = buildBranchStub(g_branchBlob2 + site, static_cast<int>(std::size(kBranchSites)) + 1, 3);
            std::vector<uint8_t> bytes(5, 0x90);
            bytes[0] = 0xE9;
            const uint32_t jump = static_cast<uint32_t>(stub - (g_branchBlob2 + site + 5));
            std::memcpy(&bytes[1], &jump, 4);
            std::memcpy(reinterpret_cast<void*>(g_branchBlob2 + site), bytes.data(), 5);
        }
        g_regs[1] = 0;
        runBlob(g_branchBlob2);
        r.branch2Path = static_cast<int>(g_regs[0]);
        r.branch2Eax = static_cast<int>(g_regs[1]);
    }

    void selfTestThread()
    {
        Sleep(150);     // Lua is not running now
        SiteHook::SelfTest r;
        const bool first = !g_testBlob[0];
        if (first)
        {
            const char* names[2] = {"selftest.fmul", "selftest.fld"};
            for (int k = 0; k < 2; ++k)
            {
                uint32_t offset = 0;
                g_testBlob[k] = buildSelfTestBlob(k == 0, offset);
                static uintptr_t holders[2];
                holders[k] = g_testBlob[k];
                g_testSpecs[k] = {names[k], 0, &holders[k], offset, static_cast<uint8_t>(k == 0 ? 1 : 0), static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_testConstant))};
                const uintptr_t stub = buildConstantStub(g_testBlob[k] + offset, g_testSpecs[k], static_cast<int>(std::size(kConstSites)) + k);
                uint8_t bytes[6] = {0xE9, 0, 0, 0, 0, 0x90};
                const uint32_t rel = static_cast<uint32_t>(stub - (g_testBlob[k] + offset + 5));
                std::memcpy(bytes + 1, &rel, 4);
                std::memcpy(reinterpret_cast<void*>(g_testBlob[k] + offset), bytes, 6);
            }
        }
        r.fmul = runBlob(g_testBlob[0]);
        const bool regsA = g_regs[0] == 0x11111111 && g_regs[1] == 0x22222222 && g_regs[2] == 0x33333333;
        r.fld = runBlob(g_testBlob[1]);
        const bool regsB = g_regs[0] == 0x11111111 && g_regs[1] == 0x22222222 && g_regs[2] == 0x33333333;
        r.registersKept = regsA && regsB;
        runAbilityTests(r);
        runDamageTest(r);
        runEffectTest(r);
        runBonusTest(r);
        runSlotTest(r);
        runBranchTest(r);
        g_selfResult = r;
        g_selfState = 2;
    }
}

bool SiteHook::selfTestStart()
{
    int idle = 0;
    if (!g_selfState.compare_exchange_strong(idle, 1))
    {
        return false;
    }
    std::thread(selfTestThread).detach();
    return true;
}

bool SiteHook::selfTestResult(SelfTest& out)
{
    if (g_selfState.load() != 2)
    {
        return false;
    }
    out = g_selfResult;
    g_selfState = 0;
    return true;
}

std::vector<SiteHook::SiteInfo> SiteHook::sites()
{
    std::vector<SiteInfo> out;
    for (size_t i = 0; i < std::size(kSites); ++i)
    {
        const uintptr_t fn = *kSites[i].function;
        out.push_back({kSites[i].hook, static_cast<int>(i), fn ? fn + kSites[i].offset : 0, g_levelSiteCalls[i].load()});
    }
    for (size_t i = 0; i < std::size(kConstSites); ++i)
    {
        const uintptr_t fn = *kConstSites[i].function;
        out.push_back({kConstSites[i].name, kConstSites[i].index, fn ? fn + kConstSites[i].offset : 0, g_constSiteCalls[i].load()});
    }
    for (size_t i = 0; i < std::size(kBranchSites); ++i)
    {
        const uintptr_t fn = *kBranchSites[i].function;
        out.push_back({"combat.branch", static_cast<int>(i), fn ? fn + kBranchSites[i].offset : 0, g_branchSiteCalls[i].load()});
    }
    for (size_t i = 0; i < std::size(kSlotSites); ++i)
    {
        const uintptr_t fn = *kSlotSites[i].function;
        out.push_back({"skillslot.select", kSlotSites[i].index, fn ? fn + kSlotSites[i].offset : 0, g_slotSiteCalls[i].load()});
    }
    for (size_t i = 0; i < std::size(kBonusSites); ++i)
    {
        const uintptr_t fn = *kBonusSites[i].function;
        out.push_back({"bonus.constant", kBonusSites[i].index, fn ? fn + kBonusSites[i].offset : 0, g_bonusSiteCalls[i].load()});
    }
    for (size_t i = 0; i < std::size(kEffectSites); ++i)
    {
        const uintptr_t fn = *kEffectSites[i].function;
        out.push_back({"effect.add", kEffectSites[i].index, fn ? fn + kEffectSites[i].offset : 0, g_effectSiteCalls[i].load()});
    }
    for (size_t i = 0; i < std::size(kDamageSites); ++i)
    {
        const uintptr_t fn = *kDamageSites[i].function;
        out.push_back({"damage", kDamageSites[i].index, fn ? fn + kDamageSites[i].offset : 0, g_damageSiteCalls[i].load()});
    }
    for (size_t i = 0; i < std::size(kAbilitySites); ++i)
    {
        const uintptr_t fn = *kAbilitySites[i].function;
        out.push_back({"creature.ability", kAbilitySites[i].index, fn ? fn + kAbilitySites[i].offset : 0, g_abilitySiteCalls[i].load()});
    }
    return out;
}

void SiteHook::setAbilityActive(bool active)
{
    g_abilityActive.store(active);
}

void SiteHook::setDamageActive(bool active)
{
    g_damageActive.store(active);
}

SiteHook::Stats SiteHook::damageStats()
{
    return {g_damageCalls.load(), g_damageAnswered.load(), g_damageBusy.load()};
}

void SiteHook::setEffectActive(bool active)
{
    g_effectActive.store(active);
}

SiteHook::Stats SiteHook::effectStats()
{
    return {g_effectCalls.load(), g_effectAnswered.load(), g_effectBusy.load()};
}

void SiteHook::setBonusActive(bool active)
{
    g_bonusActive.store(active);
}

SiteHook::Stats SiteHook::bonusStats()
{
    return {g_bonusCalls.load(), g_bonusAnswered.load(), g_bonusBusy.load()};
}

void SiteHook::setSkillSlotActive(bool active)
{
    g_slotActive.store(active);
}

SiteHook::Stats SiteHook::skillSlotStats()
{
    return {g_slotCalls.load(), g_slotAnswered.load(), g_slotBusy.load()};
}

void SiteHook::setBranchActive(bool active)
{
    g_branchActive.store(active);
}

SiteHook::Stats SiteHook::branchStats()
{
    return {g_branchCalls.load(), g_branchAnswered.load(), g_branchBusy.load()};
}

void SiteHook::setSkillKeyActive(bool active)
{
    g_keyActive.store(active);
}

SiteHook::Stats SiteHook::skillKeyStats()
{
    return {g_keyCalls.load(), g_keyAnswered.load(), g_keyBusy.load()};
}

SiteHook::Stats SiteHook::abilityStats()
{
    return {g_abilityCalls.load(), g_abilityAnswered.load(), g_abilityBusy.load()};
}

SiteHook::Stats SiteHook::levelFactorStats()
{
    return {g_calls.load(), g_answered.load(), g_busy.load()};
}
