#pragma once
#include "ddraw9/backend.h"

#include <span>

// SacredBild's DirectDraw / Direct3D 7 on NVRHI ([DDraw] Backend=nvrhi, docs/NvrhiRenderer.md). Where NVRHI can't make a
// device (neither Direct3D 12 nor Vulkan), its exports fall back to the Direct3D 9 backend's, and that one to the system
// ddraw.dll.
namespace DDrawRhi
{
    // The ddraw.dll exports the backend implements; the others keep going to the system ddraw.dll.
    std::span<const DDraw9::Export> exports();
}
