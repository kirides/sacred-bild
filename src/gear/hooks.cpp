#include "hooks.h"
#include "prof.h"
#include "chat.h"
#include "world/classes.h"
#include "items/itemtip.h"
#include "items/itemuse.h"
#include "items/itemtexture.h"
#include "world/killstat.h"
#include "items/drops.h"
#include "lua_host.h"
#include "netmsg.h"
#include "world/buffs.h"
#include "world/party.h"
#include "patch.h"
#include "world/pfx.h"
#include "items/pickup.h"
#include "items/potionpick.h"
#include "realm.h"
#include "save.h"
#include "savegames.h"
#include "items/shop.h"
#include "sitehook.h"
#include "items/master.h"
#include "ui/ui.h"
#include "world/world.h"
#include "engine/gear_addr.h"

namespace
{
    decltype(Gear::Addr::cEngine_updateZoom)::Ptr g_updateZoom = nullptr;

    // The game's update first, so handlers see this frame's state; then the mods, before the world view draws.
    void __fastcall hookUpdateZoom(void* engine, void* edx, float dt)
    {
        {
            GearProf::Scope s(GearProf::GameUpdate);
            g_updateZoom(engine, edx, dt);
        }
        Ui::setEngine(engine);
        { GearProf::Scope s(GearProf::Save); Save::frame(engine); }
        { GearProf::Scope s(GearProf::ItemTip); ItemTip::frame(); }
        { GearProf::Scope s(GearProf::Drops); Drops::frame(); }
        { GearProf::Scope s(GearProf::Party); Party::frame(); }
        { GearProf::Scope s(GearProf::Buffs); Buffs::frame(); }
        {
            GearProf::Scope s(GearProf::LuaFrame);
            LuaHost::frame(dt);
        }
        GearProf::report();
    }
}

bool Hooks::install()
{
    GearProf::init();
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
    ItemTexture::install();
    KillStat::install();
    PotionPick::install();
    Shop::install();
    Pfx::install();
    const bool committed = Patch::commit();
    // Site hooks write into the middle of functions (src/gear/sitehook.cpp); they are optional like mod drawing.
    SiteHook::install();
    Drops::installSites();
    return committed && ok;
}
