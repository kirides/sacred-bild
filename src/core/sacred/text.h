#pragma once
#include "game/sacred_addr.h"

#include <cstddef>
#include <cstdint>

// The game's texts.
namespace Sacred
{
    // The text table (g_textTable, ENG 0182ED50; a static object, destroyed at exit). global.res, in memory as on
    // disk: uint32 count, count entries {uint32 id, uint32 offset, uint32 flags, uint32 bytes} by ascending id, then
    // the UTF-16 strings; an entry's string starts at 4 + offset.
    struct cTextTable
    {
        uint8_t* data;
        uint32_t size;              // bytes
    };
    static_assert(offsetof(cTextTable, size) == 0x04);

    // Texts by id (the button prompts beside menu texts).
    struct cTextResources
    {
        // Made on first use.
        static cTextResources* instance() { return Addr::textResources_instance(); }

        // {begin, end} of the text; an empty text for an unknown id.
        const wchar_t* const* text(uint32_t id) { return Addr::textResources_get(this, id); }
    };
}
