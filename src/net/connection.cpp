#include "net/connection.h"
#include "game/sacred_de.h"
#include "config.h"
#include "log.h"
#include "patch.h"

#include <cstdint>

void Connection::install()
{
    if (!g_config.netNoDelay)
    {
        return;
    }
    // cmp ebx, 2; sete al (after xor eax, eax) -> mov al, 1: drv_disable_nagle is always 1.
    constexpr uint8_t kAlways[] = {0xB0, 0x01, 0x90, 0x90, 0x90, 0x90};
    if (Patch::verify(Sacred::Addr::initNetworkNagleTest, {0x83, 0xFB, 0x02, 0x0F, 0x94, 0xC0}) &&
        Patch::write(Sacred::Addr::initNetworkNagleTest, kAlways, sizeof(kAlways)))
    {
        LOG("Game connection: TCP_NODELAY in both data flow modes");
    }
}
