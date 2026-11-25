// DDS texture files for Gear's images (<img src="x.dds">, decorator image(x.dds)): the DirectDraw header and the DX10 header, every block compression
// (BC1 to BC7, also as the legacy DXT1..5 / ATI1 / ATI2 / BC4U / BC5U FourCCs) and the common uncompressed formats. Only the top mip is read.
//
// BC1, BC2 and BC3 of a power-of-two size whose sides are multiples of 4 stay compressed ("Block": the GPU samples them as DXT1/3/5, no decoding, 1/4 to
// 1/8 of the memory and no conversion on load). Everything else is decoded once to 8-bit ARGB: BC4 to gray, BC5 to red and green, BC6H (HDR, clamped
// and sRGB encoded) and BC7 (the GPU of the game's Direct3D 9 backend has neither). No Windows headers: tests/dds_test.cpp builds this file alone.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Gear::Dds
{
    enum class Kind
    {
        Argb,       // `argb`: 0xAARRGGBB, rows top to bottom, straight (not premultiplied) alpha
        Block,      // `blocks`: the top mip as DXT1 / DXT3 / DXT5 blocks (`fourcc`), straight alpha
    };

    constexpr uint32_t fourcc(char a, char b, char c, char d)
    {
        return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24;
    }
    constexpr uint32_t kDxt1 = fourcc('D', 'X', 'T', '1'), kDxt3 = fourcc('D', 'X', 'T', '3'), kDxt5 = fourcc('D', 'X', 'T', '5');

    struct Image
    {
        int width = 0, height = 0;
        Kind kind = Kind::Argb;
        uint32_t fourcc = 0;                // Block: kDxt1 / kDxt3 / kDxt5
        bool alpha = false;                 // the image has (or may have) transparent parts
        std::string format;                 // for the log: "BC7", "BGRA8", ...
        std::vector<uint32_t> argb;
        std::vector<uint8_t> blocks;
    };

    bool isDds(const void* data, size_t size);

    // Reads the top mip. `allowBlock` false: BC1..3 are decoded too. False (with `error`) for anything unreadable or unsupported.
    bool load(const void* data, size_t size, bool allowBlock, Image& out, std::string& error);
}
