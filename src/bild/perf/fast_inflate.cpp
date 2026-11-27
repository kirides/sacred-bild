#include "fast_inflate.h"
#include "diag/d3d_stats.h"
#include "engine/sacred_addr.h"
#include "settings/config/render.h"
#include "log.h"
#include "patch.h"

#include <libdeflate.h>

namespace
{
    using namespace Sacred;
    using UncompressFn = int(__cdecl*)(uint8_t* dest, unsigned long* destLen, const uint8_t* source, unsigned long sourceLen);
    UncompressFn g_origUncompress = nullptr;

    // zlib's uncompress, as the game links it (1.2.1): one zlib stream in, *destLen bytes of room out; Z_OK (0) with
    // *destLen set to what was written. libdeflate decodes a whole stream at once, several times faster than zlib's
    // inflate. What it refuses (too little room, damaged data) goes to the game's zlib, whose result and partial output
    // the caller then gets exactly as before.
    int __cdecl hookUncompress(uint8_t* dest, unsigned long* destLen, const uint8_t* source, unsigned long sourceLen)
    {
        D3DStats::Scope timer{D3DStats::TInflate};
        thread_local libdeflate_decompressor* decompressor = libdeflate_alloc_decompressor();
        if (decompressor && dest && destLen && source)
        {
            size_t in = 0, out = 0;
            if (libdeflate_zlib_decompress_ex(decompressor, source, sourceLen, dest, *destLen, &in, &out) == LIBDEFLATE_SUCCESS)
            {
                *destLen = static_cast<unsigned long>(out);
                return 0;
            }
        }
        return g_origUncompress(dest, destLen, source, sourceLen);
    }
}

void FastInflate::install()
{
    if (!Config::render.fastInflate || !Addr::zlibUncompress)
    {
        return;
    }
    // zlib 1.2.1's uncompress: sub esp, 38h (the z_stream on the stack); mov ecx, [esp + 48h] (destLen).
    if (!Patch::verify(Addr::zlibUncompress, {0x83, 0xEC, 0x38, 0x8B, 0x4C, 0x24, 0x48}))
    {
        LOG("FastInflate: the game's uncompress is not the expected code; zlib stays");
        return;
    }
    Patch::hook(g_origUncompress, static_cast<uintptr_t>(Addr::zlibUncompress), &hookUncompress, "zlib uncompress");
}
