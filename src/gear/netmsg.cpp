#include "netmsg.h"
#include "chat.h"
#include "log.h"
#include "patch.h"
#include "engine/gear_addr.h"

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

    // A chat message that is no Gear frame: its type and text for gear.chat (it tells who sent the line the game adds
    // next, src/gear/chat.cpp).
    void noteChat(const uint8_t* message, const uint8_t* payload, uint32_t size, uint32_t sender)
    {
        uint16_t text[512];
        size_t n = 0;
        uint16_t type = 0;
        __try
        {
            type = *reinterpret_cast<const uint16_t*>(message + 0xE);
            if (type < 3 || type > 5 || size < 6 || size > 0x10000)
            {
                return;
            }
            const uint16_t* t = reinterpret_cast<const uint16_t*>(payload + 4);
            const size_t max = (size - 4) / 2;
            while (n < max && n < 512 && t[n] != 0)
            {
                text[n] = t[n];
                ++n;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }
        Chat::noteNet(type, sender, text, n);
    }

    bool __stdcall onChat(const uint8_t* message, const uint8_t* payload, uint32_t size, uint32_t sender)
    {
        uint16_t units[NetMsg::kMaxFrameUnits];
        size_t n = 0;
        if (!copyFrame(message, payload, size, units, n))
        {
            noteChat(message, payload, size, sender);
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

    // Message 0x132 (a character block): the game server delivers the hero it holds for the player (Closed Net: from the
    // lobby). Open Net and LAN games never get one: the client keeps its local hero. The case of the dispatcher's jump table
    // for that type is redirected to a stub that notes it and goes on with the original case (the case's own code is not
    // touched). Reset by NetMsg::clearHeroFromServer (the world does it when the network game ends).
    volatile uint8_t g_heroFromServer = 0;
    uintptr_t g_heroCase = 0;
    bool g_heroHook = false;

    __declspec(naked) void heroStub()
    {
        __asm {
            mov byte ptr [g_heroFromServer], 1
            jmp dword ptr [g_heroCase]
        }
    }

    bool installHeroHook()
    {
        const uintptr_t sw = Gear::Addr::netDispatchSwitch;
        if (!sw || !Patch::verify(sw, {0x81, 0xF9, 0xD4, 0x01, 0x00, 0x00, 0x0F, 0x87}) || !Patch::verify(sw + 0xC, {0x33, 0xD2, 0x8A, 0x91}) ||
            !Patch::verify(sw + 0x14, {0xFF, 0x24, 0x95}))
        {
            LOG("NetMsg: hero source: the receive dispatcher was not recognised: games entered through the Internet count as Closed Net");
            return false;
        }
        uint32_t indexTable = 0, jumpTable = 0;
        std::memcpy(&indexTable, reinterpret_cast<const void*>(sw + 0x10), 4);
        std::memcpy(&jumpTable, reinterpret_cast<const void*>(sw + 0x17), 4);
        const uint8_t* idx = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(indexTable));
        constexpr uint32_t kType = 0x132;
        const uint8_t mine = idx[kType - 1];
        unsigned same = 0;
        for (uint32_t i = 0; i < 0x1D4; ++i)
        {
            same += idx[i] == mine;
        }
        if (same != 1)
        {
            LOG("NetMsg: hero source: message 0x132 shares its case with {} others: not hooked", same - 1);
            return false;
        }
        const uintptr_t slot = jumpTable + mine * 4u;
        g_heroCase = *reinterpret_cast<const uint32_t*>(slot);
        if (!g_heroCase || !Patch::value(slot, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&heroStub))))
        {
            return false;
        }
        g_heroHook = true;
        LOG("NetMsg: hero source: message 0x132 (case {:08X}, jump table slot {:08X}) is watched", g_heroCase, slot);
        return true;
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

    // Custom opcodes: message types above the stock range (the dispatcher's index check `cmp ecx, 0x1D4 / ja next`
    // sends every type over 0x1D5 to the next message). The `ja` is redirected to opStub, which hands frames of the
    // two configured types to onOp and then goes on to the next message like the stock code did.
    std::atomic<uint16_t> g_opControl{NetMsg::kDefaultControl}, g_opData{NetMsg::kDefaultData};
    bool g_ops = false;
    std::deque<NetMsg::OpFrame> g_opFrames;     // under g_lock
    size_t g_opBytes = 0;
    constexpr size_t kOpQueueMax = 4096, kOpBytesMax = 4u << 20, kOpWireMax = 0x10000;
    std::atomic<uint32_t> g_opSeen{0};

    bool copyOp(const uint8_t* /*message*/, const uint8_t* payload, uint32_t size, std::vector<uint8_t>& out)
    {
        __try
        {
            if (size > kOpWireMax)
            {
                return false;
            }
            out.assign(payload, payload + size);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return true;
    }

    void __stdcall onOp(const uint8_t* message, const uint8_t* payload, uint32_t size, uint32_t sender, uint32_t type)
    {
        const uint16_t t = static_cast<uint16_t>(type);
        if (t != g_opControl.load(std::memory_order_relaxed) && t != g_opData.load(std::memory_order_relaxed))
        {
            return;
        }
        NetMsg::OpFrame f;
        f.sender = sender;
        f.type = t;
        if (!copyOp(message, payload, size, f.data))
        {
            return;
        }
        if (g_opSeen.fetch_add(1) < 5)
        {
            LOG("NetMsg: custom opcode 0x{:X} received from slot {}, {} bytes", t, sender, size);
        }
        std::lock_guard lock(g_lock);
        if (g_opFrames.size() >= kOpQueueMax || g_opBytes + f.data.size() > kOpBytesMax)
        {
            return;
        }
        g_opBytes += f.data.size();
        g_opFrames.push_back(std::move(f));
    }

    // At the dispatcher's `ja next` (cmp ecx, 0x1D4): edx the type, esi the sender slot, ebx the payload size, ebp the
    // payload, [esp+0x2C] the message. The jump to the next message is the one the `ja` took.
    __declspec(naked) void opStub()
    {
        __asm {
            pushad
            push edx
            push esi
            push ebx
            push ebp
            push dword ptr [esp + 0x2C + 0x20 + 0x10]
            call onOp
            popad
            jmp dword ptr [g_nextMessage]
        }
    }

    bool installOps()
    {
        const uintptr_t sw = Gear::Addr::netDispatchSwitch;
        if (!sw || !g_nextMessage)
        {
            return false;
        }
        // ja rel32 at sw+6; its target must be the next-message address the chat stub found
        if (!Patch::verify(sw + 6, {0x0F, 0x87}))
        {
            LOG("NetMsg: the dispatcher's range check is not a ja at {:08X}: no custom opcodes", sw + 6);
            return false;
        }
        int32_t rel = 0;
        std::memcpy(&rel, reinterpret_cast<const void*>(sw + 8), 4);
        if (sw + 0xC + rel != g_nextMessage)
        {
            LOG("NetMsg: the dispatcher's range check jumps elsewhere: no custom opcodes");
            return false;
        }
        const int32_t stubRel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&opStub) - (sw + 0xC));
        if (!Patch::value(sw + 8, static_cast<uint32_t>(stubRel)))
        {
            return false;
        }
        LOG("NetMsg: custom opcodes on (0x{:X} control, 0x{:X} data, range check at {:08X})", g_opControl.load(), g_opData.load(), sw + 6);
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

bool NetMsg::heroSourceKnown() { return g_heroHook; }
bool NetMsg::heroFromServer() { return g_heroFromServer != 0; }
void NetMsg::clearHeroFromServer() { g_heroFromServer = 0; }
void NetMsg::setHeroFromServerForTest(bool on) { g_heroFromServer = on ? 1 : 0; }

bool NetMsg::install()
{
    g_channel = installChannel();
    installHeroHook();
    if (const char* ops = std::getenv("GEAR_NETOPS"))
    {
        unsigned c = 0, d = 0;
        if (std::sscanf(ops, "%i,%i", &c, &d) == 2)
        {
            setOpcodes(static_cast<uint16_t>(c), static_cast<uint16_t>(d));
        }
    }
    g_ops = g_channel && installOps();
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
    if (slot > 16 || count == 0 || count > kMaxFrameUnits || !channelReady())
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

bool NetMsg::sendChatForTest(uint16_t type, uint32_t target, const uint16_t* units, size_t count)
{
    if ((type < 3 || type > 5) || target > 16 || count == 0 || count > kMaxFrameUnits || !channelReady())
    {
        return false;
    }
    uint8_t data[4 + (kMaxFrameUnits + 1) * 2] = {};
    std::memcpy(data, &target, 4);
    std::memcpy(data + 4, units, count * 2);
    const uint32_t size = static_cast<uint32_t>(4 + (count + 1) * 2);
    Gear::Addr::cNet_sendMessage(*Gear::Addr::g_pNet.get(), data, size, type, 0);
    return true;
}
bool NetMsg::opsReady()
{
    return g_ops && channelReady();
}

bool NetMsg::setOpcodes(uint16_t control, uint16_t data)
{
    if (control <= kStockMax || data <= kStockMax || control == data || control >= kOpMax || data >= kOpMax)
    {
        return false;
    }
    g_opControl = control;
    g_opData = data;
    return true;
}

uint16_t NetMsg::controlOpcode()
{
    return g_opControl;
}

uint16_t NetMsg::dataOpcode()
{
    return g_opData;
}

bool NetMsg::sendOp(uint16_t type, const void* data, size_t size)
{
    if ((type != g_opControl && type != g_opData) || size == 0 || size > kMaxOpPayload || !opsReady())
    {
        return false;
    }
    Gear::Addr::cNet_sendMessage(*Gear::Addr::g_pNet.get(), data, static_cast<uint32_t>(size), type, 0);
    return true;
}

bool NetMsg::takeOp(OpFrame& out)
{
    std::lock_guard lock(g_lock);
    if (g_opFrames.empty())
    {
        return false;
    }
    out = std::move(g_opFrames.front());
    g_opFrames.pop_front();
    g_opBytes -= out.data.size();
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

    // The hero's name from its text resource (the id at creature +0x3C), as the hero save and the HUD take it.
    bool textName(uint32_t slot, wchar_t* out, size_t& n)
    {
        n = 0;
        if (!Gear::Addr::g_pTextManager || !Gear::Addr::cTextManager_get || !Gear::Addr::g_pObjectManager)
        {
            return false;
        }
        __try
        {
            const uint8_t* manager = static_cast<const uint8_t*>(*Gear::Addr::g_pObjectManager.get());
            void* texts = *Gear::Addr::g_pTextManager.get();
            if (!manager || !texts)
            {
                return false;
            }
            const uintptr_t* begin = *reinterpret_cast<uintptr_t* const*>(manager + 4);
            const uintptr_t* end = *reinterpret_cast<uintptr_t* const*>(manager + 8);
            if (!begin || begin + slot >= end)
            {
                return false;
            }
            const uint8_t* hero = reinterpret_cast<const uint8_t*>(begin[slot]);
            if (!hero || *reinterpret_cast<const uint32_t*>(hero + 0x0C) != slot)
            {
                return false;
            }
            const uint32_t id = *reinterpret_cast<const uint32_t*>(hero + 0x3C);
            if (id < 0x2700 || id > 0x270F)
            {
                return false;
            }
            const wchar_t* const* ws = Gear::Addr::cTextManager_get(texts, id);
            if (!ws || !ws[0])
            {
                return false;
            }
            for (const wchar_t* p = ws[0]; p < ws[1] && n < 63 && *p; ++p)
            {
                out[n++] = *p;
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
    if (textName(slot, buf, n) && n)
    {
        return {buf, n};
    }
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
