#include "netmsg.h"
#include "log.h"
#include "patch.h"
#include "game/gear_addr.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    constexpr uint32_t kIds = 0x400;
    constexpr int kLogPerId = 3;

    std::atomic<uint32_t> g_count[kIds];
    decltype(Gear::Addr::cNet_queueMessage)::Ptr g_orig = nullptr;

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
    const char* on = std::getenv("GEAR_NETTRACE");
    if (!on || on[0] != '1')
    {
        return true;
    }
    if (!Gear::Addr::cNet_queueMessage)
    {
        LOG("NetMsg: the send queue was not found: net trace is off");
        return false;
    }
    LOG("NetMsg: tracing the messages this client sends (GEAR_NETTRACE=1)");
    return Patch::hook(g_orig, Gear::Addr::cNet_queueMessage, hookQueue, "cNet_queueMessage");
}
