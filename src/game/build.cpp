#include "game/build.h"
#include "game/controller.h"
#include "game/d3d_stats.h"
#include "game/sacred_addr.h"
#include "game/focus.h"
#include "game/frame_hooks.h"
#include "game/movie.h"
#include "game/options_screen.h"
#include "game/gpu_skin.h"
#include "game/granny_async.h"
#include "game/granny_parallel.h"
#include "game/ground_mesh.h"
#include "game/ground_quads.h"
#include "game/language.h"
#include "game/map_cache.h"
#include "game/resolution.h"
#include "game/screenshot.h"
#include "game/skin_check.h"
#include "game/ui_anchor.h"
#include "game/ui_canvas.h"
#include "game/world_passes.h"
#include "net/connection.h"
#include "net/lan_client.h"
#include "net/matchmaker.h"
#include "net/tincat_shim.h"
#include "net/udp_transport.h"
#include "overlay/overlay.h"
#include "config.h"
#include "log.h"
#include "patch.h"
#include "sig.h"

#include <windows.h>
#include <cstdint>
#include <iterator>

namespace
{
#include "game/sacred_sigs.inc"

    // Builds the signatures were checked against (tools/gen_sigs.py); others work if their code is the same.
    struct KnownBuild
    {
        uint32_t timestamp;
        const char* name;
    };
    constexpr KnownBuild kKnownBuilds[] = {
        {0x451BBE74, "sacred.exe, German"},
        {0x452F85C7, "Sacred.exe, English (GOG; Steam: the same exe wrapped by SteamStub)"},
        {0x451BBDBF, "gameserver.exe, German"},
        {0x452F8580, "GameServer.exe, English (GOG, Steam)"},
    };
}

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
    LOG("Host exe: timestamp={:08x} sizeOfImage={:08x} ({})", ts, nt->OptionalHeader.SizeOfImage, name);
}

bool Sacred::resolveAddresses()
{
    logHostExe();
    const size_t missing = Sig::resolve(kAddressSigs);
    if (missing)
    {
        LOG("{} of {} game addresses not found", missing, std::size(kAddressSigs));
    }
    return missing == 0;
}

void Sacred::installHooks()
{
    D3DStats::setTiming(g_config.d3dStats);
    GrannyAsync::install();
    GroundMesh::install();     // its own transaction first: its renderTileRow hook runs inside Resolution's
    Patch::begin();
    FrameHooks::install();
    Resolution::install();
    Focus::install();
    UiCanvas::install();
    UiAnchor::install();
    Overlay::install();
    Controller::install();
    OptionsScreen::install();
    Movie::install();
    Screenshot::install();
    MapCache::install();
    GroundQuads::install();
    GpuSkin::install();
    GrannyParallel::install();
    SkinCheck::install();
    Language::install();
    if (Patch::commit())
    {
        LOG("Game hooks installed");
    }
    if (g_config.d3dStats)
    {
        WorldPasses::install();
    }
    Connection::install(false);
    UdpTransport::install(false);
    Matchmaker::install(false);
    TincatShim::install(false);
    LanClient::install();
}
