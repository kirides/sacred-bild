#include "layout_check.h"
#include "engine/sacred_addr.h"
#include "sacred/world.h"
#include "log.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>

using namespace Sacred;

namespace
{
    // How far into a function its uses of a field are looked for.
    constexpr size_t kSpan = 0x800;

    struct Field
    {
        const char* name;
        uint32_t offset;
        uintptr_t (*function)();    // a game function that accesses the field (all builds: at the same distance)
        const char* functionName;
    };

    uintptr_t renderTileRow() { return static_cast<uintptr_t>(Addr::cWorldView_renderTileRow); }
    uintptr_t drawTileLayers() { return static_cast<uintptr_t>(Addr::cWorldView_drawTileLayers); }
    uintptr_t drawWaterTiles() { return static_cast<uintptr_t>(Addr::cWorldView_drawWaterTiles); }
    uintptr_t initRowWalk() { return static_cast<uintptr_t>(Addr::cWorldView_initRowWalk); }
    uintptr_t worldPick() { return static_cast<uintptr_t>(Addr::worldPick); }
    uintptr_t setZoom() { return static_cast<uintptr_t>(Addr::cWorldView_setZoom); }

    constexpr Field kRows[] = {
        {"layeredTileCount", offsetof(cWorldView, layeredTileCount), drawTileLayers, "drawTileLayers"},
        {"waterTileCount", offsetof(cWorldView, waterTileCount), drawWaterTiles, "drawWaterTiles"},
        {"quadBatcher", offsetof(cWorldView, quadBatcher), renderTileRow, "renderTileRow"},
        {"rowEdgeLeft", offsetof(cWorldView, rowEdgeLeft), renderTileRow, "renderTileRow"},
        {"rowEdgeRight", offsetof(cWorldView, rowEdgeRight), renderTileRow, "renderTileRow"},
        {"rowLength", offsetof(cWorldView, rowLength), renderTileRow, "renderTileRow"},
        {"sectors", offsetof(cWorldView, sectors), renderTileRow, "renderTileRow"},
        {"rowEven", offsetof(cWorldView, rowEven), initRowWalk, "initRowWalk"},
        {"rowOdd", offsetof(cWorldView, rowOdd), initRowWalk, "initRowWalk"},
    };
    constexpr Field kPick[] = {
        {"pickLock", offsetof(cWorldView, pickLock), worldPick, "worldPick"},
        {"pickBegin", offsetof(cWorldView, pickBegin), worldPick, "worldPick"},
        {"pickEnd", offsetof(cWorldView, pickEnd), worldPick, "worldPick"},
    };
    constexpr Field kZoom[] = {
        {"zoom", offsetof(cWorldView, zoom), setZoom, "setZoom"},
    };

    bool g_rows = false, g_pick = false, g_zoom = false;

    bool usedIn(uintptr_t function, uint32_t displacement)
    {
        const auto* code = reinterpret_cast<const uint8_t*>(function);
        for (size_t i = 0; i + sizeof(displacement) <= kSpan; ++i)
        {
            if (std::memcmp(code + i, &displacement, sizeof(displacement)) == 0)
            {
                return true;
            }
        }
        return false;
    }

    template <size_t N>
    bool checkFields(const char* group, const Field (&fields)[N])
    {
        bool ok = true;
        for (const Field& f : fields)
        {
            const uintptr_t function = f.function();
            if (!function)
            {
                LOG("World view layout ({}): {} not checked, {} not found", group, f.name, f.functionName);
                ok = false;
            }
            else if (!usedIn(function, f.offset))
            {
                LOG("World view layout ({}): {} is not at +{:x} (the game's {} does not use that offset); the features that need it stay off",
                    group, f.name, f.offset, f.functionName);
                ok = false;
            }
        }
        return ok;
    }
}

void Layout::check()
{
    g_rows = checkFields("ground rows", kRows);
    g_pick = checkFields("pick list", kPick);
    g_zoom = checkFields("zoom", kZoom);
    if (g_rows && g_pick && g_zoom)
    {
        LOG("World view layout: as expected");
    }
}

bool Layout::worldRows() { return g_rows; }
bool Layout::worldPick() { return g_pick; }
bool Layout::worldZoom() { return g_zoom; }
