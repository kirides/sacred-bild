#pragma once
#include <windows.h>

namespace SystemDdraw
{
    // Backend=chain on Windows' own ddraw.dll: wraps its DirectDrawCreateEx so the Direct3D 7 objects created from it
    // accept render targets larger than 2048 pixels. Returns the function to export instead.
    FARPROC wrap(FARPROC directDrawCreateEx);
}
