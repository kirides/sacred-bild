#pragma once

// Runs in gameserver.exe, which sacred.exe starts with SacredBild inside (see LanClient). The gameserver announces
// its game with a limited broadcast, which Windows sends through one adapter only and VPNs without broadcasts
// never carry, and puts the address of one of the first three adapters Sacred finds into it. Here each
// announcement goes out on every adapter with that adapter's own address, and to the SacredBild players that
// subscribed at the relay port ([Net] Port, SacredBild's UDP endpoint). Each announcement is also published at the
// matchmaker ([Net] Matchmaker).
namespace LanServer
{
    // Patches the gameserver's send() import (used for the announcements only); call after the build check, before
    // the endpoint opens.
    void install();
}
