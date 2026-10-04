#pragma once
#include <cstdint>
#include <initializer_list>

namespace Patch
{
    // Overwrites code/data in the game image; returns false if the expected bytes don't match.
    bool write(uintptr_t addr, const void* data, size_t size);
    bool verify(uintptr_t addr, std::initializer_list<uint8_t> expected);

    template <class T>
    bool value(uintptr_t addr, T v) { return write(addr, &v, sizeof(v)); }

    // Replaces an imm32 operand only if it currently holds `expected`.
    bool imm32(uintptr_t addr, uint32_t expected, uint32_t value);

    // Retargets an existing `call rel32` (E8) instruction.
    bool redirectCall(uintptr_t callSite, const void* target);

    // Replaces an import of the main exe (by name, or by ordinal if the slot is bound to `function`); returns the
    // previous target or nullptr.
    void* iat(const char* dll, const char* function, void* replacement);
    // The same for an import of a loaded module (`importer`, e.g. "tincat2.dll"; nullptr = the main exe).
    void* iat(const char* importer, const char* dll, const char* function, void* replacement);

    // Detours a function; `original` receives the trampoline.
    bool hook(void** original, void* detour, const char* name);

    template <class T, class D>
    bool hook(T& original, D detour, const char* name)
    {
        return hook(reinterpret_cast<void**>(&original), reinterpret_cast<void*>(detour), name);
    }

    // Detours the function at `target` (a resolved address; 0 is logged and skipped).
    template <class T, class D>
    bool hook(T& original, uintptr_t target, D detour, const char* name)
    {
        original = reinterpret_cast<T>(target);
        return hook(reinterpret_cast<void**>(&original), reinterpret_cast<void*>(detour), name);
    }

    // Detours transaction around a batch of hook() calls.
    void begin();
    bool commit();
}
