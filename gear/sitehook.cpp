#include "sitehook.h"
#include "log.h"
#include "lua_host.h"
#include "patch.h"
#include "game/gear_addr.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
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
#include "game/gear_sites.inc"

    constexpr int kNoAnswer = static_cast<int>(0x80000000);   // the handler's "no mod answered"

    std::atomic<bool> g_levelActive{false};
    std::atomic<unsigned> g_calls{0}, g_answered{0}, g_busy{0};

    // Called by the stubs: the level as a double and the site's index. Any thread the game calls from.
    extern "C" int __cdecl levelFactorHandler(double level, int site)
    {
        g_calls.fetch_add(1, std::memory_order_relaxed);
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
    LOG("Site hooks: {} of {} installed", installed, std::size(kSites));
    return installed;
}

void SiteHook::setLevelFactorActive(bool active)
{
    g_levelActive.store(active);
}

SiteHook::Stats SiteHook::levelFactorStats()
{
    return {g_calls.load(), g_answered.load(), g_busy.load()};
}
