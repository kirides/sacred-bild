#include "netmsg.h"
#include "log.h"
#include "patch.h"
#include "game/gear_addr.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>

namespace
{
    constexpr uint32_t kIds = 0x400;
    constexpr int kLogPerId = 3;

    std::atomic<uint32_t> g_count[kIds];
    decltype(Gear::Addr::cNet_queueMessage)::Ptr g_orig = nullptr;
    bool g_trace = false;

    constexpr uint16_t kAward = 0x127;      // {hero id, experience}
    constexpr uint16_t kDeath = 0x17D;      // {victim id, 1, 1}
    constexpr DWORD kPairMs = 250;          // an award belongs to the death that follows within this time

    std::mutex g_lock;
    std::deque<NetMsg::Kill> g_kills;
    uint32_t g_awardHero = 0, g_awardAmount = 0;
    DWORD g_awardAt = 0;

    void note(uint16_t id, const uint8_t* payload, uint32_t size)
    {
        if (id != kAward && id != kDeath)
        {
            return;
        }
        uint32_t a = 0, b = 0;
        if (size >= 4)
        {
            std::memcpy(&a, payload, 4);
        }
        if (size >= 8)
        {
            std::memcpy(&b, payload + 4, 4);
        }
        std::lock_guard lock(g_lock);
        const DWORD now = GetTickCount();
        if (id == kAward)
        {
            g_awardHero = a;
            g_awardAmount = b;
            g_awardAt = now;
            return;
        }
        NetMsg::Kill k;
        k.victim = a;
        if (g_awardHero && now - g_awardAt <= kPairMs)
        {
            k.killer = g_awardHero;
            k.award = g_awardAmount;
        }
        g_awardHero = 0;
        if (g_kills.size() < 256)
        {
            g_kills.push_back(k);
        }
    }

    // The mod channel's receive side: the chat case of the dispatcher's jump table points at chatStub, which hands
    // the message to onChat and goes on with the original case, or (for a Gear frame) with the next message.
    uintptr_t g_chatCase = 0;       // ENG 007E0880
    uintptr_t g_nextMessage = 0;    // ENG 007E3CE0
    bool g_channel = false;
    struct Frame
    {
        uint32_t slot;
        std::vector<uint16_t> units;
    };
    std::deque<Frame> g_frames;     // under g_lock

