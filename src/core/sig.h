#pragma once
#include <cstddef>
#include <concepts>
#include <cstdint>
#include <span>

// Byte signatures: code is located by its bytes instead of fixed addresses, so one table serves every build with
// the same code. tools/sigs/gen_sigs.py makes them from the English GOG build and checks them against the German one.
namespace Sig
{
    enum class Take : uint8_t
    {
        Match,  // the match + offset
        Abs32,  // the absolute address stored at match + offset (an instruction's address operand)
        Rel32,  // the target of the relative operand at match + offset (where a call or jmp there goes): finds a function
                // from a call site, so the function's own bytes may be detoured by another module
    };

    // Where an entry's address goes: a uintptr_t, or the `address` of a typed one (sacred/address.h).
    struct Out
    {
        uintptr_t* address;

        constexpr Out(uintptr_t* p) : address(p) {}

        template <class A>
            requires requires(A& a) { { a.address } -> std::same_as<uintptr_t&>; }
        constexpr Out(A* a) : address(&a->address) {}
    };

    struct Entry
    {
        const char* name;
        const char* pattern;    // hex bytes, "??" = any byte; the first byte must be fixed
        uint16_t offset;
        Take take;
        Out out;                // receives the address; 0 if the pattern does not match exactly once
    };

    // Scans the host exe's code section once for all entries; returns how many did not resolve (each is logged). The bytes are
    // the exe file's (as before any module patched them), the addresses the running image's.
    size_t resolve(std::span<const Entry> entries);

    // The same over `code`, which is mapped at `codeVa` in the image the addresses are for (tests).
    size_t resolve(std::span<const Entry> entries, std::span<const uint8_t> code, uintptr_t codeVa);
}
