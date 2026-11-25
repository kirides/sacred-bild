#include "dds.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#define BCDEC_IMPLEMENTATION
#define BCDEC_BC4BC5_PRECISE
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244 4245 4267 4456 4100)
#endif
#include "bcdec.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace Gear::Dds
{
    namespace
    {
        uint32_t rd32(const uint8_t* p)
        {
            return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
        }

        constexpr uint32_t kPfFourCC = 0x4, kPfAlphaPixels = 0x1, kPfAlpha = 0x2, kPfRgb = 0x40, kPfLuminance = 0x20000;

        enum class Bc
        {
            None, Bc1, Bc2, Bc3, Bc4, Bc5, Bc6u, Bc6s, Bc7,
        };

        uint32_t pack(uint32_t a, uint32_t r, uint32_t g, uint32_t b)
        {
            return a << 24 | r << 16 | g << 8 | b;
        }

        // The value of the bits under `mask` of `v`, scaled to 8 bits.
        uint32_t channel(uint32_t v, uint32_t mask)
        {
            if (!mask)
            {
                return 0;
            }
            unsigned shift = 0;
            while (!((mask >> shift) & 1))
            {
                ++shift;
            }
            unsigned bits = 0;
            while (shift + bits < 32 && ((mask >> (shift + bits)) & 1))
            {
                ++bits;
            }
            const uint32_t x = (v & mask) >> shift;
            if (bits >= 8)
            {
                return x >> (bits - 8);
            }
            const uint32_t max = (1u << bits) - 1;
            return (x * 255 + max / 2) / max;
        }

        uint8_t srgb(float linear)
        {
            linear = std::clamp(linear, 0.0f, 1.0f);
            const float s = linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
            return static_cast<uint8_t>(std::lround(s * 255.0f));
        }

        // A block-compressed top mip decoded to ARGB.
        bool decodeBlocks(Bc bc, const uint8_t* src, size_t available, int w, int h, std::vector<uint32_t>& out)
        {
            const int bw = (w + 3) / 4, bh = (h + 3) / 4;
            const size_t blockBytes = (bc == Bc::Bc1 || bc == Bc::Bc4) ? 8 : 16;
            if (available < size_t(bw) * bh * blockBytes)
            {
                return false;
            }
            out.assign(size_t(w) * h, 0);
            for (int by = 0; by < bh; ++by)
            {
                for (int bx = 0; bx < bw; ++bx)
                {
                    const uint8_t* block = src + (size_t(by) * bw + bx) * blockBytes;
                    uint8_t px[16 * 4];        // RGBA, 4 x 4
                    uint8_t gray[16];
                    uint8_t rg[16 * 2];
                    float hdr[16 * 3];
                    switch (bc)
                    {
                    case Bc::Bc1: bcdec_bc1(block, px, 16); break;
                    case Bc::Bc2: bcdec_bc2(block, px, 16); break;
                    case Bc::Bc3: bcdec_bc3(block, px, 16); break;
                    case Bc::Bc7: bcdec_bc7(block, px, 16); break;
                    case Bc::Bc4:
                        bcdec_bc4(block, gray, 4, 0);
                        for (int i = 0; i < 16; ++i)
                        {
                            px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = gray[i];
                            px[i * 4 + 3] = 255;
                        }
                        break;
                    case Bc::Bc5:
                        bcdec_bc5(block, rg, 8, 0);
                        for (int i = 0; i < 16; ++i)
                        {
                            px[i * 4] = rg[i * 2];
                            px[i * 4 + 1] = rg[i * 2 + 1];
                            px[i * 4 + 2] = 0;
                            px[i * 4 + 3] = 255;
                        }
                        break;
                    case Bc::Bc6u: case Bc::Bc6s:
                        bcdec_bc6h_float(block, hdr, 4 * 3, bc == Bc::Bc6s ? 1 : 0);
                        for (int i = 0; i < 16; ++i)
                        {
                            px[i * 4] = srgb(hdr[i * 3]);
                            px[i * 4 + 1] = srgb(hdr[i * 3 + 1]);
                            px[i * 4 + 2] = srgb(hdr[i * 3 + 2]);
                            px[i * 4 + 3] = 255;
                        }
                        break;
                    default: return false;
                    }
                    for (int y = 0; y < 4 && by * 4 + y < h; ++y)
                    {
                        for (int x = 0; x < 4 && bx * 4 + x < w; ++x)
                        {
                            const uint8_t* p = px + (y * 4 + x) * 4;
                            out[size_t(by * 4 + y) * w + bx * 4 + x] = pack(p[3], p[0], p[1], p[2]);
                        }
                    }
                }
            }
            return true;
        }

        const char* bcName(Bc bc)
        {
            switch (bc)
            {
            case Bc::Bc1: return "BC1";
            case Bc::Bc2: return "BC2";
            case Bc::Bc3: return "BC3";
            case Bc::Bc4: return "BC4";
            case Bc::Bc5: return "BC5";
            case Bc::Bc6u: return "BC6H";
            case Bc::Bc6s: return "BC6H signed";
            case Bc::Bc7: return "BC7";
            default: return "?";
            }
        }

        // The DXGI formats this reader knows.
        enum Dxgi : uint32_t
        {
            kRgba16F = 10, kRgba8 = 28, kRgba8Srgb = 29, kRg8 = 49, kR8 = 61, kA8 = 65, kBc1 = 71, kBc1Srgb = 72, kBc2 = 74, kBc2Srgb = 75,
            kBc3 = 77, kBc3Srgb = 78, kBc4 = 80, kBc4S = 81, kBc5 = 83, kBc5S = 84, kB565 = 85, kB5551 = 86, kBgra8 = 87, kBgrx8 = 88,
            kBgra8Srgb = 91, kBgrx8Srgb = 93, kBc6u = 95, kBc6s = 96, kBc7 = 98, kBc7Srgb = 99, kB4444 = 115,
        };
    }

    bool isDds(const void* data, size_t size)
    {
        return size >= 4 && std::memcmp(data, "DDS ", 4) == 0;
    }

    bool load(const void* data, size_t size, bool allowBlock, Image& out, std::string& error)
    {
        const auto* b = static_cast<const uint8_t*>(data);
        if (size < 128 || std::memcmp(b, "DDS ", 4) != 0 || rd32(b + 4) != 124)
        {
            error = "not a DDS file";
            return false;
        }
        const int h = static_cast<int>(rd32(b + 12)), w = static_cast<int>(rd32(b + 16));
        if (w <= 0 || h <= 0 || w > 8192 || h > 8192)
        {
            error = "bad size";
            return false;
        }
        const uint32_t pfFlags = rd32(b + 80), pfFourCC = rd32(b + 84), bits = rd32(b + 88);
        const uint32_t rMask = rd32(b + 92), gMask = rd32(b + 96), bMask = rd32(b + 100), aMask = rd32(b + 104);
        const uint8_t* body = b + 128;
        Bc bc = Bc::None;
        uint32_t dxgi = 0;
        if (pfFlags & kPfFourCC)
        {
            if (pfFourCC == fourcc('D', 'X', '1', '0'))
            {
                if (size < 148)
                {
                    error = "truncated DX10 header";
                    return false;
                }
                dxgi = rd32(b + 128);
                body = b + 148;
                switch (dxgi)
                {
                case kBc1: case kBc1Srgb: bc = Bc::Bc1; break;
                case kBc2: case kBc2Srgb: bc = Bc::Bc2; break;
                case kBc3: case kBc3Srgb: bc = Bc::Bc3; break;
                case kBc4: bc = Bc::Bc4; break;
                case kBc5: bc = Bc::Bc5; break;
                case kBc6u: bc = Bc::Bc6u; break;
                case kBc6s: bc = Bc::Bc6s; break;
                case kBc7: case kBc7Srgb: bc = Bc::Bc7; break;
                default: break;
                }
            }
            else if (pfFourCC == kDxt1) { bc = Bc::Bc1; }
            else if (pfFourCC == fourcc('D', 'X', 'T', '2') || pfFourCC == kDxt3) { bc = Bc::Bc2; }
            else if (pfFourCC == fourcc('D', 'X', 'T', '4') || pfFourCC == kDxt5) { bc = Bc::Bc3; }
            else if (pfFourCC == fourcc('A', 'T', 'I', '1') || pfFourCC == fourcc('B', 'C', '4', 'U')) { bc = Bc::Bc4; }
            else if (pfFourCC == fourcc('A', 'T', 'I', '2') || pfFourCC == fourcc('B', 'C', '5', 'U')) { bc = Bc::Bc5; }
            else
            {
                error = "unsupported FourCC";
                return false;
            }
        }
        const size_t available = size - size_t(body - b);
        out = Image{};
        out.width = w;
        out.height = h;
        if (bc != Bc::None)
        {
            out.format = bcName(bc);
            const bool native = allowBlock && (bc == Bc::Bc1 || bc == Bc::Bc2 || bc == Bc::Bc3) && w >= 4 && h >= 4 &&
                                (w & (w - 1)) == 0 && (h & (h - 1)) == 0;
            if (native)
            {
                const size_t bytes = size_t(w / 4) * (h / 4) * (bc == Bc::Bc1 ? 8 : 16);
                if (available < bytes)
                {
                    error = "truncated";
                    return false;
                }
                out.kind = Kind::Block;
                out.fourcc = bc == Bc::Bc1 ? kDxt1 : bc == Bc::Bc2 ? kDxt3 : kDxt5;
                out.alpha = true;       // DXT1 may carry one-bit alpha
                out.blocks.assign(body, body + bytes);
                return true;
            }
            out.alpha = bc == Bc::Bc1 || bc == Bc::Bc2 || bc == Bc::Bc3 || bc == Bc::Bc7;
            if (!decodeBlocks(bc, body, available, w, h, out.argb))
            {
                error = "truncated";
                return false;
            }
            return true;
        }

        // Uncompressed: by DXGI format or by the pixel format's masks.
        uint32_t bpp = 0;      // bits per pixel
        enum class Fmt { Masks, Rgba8, Bgra8, Bgrx8, R8, Rg8, A8, B565, B5551, B4444, Rgba16F } fmt = Fmt::Masks;
        if (dxgi)
        {
            switch (dxgi)
            {
            case kRgba8: case kRgba8Srgb: fmt = Fmt::Rgba8; bpp = 32; break;
            case kBgra8: case kBgra8Srgb: fmt = Fmt::Bgra8; bpp = 32; break;
            case kBgrx8: case kBgrx8Srgb: fmt = Fmt::Bgrx8; bpp = 32; break;
            case kR8: fmt = Fmt::R8; bpp = 8; break;
            case kRg8: fmt = Fmt::Rg8; bpp = 16; break;
            case kA8: fmt = Fmt::A8; bpp = 8; break;
            case kB565: fmt = Fmt::B565; bpp = 16; break;
            case kB5551: fmt = Fmt::B5551; bpp = 16; break;
            case kB4444: fmt = Fmt::B4444; bpp = 16; break;
            case kRgba16F: fmt = Fmt::Rgba16F; bpp = 64; break;
            default:
                error = "unsupported DXGI format " + std::to_string(dxgi);
                return false;
            }
        }
        else if (pfFlags & (kPfRgb | kPfLuminance | kPfAlpha))
        {
            bpp = bits;
            if (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32)
            {
                error = "unsupported pixel format";
                return false;
            }
        }
        else
        {
            error = "unsupported pixel format";
            return false;
        }
        const size_t stride = size_t(w) * (bpp / 8);
        if (available < stride * h)
        {
            error = "truncated";
            return false;
        }
        out.format = dxgi ? "DXGI " + std::to_string(dxgi) : "uncompressed " + std::to_string(bpp) + " bit";
        out.argb.resize(size_t(w) * h);
        for (int y = 0; y < h; ++y)
        {
            const uint8_t* row = body + stride * y;
            for (int x = 0; x < w; ++x)
            {
                const uint8_t* p = row + size_t(x) * (bpp / 8);
                uint32_t argb = 0;
                switch (fmt)
                {
                case Fmt::Rgba8: argb = pack(p[3], p[0], p[1], p[2]); out.alpha = true; break;
                case Fmt::Bgra8: argb = pack(p[3], p[2], p[1], p[0]); out.alpha = true; break;
                case Fmt::Bgrx8: argb = pack(255, p[2], p[1], p[0]); break;
                case Fmt::R8: argb = pack(255, p[0], p[0], p[0]); break;
                case Fmt::Rg8: argb = pack(255, p[0], p[1], 0); break;
                case Fmt::A8: argb = pack(p[0], 255, 255, 255); out.alpha = true; break;
                case Fmt::B565:
                {
                    const uint32_t v = p[0] | p[1] << 8;
                    argb = pack(255, channel(v, 0xF800), channel(v, 0x7E0), channel(v, 0x1F));
                    break;
                }
                case Fmt::B5551:
                {
                    const uint32_t v = p[0] | p[1] << 8;
                    argb = pack(channel(v, 0x8000), channel(v, 0x7C00), channel(v, 0x3E0), channel(v, 0x1F));
                    out.alpha = true;
                    break;
                }
                case Fmt::B4444:
                {
                    const uint32_t v = p[0] | p[1] << 8;
                    argb = pack(channel(v, 0xF000), channel(v, 0xF00), channel(v, 0xF0), channel(v, 0xF));
                    out.alpha = true;
                    break;
                }
                case Fmt::Rgba16F:
                {
                    // half floats: the value 0..1 only (HDR clamps)
                    auto half = [&](int i)
                    {
                        const uint32_t hv = p[i * 2] | p[i * 2 + 1] << 8;
                        const uint32_t s = hv >> 15, e = (hv >> 10) & 31, m = hv & 1023;
                        const float f = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? 65504.0f : std::ldexp(float(m | 1024), int(e) - 25);
                        return s ? 0.0f : f;
                    };
                    argb = pack(uint32_t(std::lround(std::clamp(half(3), 0.0f, 1.0f) * 255.0f)), srgb(half(0)), srgb(half(1)), srgb(half(2)));
                    out.alpha = true;
                    break;
                }
                default:
                {
                    uint32_t v = 0;
                    for (uint32_t i = 0; i < bpp / 8; ++i)
                    {
                        v |= uint32_t(p[i]) << (8 * i);
                    }
                    if (pfFlags & kPfLuminance)
                    {
                        const uint32_t l = channel(v, rMask ? rMask : 0xFF);
                        const uint32_t a = (pfFlags & kPfAlphaPixels) ? channel(v, aMask) : 255;
                        argb = pack(a, l, l, l);
                    }
                    else if (pfFlags & kPfRgb)
                    {
                        const uint32_t a = (pfFlags & kPfAlphaPixels) ? channel(v, aMask) : 255;
                        argb = pack(a, channel(v, rMask), channel(v, gMask), channel(v, bMask));
                    }
                    else
                    {
                        argb = pack(channel(v, aMask ? aMask : 0xFF), 255, 255, 255);
                    }
                    out.alpha = out.alpha || (pfFlags & (kPfAlphaPixels | kPfAlpha)) != 0;
                    break;
                }
                }
                out.argb[size_t(y) * w + x] = argb;
            }
        }
        return true;
    }
}
