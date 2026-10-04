#pragma once
#include <windows.h>

#include <span>

// SacredBild's own DirectDraw / Direct3D 7 implementation on Direct3D 9Ex ([DDraw] Backend=d3d9).
namespace DDraw9
{
    struct Export
    {
        const char* name;
        FARPROC proc;
    };

    // The ddraw.dll exports the backend implements; the others keep going to the system ddraw.dll.
    std::span<const Export> exports();

    // The system ddraw.dll: the exports above fall back to it if Direct3D 9Ex is not available.
    void setFallback(HMODULE systemDdraw);
}
