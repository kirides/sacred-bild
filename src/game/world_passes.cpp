#include "game/world_passes.h"
#include "game/d3d_stats.h"
#include "game/sacred_addr.h"
#include "log.h"
#include "patch.h"

#include <cstdint>

namespace
{
    using namespace Sacred;
    using D3DStats::Pass;
    using D3DStats::PassScope;

    // thiscall targets are hooked as fastcall with an unused EDX parameter.
    using DeviceFn = void(__fastcall*)(void* self, void* edx, void* device);
    using TileRowFn = void(__fastcall*)(void* self, void* edx, void* device, void* rowPos, int detail);
    using DrawModelFn = uint32_t(__fastcall*)(void* self, void* edx, void* device, void* model, void* instance,
        uint32_t flagsLow, uint32_t flagsHigh);

    TileRowFn g_origTileRow = nullptr;
    DeviceFn g_origFlush = nullptr;
    DeviceFn g_origLayers = nullptr;
    DeviceFn g_origWater = nullptr;
    DeviceFn g_origObjects = nullptr;
    DeviceFn g_origObjects2 = nullptr;
    DrawModelFn g_origDrawModel = nullptr;

    void __fastcall hookTileRow(void* self, void* edx, void* device, void* rowPos, int detail)
    {
        PassScope s{D3DStats::PRows};
        g_origTileRow(self, edx, device, rowPos, detail);
    }

    // The layer and water passes flush through the same batcher: those draws stay theirs.
    void __fastcall hookFlush(void* self, void* edx, void* device)
    {
        const Pass pass = D3DStats::currentPass();
        if (pass != D3DStats::PRows && pass != D3DStats::PWorld)
        {
            g_origFlush(self, edx, device);
            return;
        }
        PassScope s{D3DStats::PGround};
        g_origFlush(self, edx, device);
    }

    void __fastcall hookLayers(void* self, void* edx, void* device)
    {
        PassScope s{D3DStats::PLayers};
        g_origLayers(self, edx, device);
    }

    void __fastcall hookWater(void* self, void* edx, void* device)
    {
        PassScope s{D3DStats::PWater};
        g_origWater(self, edx, device);
    }

    void __fastcall hookObjects(void* self, void* edx, void* device)
    {
        PassScope s{D3DStats::PObjects};
        g_origObjects(self, edx, device);
    }

    void __fastcall hookObjects2(void* self, void* edx, void* device)
    {
        PassScope s{D3DStats::PObjects2};
        g_origObjects2(self, edx, device);
    }

    // Returns what the game's function left in EAX, in case a caller reads it.
    uint32_t __fastcall hookDrawModel(void* self, void* edx, void* device, void* model, void* instance, uint32_t flagsLow,
        uint32_t flagsHigh)
    {
        PassScope s{D3DStats::PModels};
        return g_origDrawModel(self, edx, device, model, instance, flagsLow, flagsHigh);
    }
}

void WorldPasses::install()
{
    Patch::begin();
    Patch::hook(g_origTileRow, Addr::cWorldView_renderTileRow, &hookTileRow, "cWorldView::renderTileRow (passes)");
    Patch::hook(g_origFlush, Addr::cQuadBatcher_flush, &hookFlush, "cQuadBatcher::flush");
    Patch::hook(g_origLayers, Addr::cWorldView_drawTileLayers, &hookLayers, "cWorldView::drawTileLayers");
    Patch::hook(g_origWater, Addr::cWorldView_drawWaterTiles, &hookWater, "cWorldView::drawWaterTiles");
    Patch::hook(g_origObjects, Addr::cWorldView_drawObjects, &hookObjects, "cWorldView::drawObjects");
    Patch::hook(g_origObjects2, Addr::cWorldView_drawObjects2, &hookObjects2, "cWorldView::drawObjects2");
    Patch::hook(g_origDrawModel, Addr::cObject3D_drawModel, &hookDrawModel, "cObject3D::drawModel");
    if (Patch::commit())
    {
        LOG("World passes: timed for D3DStats");
    }
}
