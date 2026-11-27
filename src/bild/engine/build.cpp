#include "build.h"
#include "controller/click_walk.h"
#include "display/zoom.h"
#include "controller/controller.h"
#include "diag/d3d_stats.h"
#include "perf/fast_math.h"
#include "perf/fast_inflate.h"
#include "render/font_shadow.h"
#include "perf/read_cache.h"
#include "perf/file_cache.h"
#include "diag/file_io_stats.h"
#include "sacred_addr.h"
#include "display/focus.h"
#include "display/window_fit.h"
#include "render/frame_hooks.h"
#include "game/movie.h"
#include "controller/options_screen.h"
#include "render/gpu_skin.h"
#include "render/granny_async.h"
#include "render/granny_parallel.h"
#include "render/ground_mesh.h"
#include "render/ground_quads.h"
#include "game/language.h"
#include "diag/layout_check.h"
#include "perf/map_cache.h"
#include "display/resolution.h"
#include "game/screenshot.h"
#include "diag/skin_check.h"
#include "perf/sound_lock.h"
#include "render/sprite_quads.h"
#include "render/world_statics.h"
#include "ui/ui_anchor.h"
#include "ui/ui_windows.h"
#include "ui/ui_canvas.h"
#include "game/pets.h"
#include "diag/world_passes.h"
#include "net/connection.h"
#include "net/lan_client.h"
#include "net/matchmaker.h"
#include "net/tincat_shim.h"
#include "net/udp_transport.h"
#include "overlay/overlay.h"
#include "settings/config/debug.h"
#include "log.h"
#include "patch.h"
#include "sig.h"

#include <windows.h>
#include <cstdint>
#include <iterator>

namespace
{
#include "sacred_sigs.inc"

    // Builds the signatures were checked against (tools/sigs/gen_sigs.py); others work if their code is the same.
    struct KnownBuild
    {
        uint32_t timestamp;
        const char* name;
    };
    constexpr KnownBuild kKnownBuilds[] = {
        {0x451BBE74, "sacred.exe, German"},
        {0x452F85C7, "Sacred.exe, English (GOG; Steam: the same exe wrapped by SteamStub)"},
        {0x451BBDBF, "gameserver.exe, German"},
        {0x45462A42, "SacredReborn Gameserver.exe"},
        {0x452F8580, "GameServer.exe, English (GOG, Steam)"},
    };

    // ReBorn's jump from the object culling code into the code it appended to the exe (what makes it handle the
    // screen size itself); the German and English builds do not have it.
    constexpr char kReBornMarker[] = "0F BF 54 24 68 E9 ?? ?? ?? ?? 90 90 90 90 90 90 03 D7 89 7C 24 60";
    bool g_reBorn = false;

    bool findReBorn()
    {
        uintptr_t found = 0;
        const Sig::Entry marker[] = {{"ReBorn marker (only in ReBorn)", kReBornMarker, 0, Sig::Take::Match, &found}};
        return Sig::resolve(marker) == 0;
    }
}

bool Sacred::reBorn() { return g_reBorn; }

void Sacred::logHostExe()
{
    const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto ts = nt->FileHeader.TimeDateStamp;
    const char* name = "unknown build";
    for (const KnownBuild& b : kKnownBuilds)
    {
        if (b.timestamp == ts)
        {
            name = b.name;
        }
    }
    if (g_reBorn)
    {
        name = "SacredReborn.exe, Sacred ReBorn (the German build, patched)";
    }
    LOG("Host exe: timestamp={:08x} sizeOfImage={:08x} ({})", ts, nt->OptionalHeader.SizeOfImage, name);
}

bool Sacred::resolveAddresses()
{
    g_reBorn = findReBorn();
    logHostExe();
    size_t missing = Sig::resolve(kAddressSigs);
    if (g_reBorn)
    {
        LOG("Sacred ReBorn: it patched the resolution code itself and loads the language files itself");
    }
    else
    {
        missing += Sig::resolve(kOptionalAddressSigs);
    }
    if (missing)
    {
        LOG("{} of {} game addresses not found", missing, std::size(kAddressSigs));
    }
    Layout::check();
    return missing == 0;
}

void Sacred::installHooks()
{
    D3DStats::setTiming(Config::debug.d3dStats);
    FileCache::install();
    GrannyAsync::install();
    GroundMesh::install();     // its own transaction first: its renderTileRow hook runs inside Resolution's
    WorldStatics::installWater();   // likewise: Resolution's drawWaterTiles hook runs around it
    FastMath::install();
    Patch::begin();
    FrameHooks::install();
    Resolution::install();
    Focus::install();
    WindowFit::install();
    UiCanvas::install();
    UiWindows::install();
    UiAnchor::install();
    Overlay::install();
    Controller::install();
    ClickWalk::install();
    Zoom::install();
    OptionsScreen::install();
    Movie::install();
    FastInflate::install();
    FontShadow::install();
    Screenshot::install();
    MapCache::install();
    SoundLock::install();
    GroundQuads::install();
    SpriteQuads::install();
    WorldStatics::install();
    GpuSkin::install();
    GrannyParallel::install();
    SkinCheck::install();
    Language::install();
    if (Patch::commit())
    {
        LOG("Game hooks installed");
    }
    Pets::install();
    if (Config::debug.d3dStats)
    {
        WorldPasses::install();
        FileIoStats::install();
    }
    ReadCache::install();       // after FileIoStats: in front of its hooks, which then time only what reaches Windows
    Connection::install(false);
    UdpTransport::install(false);
    Matchmaker::install(false);
    TincatShim::install(false);
    LanClient::install();
}
