#pragma once

// The game messages this client sends, seen through the send queue (cNet_queueMessage):
// - kills: a hero's kill is sent as 0x127 (experience award: hero id, amount) and, within the same moment, 0x17D
//   (creature death: victim id); NetMsg::takeKill hands them to the Lua host as the event `kill`;
// - trace: with the environment variable GEAR_NETTRACE=1 the first few messages of every id are logged (id, size,
//   sender slot, start of the payload), to map messages (docs/GearServerMods.md).
// The hook only looks at the messages; the game's call goes on unchanged.
// - the mod channel (gear.mp.send): frames of UTF-16 units sent as private chat (0x003) to a slot, and the frames
//   other clients sent this one, taken out of the receive dispatcher's chat case before the chat box sees them. A
//   frame starts with the marker unit kFrameMarker; any other private chat goes on to the chat as before.
#include <cstdint>
#include <string>
#include <vector>

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

    constexpr uint16_t kFrameMarker = 0xE1A0;
    constexpr size_t kMaxFrameUnits = 127;      // the 254 bytes of text the game server relays

    // Hero source: the game server delivered a character block (message 0x132) in this network game, i.e. the hero is the
    // server's (Closed Net). heroSourceKnown: the receive hook is in (without it nothing can be proven: callers must not
    // assume Open Net). Cleared when the network game ends.
    bool heroSourceKnown();
    bool heroFromServer();
    void clearHeroFromServer();
    void setHeroFromServerForTest(bool on);

    // Whether frames can be sent and received (the hooks are in, a connection is up).
    bool channelReady();
    // Sends one frame (1 to 127 units, none of them 0) to `slot` (1 to 16) as private chat. Engine thread.
    bool sendFrame(uint32_t slot, const uint16_t* units, size_t count);
    // Self tests (gear.auto.chat_send): a chat message of 	ype (3 private, 4 team, 5 everyone) with the target slot
    // and the text, laid out as the chat sender 0x7D4510 does ([u32 target][the units][a NUL unit]).
    bool sendChatForTest(uint16_t type, uint32_t target, const uint16_t* units, size_t count);
    // The oldest frame received and not taken yet: the sender's slot (0: the server) and its units.
    bool takeFrame(uint32_t& slot, std::vector<uint16_t>& units);

    // Custom opcodes: two message types above the stock range (the stock dispatcher's table ends at 0x1D5; anything
    // above goes to its default case, which only moves on to the next message). The receive side hands frames of
    // these two types to takeOp; the send side queues one message of such a type like any other game message. The
    // ids are configurable (setOpcodes before the first send, or GEAR_NETOPS="0x1E0,0x1E1" in the environment) so
    // that servers with their own custom ids can be matched.
    constexpr uint16_t kStockMax = 0x1D5;
    constexpr uint16_t kOpMax = 0x400;
    constexpr uint16_t kDefaultControl = 0x1E0, kDefaultData = 0x1E1;
    constexpr size_t kMaxOpPayload = 4096 + 16;     // what one message may carry (the data frame header and 4000 bytes)

    struct OpFrame
    {
        uint32_t sender = 0;            // the header's sender slot (0: the server)
        uint16_t type = 0;
        std::vector<uint8_t> data;      // the message payload
    };
    // Both hooks are in and a connection is up.
    bool opsReady();
    bool setOpcodes(uint16_t control, uint16_t data);
    uint16_t controlOpcode();
    uint16_t dataOpcode();
    bool sendOp(uint16_t type, const void* data, size_t size);
    bool takeOp(OpFrame& out);

    // The players (the object at g_pNetPlayers, ENG 0x7D7A70: +0x01 "this computer hosts", +0x14 the local slot,
    // +0x54 17 roster records of 0x342 bytes by slot: +0x00 occupied, +0x07 host, +0x16 the player name, +0x11E the
    // hero's name, both UTF-16; +0x172 the local hero's name from its save preview).
    struct Host
    {
        bool network = false;   // a LAN / Open Net / Closed Net game (the flag at 0x0182EBEC)
        bool isHost = false;    // this computer hosts it
        uint32_t hostSlot = 0;  // the host's slot, 0 if unknown (the game's 0x7F0C80)
        uint32_t localSlot = 0;
    };
    Host host();
    // The hero's name in `slot` (1 to 16) as UTF-16; empty if the game has none.
    std::wstring heroName(uint32_t slot);

    // A hero file the game wrote (the hook on its hero writer): the save slot from the file name (HeroNN.pax, 0 if
    // the name has none) and whether a network game ran.
    struct HeroSaved
    {
        uint32_t slot = 0;
        bool network = false;
    };
    bool takeHeroSaved(HeroSaved& out);
}
