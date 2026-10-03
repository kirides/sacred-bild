#pragma once
#include <windows.h>
#include <d3d.h>

// Layout of Direct3D 7 flexible vertex formats.
namespace Fvf
{
    inline bool pretransformed(DWORD fvf)
    {
        return (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
    }

    inline UINT texCount(DWORD fvf)
    {
        return (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    }

    // Floats in texture coordinate set `set` (D3DFVF_TEXTUREFORMAT2, 3, 4, 1).
    inline UINT texCoordSize(DWORD fvf, UINT set)
    {
        static constexpr UINT kSizes[4] = {2, 3, 4, 1};
        return kSizes[(fvf >> (16 + set * 2)) & 3];
    }

    // Byte offset of texture coordinate set `set`.
    inline UINT texCoordOffset(DWORD fvf, UINT set)
    {
        UINT offset = 0;
        switch (fvf & D3DFVF_POSITION_MASK)
        {
        case D3DFVF_XYZ: offset = 12; break;
        case D3DFVF_XYZRHW: offset = 16; break;
        case D3DFVF_XYZB1: offset = 16; break;
        case D3DFVF_XYZB2: offset = 20; break;
        case D3DFVF_XYZB3: offset = 24; break;
        case D3DFVF_XYZB4: offset = 28; break;
        case D3DFVF_XYZB5: offset = 32; break;
        }
        if (fvf & D3DFVF_NORMAL) offset += 12;
        if (fvf & D3DFVF_RESERVED1) offset += 4;
        if (fvf & D3DFVF_DIFFUSE) offset += 4;
        if (fvf & D3DFVF_SPECULAR) offset += 4;
        for (UINT i = 0; i < set; ++i)
        {
            offset += texCoordSize(fvf, i) * 4;
        }
        return offset;
    }

    // Byte size of one vertex.
    inline UINT stride(DWORD fvf)
    {
        return texCoordOffset(fvf, texCount(fvf));
    }
}