    bool copyFrame(const uint8_t* message, const uint8_t* payload, uint32_t size, uint16_t* units, size_t& n)
    {
        __try
        {
            if (*reinterpret_cast<const uint16_t*>(message + 0xE) != 3 || size < 6 || size > 0x10000)
            {
                return false;
            }
            const uint16_t* text = reinterpret_cast<const uint16_t*>(payload + 4);
            const size_t max = (size - 4) / 2;
            if (text[0] != NetMsg::kFrameMarker)
            {
                return false;
            }
            while (n < max && n < NetMsg::kMaxFrameUnits && text[n] != 0)
            {
                units[n] = text[n];
                ++n;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return true;
    }

    bool __stdcall onChat(const uint8_t* message, const uint8_t* payload, uint32_t size, uint32_t sender)
    {
        uint16_t units[NetMsg::kMaxFrameUnits];
        size_t n = 0;
        if (!copyFrame(message, payload, size, units, n))
        {
            return false;
        }
        std::lock_guard lock(g_lock);
        if (g_frames.size() < 8192)
        {
            g_frames.push_back({sender, std::vector<uint16_t>(units, units + n)});
        }
        return true;
    }

    // At the dispatcher's `jmp [edx*4 + table]`: [esp+0x2C] the message, ebp its payload, ebx the payload size, esi
    // the sender slot. Every register is kept; the flags are dead there (the case code sets its own).
    __declspec(naked) void chatStub()
    {
        __asm {
            pushad
            push esi
            push ebx
            push ebp
            push dword ptr [esp + 0x2C + 0x20 + 0x0C]
            call onChat
            test al, al
            popad
            jnz skip
            jmp dword ptr [g_chatCase]
        skip:
            jmp dword ptr [g_nextMessage]
        }
    }

    bool installChannel()
    {
        const uintptr_t sw = Gear::Addr::netDispatchSwitch;
        if (!sw || !Gear::Addr::cNet_sendMessage || !Gear::Addr::g_pNet)
        {
            LOG("NetMsg: the receive dispatcher or the message sender was not found: no mod channel");
            return false;
        }
        // cmp ecx, 0x1D4 / ja rel32 / xor edx, edx / mov dl, [ecx + abs32] / jmp [edx*4 + abs32]
        if (!Patch::verify(sw, {0x81, 0xF9, 0xD4, 0x01, 0x00, 0x00, 0x0F, 0x87}) || !Patch::verify(sw + 0xC, {0x33, 0xD2, 0x8A, 0x91}) ||
            !Patch::verify(sw + 0x14, {0xFF, 0x24, 0x95}))
        {
            LOG("NetMsg: the receive dispatcher has unexpected code at {:08X}: no mod channel", sw);
            return false;
        }
        int32_t rel = 0;
        uint32_t indexTable = 0, jumpTable = 0;
        std::memcpy(&rel, reinterpret_cast<const void*>(sw + 8), 4);
        std::memcpy(&indexTable, reinterpret_cast<const void*>(sw + 0x10), 4);
        std::memcpy(&jumpTable, reinterpret_cast<const void*>(sw + 0x17), 4);
        const uint8_t* idx = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(indexTable));
        // types 3, 4 and 5 (the index is type - 1) share the chat case
        if (idx[2] != idx[3] || idx[2] != idx[4])
        {
            LOG("NetMsg: the chat messages do not share a case: no mod channel");
            return false;
        }
        const uintptr_t slot = jumpTable + idx[2] * 4u;
        const uintptr_t chatCase = *reinterpret_cast<const uint32_t*>(slot);
        if (!Patch::verify(chatCase, {0x8D, 0x4C, 0x24, 0x44, 0xE8}))
        {
            LOG("NetMsg: the chat case at {:08X} has unexpected code: no mod channel", chatCase);
            return false;
        }
        g_chatCase = chatCase;
        g_nextMessage = sw + 0xC + rel;
        const uint32_t stub = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&chatStub));
        if (!Patch::value(slot, stub))
        {
            return false;
        }
        LOG("NetMsg: mod channel on (chat case {:08X}, jump table slot {:08X})", chatCase, slot);
        return true;
    }

    decltype(Gear::Addr::heroSave)::Ptr g_heroSave = nullptr;
    std::deque<NetMsg::HeroSaved> g_heroSaves;     // under g_lock

    // The NN of ...HeroNN.pax, 0 if none.
    uint32_t heroSlotOf(const char* path)
    {
        uint32_t slot = 0;
        __try
        {
            for (const char* p = path; p && *p; ++p)
            {
                if ((p[0] == 'H' || p[0] == 'h') && (p[1] == 'e' || p[1] == 'E') && (p[2] == 'r' || p[2] == 'R') &&
                    (p[3] == 'o' || p[3] == 'O') && p[4] >= '0' && p[4] <= '9' && p[5] >= '0' && p[5] <= '9')
                {
                    slot = static_cast<uint32_t>((p[4] - '0') * 10 + (p[5] - '0'));
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            slot = 0;
        }
        return slot;
    }

    uint8_t __cdecl hookHeroSave(const char* path)
    {
        const uint8_t ok = g_heroSave(path);
        if (ok)
        {
            NetMsg::HeroSaved h;
            h.network = NetMsg::host().network;
            h.slot = heroSlotOf(path);
            std::lock_guard lock(g_lock);
            if (g_heroSaves.size() < 64)
            {
                g_heroSaves.push_back(h);
            }
        }
        return ok;
    }

    void trace(const uint32_t* message)
    {
        uint8_t head[16 + 24] = {};
        __try
        {
            std::memcpy(head, message, sizeof(head));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }
        uint32_t word0;
        uint16_t id;
        std::memcpy(&word0, head, 4);
        std::memcpy(&id, head + 0xE, 2);
        const uint32_t size = word0 & 0x3FFFFF;
        const uint32_t slot = word0 >> 27;           // bits 27..31; 0 is the server
        const uint32_t flags = (word0 >> 22) & 0x1F; // bits 22..26
        const uint32_t n = g_count[id % kIds].fetch_add(1) + 1;
        if (n > kLogPerId && n % 2000 != 0)
        {
            return;
        }
        char hex[16 * 3 + 1] = {};
        const uint32_t payload = size > 16 ? (size - 16 > 24 ? 24 : size - 16) : 0;
        for (uint32_t i = 0; i < payload && i < 16; ++i)
        {
            std::snprintf(hex + i * 3, 4, "%02X ", head[16 + i]);
        }
        LOG("NetMsg: id 0x{:X} size {} slot {} flags {} #{} payload {}", id, size, slot, flags, n, hex);
    }

    uint32_t __fastcall hookQueue(void* net, void* edx, uint32_t* message, uint8_t flush)
    {
        if (message)
        {
            trace(message);
        }
        return g_orig(net, edx, message, flush);
    }
}

bool NetMsg::install()
{
    g_channel = installChannel();
    if (Gear::Addr::heroSave)
    {
        Patch::hook(g_heroSave, Gear::Addr::heroSave, hookHeroSave, "heroSave");
    }
    else
    {
        LOG("NetMsg: the hero writer was not found: no game.hero_saved");
    }
    const char* on = std::getenv("GEAR_NETTRACE");
    g_trace = on && on[0] == '1';
    if (!Gear::Addr::cNet_queueMessage)
    {
        LOG("NetMsg: the send queue was not found: no kill events{}", g_trace ? ", no net trace" : "");
        return false;
    }
    if (g_trace)
    {
        LOG("NetMsg: tracing the messages this client sends (GEAR_NETTRACE=1)");
    }
    return Patch::hook(g_orig, Gear::Addr::cNet_queueMessage, hookQueue, "cNet_queueMessage");
}

bool NetMsg::channelReady()
{
    if (!g_channel)
    {
        return false;
    }
    uint8_t up[2] = {};
    __try
    {
        const uint8_t* net = *Gear::Addr::g_pNet.get();
        if (!net)
        {
            return false;
        }
        up[0] = net[0x2A];
        up[1] = net[0x2B];
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return up[0] && up[1];
}

bool NetMsg::sendFrame(uint32_t slot, const uint16_t* units, size_t count)
{
    if (slot < 1 || slot > 16 || count == 0 || count > kMaxFrameUnits || !channelReady())
    {
        return false;
    }
    for (size_t i = 0; i < count; ++i)
    {
        if (units[i] == 0)
        {
            return false;
        }
    }
    // [u32 target][the units][a NUL unit], as the chat sender 0x7D4510 lays it out
    uint8_t data[4 + (kMaxFrameUnits + 1) * 2] = {};
    std::memcpy(data, &slot, 4);
    std::memcpy(data + 4, units, count * 2);
    const uint32_t size = static_cast<uint32_t>(4 + (count + 1) * 2);
    Gear::Addr::cNet_sendMessage(*Gear::Addr::g_pNet.get(), data, size, 3, 0);   // queued; its result says nothing more
    return true;
}

bool NetMsg::takeFrame(uint32_t& slot, std::vector<uint16_t>& units)
{
    std::lock_guard lock(g_lock);
    if (g_frames.empty())
    {
        return false;
    }
    slot = g_frames.front().slot;
    units = std::move(g_frames.front().units);
    g_frames.pop_front();
    return true;
}

namespace
{
    constexpr uint32_t kRoster = 0x54, kRecord = 0x342, kRecordHost = 0x07, kRecordPlayerName = 0x16,
                       kRecordHeroName = 0x11E, kLocalHeroName = 0x172;

    const uint8_t* players()
    {
        const uint8_t* p = nullptr;
        __try
        {
            p = Gear::Addr::g_pNetPlayers ? *Gear::Addr::g_pNetPlayers.get() : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            p = nullptr;
        }
        return p;
    }

    // A NUL-terminated UTF-16 string of at most `max` units at p.
    bool readName(const uint8_t* p, size_t max, wchar_t* out, size_t& n)
    {
        n = 0;
        __try
        {
            const wchar_t* s = reinterpret_cast<const wchar_t*>(p);
            while (n < max && s[n] != 0)
            {
                out[n] = s[n];
                ++n;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            n = 0;
            return false;
        }
        return true;
    }

    bool readHost(const uint8_t* net, NetMsg::Host& h)
    {
        __try
        {
            h.network = *reinterpret_cast<const uint8_t*>(Gear::Addr::g_pNetPlayers.address + 4) != 0;
            if (!net)
            {
                return true;
            }
            h.isHost = net[1] != 0;
            h.localSlot = *reinterpret_cast<const uint32_t*>(net + 0x14);
            if (h.isHost)
            {
                h.hostSlot = h.localSlot;
            }
            else
            {
                for (uint32_t s = 1; s <= 16; ++s)
                {
                    if (net[kRoster + s * kRecord + kRecordHost])
                    {
                        h.hostSlot = s;
                        break;
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return true;
    }
}

NetMsg::Host NetMsg::host()
{
    Host h;
    if (Gear::Addr::g_pNetPlayers && !readHost(players(), h))
    {
        h = Host{};
        h.network = true;   // unreadable: assume a network game whose host is unknown
    }
    return h;
}

std::wstring NetMsg::heroName(uint32_t slot)
{
    const uint8_t* net = players();
    if (!net || slot < 1 || slot > 16)
    {
        return {};
    }
    wchar_t buf[128];
    size_t n = 0;
    const uint8_t* record = net + kRoster + slot * kRecord;
    // the hero's own name from the roster (network games), then the player name, then (the local hero, also in single
    // player) the name of the hero the save preview holds
    if (readName(record + kRecordHeroName, 64, buf, n) && n)
    {
        return {buf, n};
    }
    if (readName(record + kRecordPlayerName, 128, buf, n) && n)
    {
        return {buf, n};
    }
    const Host h = host();
    if ((h.localSlot == slot || !h.network) && readName(net + kLocalHeroName, 64, buf, n) && n)
    {
        return {buf, n};
    }
    return {};
}

bool NetMsg::takeHeroSaved(HeroSaved& out)
{
    std::lock_guard lock(g_lock);
    if (g_heroSaves.empty())
    {
        return false;
    }
    out = g_heroSaves.front();
    g_heroSaves.pop_front();
    return true;
}

bool NetMsg::takeKill(Kill& out)
{
    std::lock_guard lock(g_lock);
    if (g_kills.empty())
    {
        return false;
    }
    out = g_kills.front();
    g_kills.pop_front();
    return true;
}
