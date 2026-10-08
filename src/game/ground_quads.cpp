#include "game/ground_quads.h"
#include "game/device_proxy.h"
#include "game/sacred_addr.h"
#include "config/render.h"
#include "log.h"
#include "mem.h"
#include "patch.h"

#include <cstdint>

namespace
{
    using namespace Sacred;

    // thiscall targets are hooked/called as fastcall with an unused EDX parameter.
    using FlushFn = void(__fastcall*)(uint8_t* self, void* edx, IDirect3DDevice7* device);
    using TextureGetFn = uint8_t*(__fastcall*)(void* manager, void* edx, uint32_t handle, int flags);

    FlushFn g_origFlush = nullptr;
    TextureGetFn g_textureGet = nullptr;

    using Mem::member;

    // The texture manager's surface for a handle, as the game's flush looks it up.
    IDirectDrawSurface7* lookup(uint32_t handle)
    {
        void* manager = *reinterpret_cast<void**>(Addr::g_pTextureManager);
        uint8_t* texture = g_textureGet(manager, nullptr, handle, 0);
        return texture ? member<IDirectDrawSurface7*>(texture, 0x14) : nullptr;
    }

    void __fastcall hookFlush(uint8_t* self, void* edx, IDirect3DDevice7* device)
    {
        const uint32_t vertCount = member<uint32_t>(self, QuadBatcher::vertCount);
        DeviceProxy* proxy = DeviceProxy::instance();
        if (vertCount == 0 || !proxy || device != proxy)
        {
            g_origFlush(self, edx, device);
            return;
        }
        proxy->drawQuads(&lookup, member<uint32_t>(self, QuadBatcher::texture0), member<uint32_t>(self, QuadBatcher::texture1),
            QuadBatcher::fvf, self, vertCount, reinterpret_cast<const WORD*>(self + QuadBatcher::indices),
            member<uint32_t>(self, QuadBatcher::indexCount));
        // The game's own bookkeeping (it counts vertices / 3 as triangles).
        member<uint32_t>(self, QuadBatcher::indexCount) = 0;
        ++member<uint32_t>(self, QuadBatcher::flushes);
        member<uint32_t>(self, QuadBatcher::triangles) += vertCount / 3;
        member<uint32_t>(self, QuadBatcher::vertCount) = 0;
    }
}

void GroundQuads::install()
{
    if (!Config::render.batch || !Config::render.batchGround || !Addr::cTextureManager_get || !Addr::g_pTextureManager)
    {
        return;
    }
    g_textureGet = Addr::cTextureManager_get.ptr();
    if (Patch::hook(g_origFlush, Addr::cQuadBatcher_flush, &hookFlush, "cQuadBatcher::flush"))
    {
        LOG("Ground: quad batcher flushes go to the batcher directly");
    }
}
