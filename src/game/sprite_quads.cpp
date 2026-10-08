#include "game/sprite_quads.h"
#include "game/device_proxy.h"
#include "config/render.h"
#include "log.h"
#include "patch.h"
#include "sacred/render.h"

#include <cstdint>

namespace
{
    using namespace Sacred;

    decltype(Addr::cSpriteBatcher_setTexture)::Ptr g_origSetTexture = nullptr;
    decltype(Addr::cSpriteBatcher_flush)::Ptr g_origFlush = nullptr;

    // The texture manager's surface for a handle, as the game's flush looks it up.
    IDirectDrawSurface7* lookup(uint32_t handle)
    {
        cTexture* texture = cTextureManager::instance()->get(handle);
        return texture ? texture->surface : nullptr;
    }

    // The game's flush with the batcher instead of SetTexture + DrawIndexedPrimitive; false if the proxy isn't the
    // device (then the game's own code runs).
    bool flush(cSpriteBatcher* self, IDirect3DDevice7* device)
    {
        DeviceProxy* proxy = DeviceProxy::instance();
        if (!proxy || device != proxy)
        {
            return false;
        }
        const uint32_t vertCount = self->vertCount;
        if (vertCount == 0)
        {
            return true;
        }
        proxy->drawQuads(&lookup, self->texture, 0, cSpriteBatcher::fvf, self->vertices, vertCount, self->indices,
            self->indexCount);
        // The game's own bookkeeping (it counts vertices / 3 as triangles).
        self->indexCount = 0;
        ++self->flushes;
        self->vertCount = 0;
        self->triangles += vertCount / 3;
        return true;
    }

    void __fastcall hookSetTexture(cSpriteBatcher* self, void* edx, IDirect3DDevice7* device, uint32_t handle)
    {
        if (handle == self->texture)
        {
            return;
        }
        if (!flush(self, device))
        {
            g_origSetTexture(self, edx, device, handle);
            return;
        }
        self->texture = handle;
    }

    void __fastcall hookFlush(cSpriteBatcher* self, void* edx, IDirect3DDevice7* device)
    {
        if (!flush(self, device))
        {
            g_origFlush(self, edx, device);
        }
    }
}

void SpriteQuads::install()
{
    if (!Config::render.batch || !Config::render.batchSprites || !Addr::cTextureManager_get || !Addr::g_pTextureManager)
    {
        return;
    }
    if (Patch::hook(g_origSetTexture, Addr::cSpriteBatcher_setTexture, &hookSetTexture, "cSpriteBatcher::setTexture") &&
        Patch::hook(g_origFlush, Addr::cSpriteBatcher_flush, &hookFlush, "cSpriteBatcher::flush"))
    {
        LOG("Sprites: sprite batcher flushes go to the batcher directly");
    }
}
