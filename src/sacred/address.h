#pragma once
#include <cstdint>

// Typed addresses in sacred.exe: a function with its signature, or a global variable with its type. Each holds the
// address Sig::resolve found at startup (0 until then, or if its signature did not match) and converts to it, so
// hooks and patches take it like a plain address. Declared in game/sacred_addr.h, used through the classes in
// sacred/.
namespace Sacred
{
    template <class Signature>
    struct Thiscall;

    // thiscall: `this` in ECX, the arguments on the stack, popped by the callee. MSVC has __thiscall for member
    // functions only, so these are called through (and hooks are written as) __fastcall with an unused EDX.
    template <class R, class Self, class... Args>
    struct Thiscall<R(Self, Args...)>
    {
        using Ptr = R(__fastcall*)(Self, void* edx, Args...);

        uintptr_t address;

        operator uintptr_t() const { return address; }
        Ptr ptr() const { return reinterpret_cast<Ptr>(address); }
        R operator()(Self self, Args... args) const { return ptr()(self, nullptr, args...); }
    };

    template <class Signature>
    struct Cdecl;

    // cdecl: the arguments on the stack, popped by the caller.
    template <class R, class... Args>
    struct Cdecl<R(Args...)>
    {
        using Ptr = R(__cdecl*)(Args...);

        uintptr_t address;

        operator uintptr_t() const { return address; }
        Ptr ptr() const { return reinterpret_cast<Ptr>(address); }
        R operator()(Args... args) const { return ptr()(args...); }
    };

    // A global variable of the game.
    template <class T>
    struct Global
    {
        uintptr_t address;

        operator uintptr_t() const { return address; }
        T* get() const { return reinterpret_cast<T*>(address); }
        T& operator*() const { return *get(); }
        T* operator->() const { return get(); }
    };
}
