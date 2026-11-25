#pragma once

// gameserver.exe, which sacred.exe starts with SacredBild injected for the games it hosts (see LanClient).
namespace GameServer
{
    // True when the host process is gameserver.exe.
    bool isHostProcess();

    // Checks the build and installs the gameserver patches (LAN relay, join time-out); call once from DllMain.
    void installHooks();
}
