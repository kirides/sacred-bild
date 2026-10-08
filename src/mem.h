#pragma once
#include <cstdint>
#include <cstring>

// Fields of granny.dll's objects by byte offset, for the layouts in game/granny_mesh.h (the game's own objects are
// structs in sacred/).
namespace Mem
{
    // A copy of the field, at any alignment.
    template <class T>
    T field(const void* p, uintptr_t offset)
    {
        T v;
        std::memcpy(&v, static_cast<const uint8_t*>(p) + offset, sizeof(T));
        return v;
    }
}
