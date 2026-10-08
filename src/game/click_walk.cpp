#include "game/click_walk.h"
#include "config/debug.h"
#include "log.h"
#include "patch.h"
#include "sacred/engine.h"
#include "sacred/object.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace
{
    using namespace Sacred;

    decltype(Addr::cEngine_sendOrder)::Ptr g_origSendOrder = nullptr;

    constexpr double kTileSize = 53.66563;      // world units per tile (the world -> tile conversion, ENG 006224E0)
    constexpr double kMaxTiles = 30.0;          // inside the path finders' +-32 tile grid, a margin for rounding

    void __fastcall hookSendOrder(cEngine* engine, void* edx, cCreature* creature, cOrder* order)
    {
        cObjectManager* objects = cObjectManager::instance();
        if (order && creature && order->type == cOrder::move && order->mode == cOrder::walk && !order->follow &&
            objects && objects->hero() == creature)
        {
            const double dx = static_cast<double>(order->x) - creature->x;
            const double dy = static_cast<double>(order->y) - creature->y;
            const double tiles = std::max(std::abs(dx), std::abs(dy)) / kTileSize;
            if (tiles > kMaxTiles)
            {
                const double scale = kMaxTiles / tiles;
                const int32_t x = creature->x + static_cast<int32_t>(std::lround(dx * scale));
                const int32_t y = creature->y + static_cast<int32_t>(std::lround(dy * scale));
                if (Config::debug.uiTrace)
                {
                    LOG("Click walk: target {},{} is {:.0f} tiles out, walking to {},{} instead", order->x, order->y,
                        tiles, x, y);
                }
                order->x = x;
                order->y = y;
            }
        }
        g_origSendOrder(engine, edx, creature, order);
    }
}

void ClickWalk::install()
{
    if (!Addr::cObjectManager_hero || !Addr::g_pObjectManager)
    {
        return;
    }
    Patch::hook(g_origSendOrder, Addr::cEngine_sendOrder, &hookSendOrder, "cEngine::sendOrder");
}
