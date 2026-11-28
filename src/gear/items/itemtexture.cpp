#include "itemtexture.h"
#include "engine/gear_addr.h"
#include "patch.h"
#include "world/world.h"

#include <windows.h>
#include <cstdint>

namespace
{
    using namespace Gear;

    decltype(Addr::modelRenderer_drawItem)::Ptr g_orig = nullptr;

    void __fastcall hookDrawItem(void* self, void* edx, void* device, uint32_t model0, uint32_t model1, int x, int y, uint32_t type, float scale, int a8,
                                 uint32_t flags, int a10, int a11)
    {
        World::iconBegin(type);
        __try
        {
            g_orig(self, edx, device, model0, model1, x, y, type, scale, a8, flags, a10, a11);
        }
        __finally
        {
            World::iconEnd();
        }
    }
}

bool ItemTexture::install()
{
    if (!Addr::modelRenderer_drawItem)
    {
        return false;
    }
    return Patch::hook(g_orig, Addr::modelRenderer_drawItem, hookDrawItem, "modelRenderer_drawItem");
}
