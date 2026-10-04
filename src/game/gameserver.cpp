#include "game/gameserver.h"
#include "game/build.h"
#include "game/gameserver_addr.h"
#include "net/lan_server.h"
#include "config.h"
#include "log.h"
#include "patch.h"
#include "sig.h"

#include <windows.h>
#include <algorithm>

namespace
{
#include "game/gameserver_sigs.inc"

    constexpr uint32_t kGameFirstContactMs = 5000;

    // A player that connected has 5 s to send its first message, or the watchdog drops the connection: tight for
    // slow or distant links, where joins then fail with a connect time-out.
    void patchJoinTimeout()
    {
        const uint32_t ms = static_cast<uint32_t>(std::max(g_config.netJoinTimeout, 5)) * 1000;
        if (ms != kGameFirstContactMs && Patch::imm32(GameServer::Addr::firstContactTimeoutImm, kGameFirstContactMs, ms))
        {
            LOG("Join time-out: {} s (game: 5 s)", ms / 1000);
        }
    }
}

bool GameServer::isHostProcess()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    return _wcsicmp(name ? name + 1 : path, L"gameserver.exe") == 0;
}

void GameServer::installHooks()
{
    Sacred::logHostExe();
    if (Sig::resolve(kAddressSigs) != 0)
    {
        LOG("Unsupported gameserver.exe build: no patches");
        return;
    }
    patchJoinTimeout();
    LanServer::install();
}
