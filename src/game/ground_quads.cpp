#include "game/ground_quads.h"
#include "game/device_proxy.h"
#include "config/render.h"
#include "log.h"
#include "patch.h"
#include "sacred/render.h"

#include <cstdint>

namespace
{
    using namespace Sacred;

    decltype(Addr::cQuadBatcher_flush)::Ptr g_origFlush = nullptr;

    // The texture manager's surface for a handle, as the game's flush looks it up.
    IDirectDrawSurface7* lookup(uint32_t handle)
    {
        cTexture* texture = cTextureManager::instance()->get(handle);
        return texture ? texture->surface : nullptr;
    }

    void __fastcall hookFlush(cQuadBatcher* self, void* edx, IDirect3DDevice7* device)
    {
        const uint32_t vertCount = self->vertCount;
        DeviceProxy* proxy = DeviceProxy::instance();
        if (vertCount == 0 || !proxy || device != proxy)
        {
            g_origFlush(self, edx, device);
            return;
        }
        proxy->drawQuads(&lookup, self->texture0, self->texture1, cQuadBatcher::fvf, self->vertices, vertCount,
            self->indices, self->indexCount);
        // The game's own bookkeeping (it counts vertices / 3 as triangles).
        self->indexCount = 0;
        ++self->flushes;
        self->triangles += vertCount / 3;
        self->vertCount = 0;
    }
}

void GroundQuads::install()
{
    if (!Config::render.batch || !Config::render.batchGround || !Addr::cTextureManager_get || !Addr::g_pTextureManager)
    {
        return;
    }
    if (Patch::hook(g_origFlush, Addr::cQuadBatcher_flush, &hookFlush, "cQuadBatcher::flush"))
    {
        LOG("Ground: quad batcher flushes go to the batcher directly");
    }
}
