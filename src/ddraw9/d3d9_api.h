#pragma once
// Direct3D 7 (ddraw.h, d3d.h) and Direct3D 9 in one translation unit. Their headers declare many of the same names
// (D3DPRIMITIVETYPE, D3DRENDERSTATETYPE, ...) with different contents, so Direct3D 9 lives in namespace d9. The
// structures both versions share and guard (D3DVECTOR, D3DCOLORVALUE, D3DRECT, D3DMATRIX) come from the
// Direct3D 7 headers; their layouts are identical.
#include <windows.h>
#include <objbase.h>
#include <stdlib.h>
#include <ddraw.h>
#include <d3d.h>

#pragma push_macro("DIRECT3D_VERSION")
#undef DIRECT3D_VERSION
#define DIRECT3D_VERSION 0x0900
#pragma warning(push)
#pragma warning(disable : 4005)     // macros both versions define with different values: Direct3D 9's win
namespace d9
{
#include <d3d9.h>
}
#pragma warning(pop)
#pragma pop_macro("DIRECT3D_VERSION")
