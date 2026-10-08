#pragma once
#include <cstddef>

// The exe's std::vector: first, last and end of storage (12 bytes).
namespace Sacred
{
    template <class T>
    struct Vector
    {
        T* first;
        T* last;
        T* capacity;

        // A vector that doesn't look like one (null, or last before first) is empty.
        size_t size() const { return first && last > first ? static_cast<size_t>(last - first) : 0; }
        T* begin() const { return first; }
        T* end() const { return first + size(); }
        T& operator[](size_t i) const { return first[i]; }
    };
    static_assert(sizeof(Vector<int>) == 12);
}
