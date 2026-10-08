#pragma once
#include <cstdint>
#include <cstring>

// Fields of the game's (and granny.dll's) objects by byte offset, for the layouts in sacred_addr.h and granny_mesh.h.
namespace Mem
{
    // The field itself, to read or write in place.
    template <class T>
    T& member(void* obj, uintptr_t offset)
    {
        return *reinterpret_cast<T*>(static_cast<uint8_t*>(obj) + offset);
    }

    // A copy of the field, at any alignment.
    template <class T>
    T field(const void* p, uintptr_t offset)
    {
        T v;
        std::memcpy(&v, static_cast<const uint8_t*>(p) + offset, sizeof(T));
        return v;
    }
}
