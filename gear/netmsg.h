#pragma once

// The game messages this client sends, seen through the send queue (cNet_queueMessage):
// - kills: a hero's kill is sent as 0x127 (experience award: hero id, amount) and, within the same moment, 0x17D
//   (creature death: victim id); NetMsg::takeKill hands them to the Lua host as the event `kill`;
// - trace: with the environment variable GEAR_NETTRACE=1 the first few messages of every id are logged (id, size,
//   sender slot, start of the payload), to map messages (docs/GearServerMods.md).
// The hook only looks at the messages; the game's call goes on unchanged.
#include <cstdint>

namespace NetMsg
{
    // False if the send queue was not found (no kill events then).
    bool install();

    struct Kill
    {
        uint32_t victim = 0;    // object id of the creature that died (read its type soon: it is retired later)
        uint32_t killer = 0;    // hero object id the experience was awarded to, 0 if none (killed by a creature)
        uint32_t award = 0;     // experience
    };

    // The oldest kill not taken yet. Engine thread (the Lua host's).
    bool takeKill(Kill& out);
}
