#pragma once

// Net trace: a diagnostic that logs the game messages the client sends (message id, size, sender slot, the start of
// the payload), the first few of every id, so that the ids of kills, chat and loot can be read off a play session.
// Off unless the environment variable GEAR_NETTRACE is set to 1. Used to map the multiplayer messages for gear.mp.
namespace NetMsg
{
    // Installs the hook if GEAR_NETTRACE=1 and the send queue was found; true if nothing was asked or it worked.
    bool install();
}
