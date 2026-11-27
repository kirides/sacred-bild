#pragma once
#include <string>

// [DDraw] in SacredBild.ini (read by config.cpp).
namespace Config
{
    struct DDraw
    {
        // DirectDraw / Direct3D 7 on SacredBild's own Direct3D 9Ex backend (Backend=d3d9), or on the chain-loaded ddraw
        // (Backend=chain): `chain`, or the system ddraw.dll if that is empty or missing.
        bool d3d9 = true;
        // Backend=nvrhi: SacredBild's own DirectDraw / Direct3D 7 on NVRHI (docs/NvrhiRenderer.md), on Direct3D 12 or with
        // [Render] Api=vulkan on Vulkan. d3d9 is false then. Experimental, opt-in.
        bool nvrhi = false;
        bool vulkan = false;
        // One of SacredBild's own backends (d3d9 or nvrhi), not the chain-loaded ddraw.
        bool own() const { return d3d9 || nvrhi; }
        std::wstring chain = L"SacredBild\\DDrawCompat.dll";
        // Backend=d3d9: d3d9.dll to load first (e.g. DXVK), relative to the game folder or absolute. Then a d3d9.dll
        // next to the exe, then the system's.
        std::wstring d3d9Path;
        // Movies through Media Foundation instead of the game's DirectShow/DirectDraw path. Backend=d3d9 always does
        // (DirectShow can't decode into its surfaces); with Backend=chain this decides.
        bool mediaFoundation = true;
    };
    inline DDraw ddraw;
}
