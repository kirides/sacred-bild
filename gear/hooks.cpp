#include "hooks.h"
#include "lua_host.h"
#include "patch.h"
#include "pickup.h"
#include "ui.h"
#include "game/gear_addr.h"

namespace
{
    decltype(Gear::Addr::cEngine_updateZoom)::Ptr g_updateZoom = nullptr;

    // The game's update first, so handlers see this frame's state; then the mods, before the world view draws.
    void __fastcall hookUpdateZoom(void* engine, void* edx, float dt)
    {
        g_updateZoom(engine, edx, dt);
        LuaHost::frame(dt);
    }
}

bool Hooks::install()
{
    Patch::begin();
    const bool ok = Patch::hook(g_updateZoom, Gear::Addr::cEngine_updateZoom, hookUpdateZoom, "cEngine_updateZoom");
    // Mod drawing is optional: without it the mods still run.
    Ui::install();
    Pickup::install();
    return Patch::commit() && ok;
}
