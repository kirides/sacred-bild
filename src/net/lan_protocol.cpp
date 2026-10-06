#include "net/lan_protocol.h"

#include <windows.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace
{
    constexpr uint32_t kUncompressed = 0x08000000;
    constexpr uint32_t kKey = 0xB5D6C7A3;
}

void LanAnnounce::encode(const uint8_t* plain, uint32_t address, uint8_t* wire)
{
    uint8_t payload[kSize];
    std::memcpy(payload, plain, kSize);
    const uint32_t hostOrder = _byteswap_ulong(address);
    std::memcpy(payload + kAddressOffset, &hostOrder, 4);

    // UncompressMemory: each word is the cipher word XOR (previous cipher word + key), starting from the size;
    // the last size % 4 bytes are stored as they are.
    const uint32_t header = static_cast<uint32_t>(kSize) | kUncompressed;
    std::memcpy(wire, &header, 4);
    uint32_t prev = static_cast<uint32_t>(kSize);
    for (size_t i = 0; i + 4 <= kSize; i += 4)
    {
        uint32_t word;
        std::memcpy(&word, payload + i, 4);
        prev = (prev + kKey) ^ word;
        std::memcpy(wire + 4 + i, &prev, 4);
    }
    const size_t tail = kSize & ~size_t{3};
    std::memcpy(wire + 4 + tail, payload + tail, kSize - tail);
}

std::string LanAnnounce::gameName(const uint8_t* plain)
{
    wchar_t name[kNameChars + 1] = {};
    std::memcpy(name, plain + kNameOffset, kNameChars * sizeof(wchar_t));
    char out[kNameChars * 3 + 1] = {};
    WideCharToMultiByte(CP_UTF8, 0, name, -1, out, sizeof(out), nullptr, nullptr);
    return out;
}

void LanAnnounce::prefixName(uint8_t* plain, std::wstring_view prefix)
{
    wchar_t name[kNameChars + 1] = {};
    std::memcpy(name, plain + kNameOffset, kNameChars * sizeof(wchar_t));
    std::wstring tagged(prefix);
    tagged += name;
    tagged.resize(std::min(tagged.size(), kNameChars - 1));
    std::memset(plain + kNameOffset, 0, kNameChars * sizeof(wchar_t));
    std::memcpy(plain + kNameOffset, tagged.data(), tagged.size() * sizeof(wchar_t));
}

bool LanRelay::hasHeader(const uint8_t* msg, size_t size, const uint8_t (&magic)[4], size_t minSize)
{
    uint32_t version = 0;
    if (size < minSize || std::memcmp(msg, magic, 4) != 0)
    {
        return false;
    }
    std::memcpy(&version, msg + 4, 4);
    return version == kVersion;
}

void LanRelay::writeHeader(uint8_t* msg, const uint8_t (&magic)[4])
{
    std::memcpy(msg, magic, 4);
    std::memcpy(msg + 4, &kVersion, 4);
}
