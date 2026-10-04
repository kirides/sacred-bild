#pragma once

// sacred.exe side of LAN games over VPNs:
// - CreateProcessA: the gameserver Sacred starts for a game it hosts gets SacredBild injected (LanServer),
//   unless [Net] Relay=0.
// - recvfrom / __WSAFDIsSet, used only by the LAN list's poll (cGCclass_pollLanGames): while the list is open,
//   SacredBild subscribes at the hosts in [Net] Hosts and hands their announcements to the poll as if they had
//   arrived on the game's socket, with the host's address as the one to connect to. The copies this PC's own
//   relay sends on its other adapters are dropped, so a host sees its own game once.
namespace LanClient
{
    void install();
}
