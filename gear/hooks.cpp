#include "hooks.h"
#include "chat.h"
#include "classes.h"
#include "itemtip.h"
#include "itemuse.h"
#include "killstat.h"
#include "drops.h"
#include "lua_host.h"
#include "netmsg.h"
#include "buffs.h"
#include "party.h"
#include "patch.h"
#include "pickup.h"
#include "potionpick.h"
#include "realm.h"
#include "save.h"
#include "savegames.h"
#include "sitehook.h"
#include "master.h"
#include "ui.h"
#include "world.h"
#include "game/gear_addr.h"

namespace
{
    decltype(Gear::Addr::cEngine_updateZoom)::Ptr g_updateZoom = nullptr;

    // The game's update first, so handlers see this frame's state; then the mods, before the world view draws.
    void __fastcall hookUpdateZoom(void* engine, void* edx, float dt)
    {
        g_updateZoom(engine, edx, dt);
        Ui::setEngine(engine);
        Save::frame(engine);
        ItemTip::frame();
        Drops::frame();
        Party::frame();
        Buffs::frame();
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
    Realm::install(LuaHost::stateFilePath(L"gear_realm.txt"));
    Drops::install();
    World::installCreateHook();
    NetMsg::install();
    Chat::install();
    Party::install();
    Buffs::install();
    Classes::install();
    Master::install();
    Savegames::install();
    ItemTip::install();
    ItemUse::install();
    KillStat::install();
    PotionPick::install();
    const bool committed = Patch::commit();
    // Site hooks write into the middle of functions (gear/sitehook.cpp); they are optional like mod drawing.
    SiteHook::install();
    Drops::installSites();
    return committed && ok;
}
