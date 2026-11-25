#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace Net
{
    // An IPv4 address of a network adapter that is up (loopback excluded). Addresses in network byte order.
    struct LocalAddress
    {
        uint32_t ifIndex;
        uint32_t address;
        uint32_t broadcast;     // the subnet's broadcast address, 0 for /31 and /32

        bool operator==(const LocalAddress&) const = default;
    };

    std::vector<LocalAddress> localAddresses();

    // The adapter a limited broadcast (255.255.255.255) leaves through: Windows sends it on the best route only,
    // not on every adapter. 0 if there is none.
    uint32_t limitedBroadcastInterface();

    std::string toString(uint32_t address);
}
