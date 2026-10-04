#include "ddraw9/format.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace DDraw9::Format
{
    namespace
    {
        DDPIXELFORMAT rgb(DWORD bits, DWORD r, DWORD g, DWORD b, DWORD a)
        {
            DDPIXELFORMAT pf = {};
            pf.dwSize = sizeof(pf);
            pf.dwFlags = DDPF_RGB | (a ? DDPF_ALPHAPIXELS : 0);
            pf.dwRGBBitCount = bits;
            pf.dwRBitMask = r;
            pf.dwGBitMask = g;
            pf.dwBBitMask = b;
            pf.dwRGBAlphaBitMask = a;
            return pf;
        }

        DDPIXELFORMAT zbuffer(DWORD bits, DWORD zMask, DWORD stencilBits, DWORD stencilMask)
        {
            DDPIXELFORMAT pf = {};
            pf.dwSize = sizeof(pf);
            pf.dwFlags = DDPF_ZBUFFER | (stencilBits ? DDPF_STENCILBUFFER : 0);
            pf.dwZBufferBitDepth = bits;
            pf.dwStencilBitDepth = stencilBits;
            pf.dwZBitMask = zMask;
            pf.dwStencilBitMask = stencilMask;
            return pf;
        }

        bool masks(const DDPIXELFORMAT& pf, DWORD r, DWORD g, DWORD b)
        {
            return pf.dwRBitMask == r && pf.dwGBitMask == g && pf.dwBBitMask == b;
        }

        int lowestBit(uint32_t mask)
        {
            int shift = 0;
            while (mask && !(mask & 1))
            {
                mask >>= 1;
                ++shift;
            }
            return shift;
        }

        int bitCount(uint32_t mask)
        {
            int n = 0;
            for (; mask; mask &= mask - 1)
            {
                ++n;
            }
            return n;
        }
    }

    d9::D3DFORMAT fromPixelFormat(const DDPIXELFORMAT& pf)
    {
        if (pf.dwFlags & DDPF_FOURCC)
        {
            switch (pf.dwFourCC)
            {
            case MAKEFOURCC('D', 'X', 'T', '1'): return d9::D3DFMT_DXT1;
            case MAKEFOURCC('D', 'X', 'T', '2'): return d9::D3DFMT_DXT2;
            case MAKEFOURCC('D', 'X', 'T', '3'): return d9::D3DFMT_DXT3;
            case MAKEFOURCC('D', 'X', 'T', '4'): return d9::D3DFMT_DXT4;
            case MAKEFOURCC('D', 'X', 'T', '5'): return d9::D3DFMT_DXT5;
            }
            return d9::D3DFMT_UNKNOWN;
        }
        if (pf.dwFlags & DDPF_ZBUFFER)
        {
            const DWORD bits = pf.dwZBufferBitDepth;
            const DWORD stencilBits = (pf.dwFlags & DDPF_STENCILBUFFER) ? pf.dwStencilBitDepth : 0;
            if (bits == 16)
            {
                return stencilBits ? d9::D3DFMT_D15S1 : d9::D3DFMT_D16;
            }
            if (bits == 24)
            {
                return stencilBits ? d9::D3DFMT_D24S8 : d9::D3DFMT_D24X8;
            }
            if (bits == 32)
            {
                if (stencilBits == 8)
                {
                    return d9::D3DFMT_D24S8;
                }
                if (stencilBits == 4)
                {
                    return d9::D3DFMT_D24X4S4;
                }
                return pf.dwZBitMask == 0xFFFFFFFF ? d9::D3DFMT_D32 : d9::D3DFMT_D24X8;
            }
            return d9::D3DFMT_UNKNOWN;
        }
        if (pf.dwFlags & DDPF_RGB)
        {
            const DWORD alpha = (pf.dwFlags & DDPF_ALPHAPIXELS) ? pf.dwRGBAlphaBitMask : 0;
            switch (pf.dwRGBBitCount)
            {
            case 32:
                if (masks(pf, 0xFF0000, 0xFF00, 0xFF))
                {
                    return alpha ? d9::D3DFMT_A8R8G8B8 : d9::D3DFMT_X8R8G8B8;
                }
                if (masks(pf, 0xFF, 0xFF00, 0xFF0000))
                {
                    return alpha ? d9::D3DFMT_A8B8G8R8 : d9::D3DFMT_X8B8G8R8;
                }
                break;
            case 24:
                if (masks(pf, 0xFF0000, 0xFF00, 0xFF))
                {
                    return d9::D3DFMT_R8G8B8;
                }
                break;
            case 16:
                if (masks(pf, 0xF800, 0x7E0, 0x1F))
                {
                    return d9::D3DFMT_R5G6B5;
                }
                if (masks(pf, 0x7C00, 0x3E0, 0x1F))
                {
                    return alpha ? d9::D3DFMT_A1R5G5B5 : d9::D3DFMT_X1R5G5B5;
                }
                if (masks(pf, 0xF00, 0xF0, 0xF))
                {
                    return alpha ? d9::D3DFMT_A4R4G4B4 : d9::D3DFMT_X4R4G4B4;
                }
                break;
            case 8:
                if (masks(pf, 0xE0, 0x1C, 0x3))
                {
                    return d9::D3DFMT_R3G3B2;
                }
                break;
            }
            return d9::D3DFMT_UNKNOWN;
        }
        if (pf.dwFlags & DDPF_LUMINANCE)
        {
            const DWORD alpha = (pf.dwFlags & DDPF_ALPHAPIXELS) ? pf.dwLuminanceAlphaBitMask : 0;
            if (pf.dwLuminanceBitCount == 8)
            {
                return alpha ? d9::D3DFMT_A4L4 : d9::D3DFMT_L8;
            }
            if (pf.dwLuminanceBitCount == 16 && alpha)
            {
                return d9::D3DFMT_A8L8;
            }
            return d9::D3DFMT_UNKNOWN;
        }
        if ((pf.dwFlags & DDPF_ALPHA) && pf.dwAlphaBitDepth == 8)
        {
            return d9::D3DFMT_A8;
        }
        return d9::D3DFMT_UNKNOWN;
    }

    DDPIXELFORMAT toPixelFormat(d9::D3DFORMAT format)
    {
        switch (format)
        {
        case d9::D3DFMT_A8R8G8B8: return rgb(32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000);
        case d9::D3DFMT_X8R8G8B8: return rgb(32, 0xFF0000, 0xFF00, 0xFF, 0);
        case d9::D3DFMT_A8B8G8R8: return rgb(32, 0xFF, 0xFF00, 0xFF0000, 0xFF000000);
        case d9::D3DFMT_X8B8G8R8: return rgb(32, 0xFF, 0xFF00, 0xFF0000, 0);
        case d9::D3DFMT_R8G8B8: return rgb(24, 0xFF0000, 0xFF00, 0xFF, 0);
        case d9::D3DFMT_R5G6B5: return rgb(16, 0xF800, 0x7E0, 0x1F, 0);
        case d9::D3DFMT_X1R5G5B5: return rgb(16, 0x7C00, 0x3E0, 0x1F, 0);
        case d9::D3DFMT_A1R5G5B5: return rgb(16, 0x7C00, 0x3E0, 0x1F, 0x8000);
        case d9::D3DFMT_A4R4G4B4: return rgb(16, 0xF00, 0xF0, 0xF, 0xF000);
        case d9::D3DFMT_X4R4G4B4: return rgb(16, 0xF00, 0xF0, 0xF, 0);
        case d9::D3DFMT_R3G3B2: return rgb(8, 0xE0, 0x1C, 0x3, 0);
        case d9::D3DFMT_D16: return zbuffer(16, 0xFFFF, 0, 0);
        case d9::D3DFMT_D15S1: return zbuffer(16, 0xFFFE, 1, 0x1);
        case d9::D3DFMT_D24X8: return zbuffer(32, 0xFFFFFF00, 0, 0);
        case d9::D3DFMT_D24S8: return zbuffer(32, 0xFFFFFF00, 8, 0xFF);
        case d9::D3DFMT_D24X4S4: return zbuffer(32, 0xFFFFFF00, 4, 0xF);
        case d9::D3DFMT_D32: return zbuffer(32, 0xFFFFFFFF, 0, 0);
        default: break;
        }
        DDPIXELFORMAT pf = {};
        pf.dwSize = sizeof(pf);
        switch (format)
        {
        case d9::D3DFMT_DXT1: case d9::D3DFMT_DXT2: case d9::D3DFMT_DXT3: case d9::D3DFMT_DXT4: case d9::D3DFMT_DXT5:
            pf.dwFlags = DDPF_FOURCC;
            pf.dwFourCC = static_cast<DWORD>(format);
            break;
        case d9::D3DFMT_L8:
            pf.dwFlags = DDPF_LUMINANCE;
            pf.dwLuminanceBitCount = 8;
            pf.dwLuminanceBitMask = 0xFF;
            break;
        case d9::D3DFMT_A4L4:
            pf.dwFlags = DDPF_LUMINANCE | DDPF_ALPHAPIXELS;
            pf.dwLuminanceBitCount = 8;
            pf.dwLuminanceBitMask = 0xF;
            pf.dwLuminanceAlphaBitMask = 0xF0;
            break;
        case d9::D3DFMT_A8L8:
            pf.dwFlags = DDPF_LUMINANCE | DDPF_ALPHAPIXELS;
            pf.dwLuminanceBitCount = 16;
            pf.dwLuminanceBitMask = 0xFF;
            pf.dwLuminanceAlphaBitMask = 0xFF00;
            break;
        case d9::D3DFMT_A8:
            pf.dwFlags = DDPF_ALPHA;
            pf.dwAlphaBitDepth = 8;
            break;
        default:
            break;
        }
        return pf;
    }

    UINT bitsPerPixel(d9::D3DFORMAT format)
    {
        switch (format)
        {
        case d9::D3DFMT_A8R8G8B8: case d9::D3DFMT_X8R8G8B8: case d9::D3DFMT_A8B8G8R8: case d9::D3DFMT_X8B8G8R8:
        case d9::D3DFMT_D24X8: case d9::D3DFMT_D24S8: case d9::D3DFMT_D24X4S4: case d9::D3DFMT_D32:
            return 32;
        case d9::D3DFMT_R8G8B8:
            return 24;
        case d9::D3DFMT_R5G6B5: case d9::D3DFMT_X1R5G5B5: case d9::D3DFMT_A1R5G5B5: case d9::D3DFMT_A4R4G4B4:
        case d9::D3DFMT_X4R4G4B4: case d9::D3DFMT_A8L8: case d9::D3DFMT_D16: case d9::D3DFMT_D15S1:
            return 16;
        case d9::D3DFMT_R3G3B2: case d9::D3DFMT_L8: case d9::D3DFMT_A4L4: case d9::D3DFMT_A8: case d9::D3DFMT_P8:
        case d9::D3DFMT_DXT2: case d9::D3DFMT_DXT3: case d9::D3DFMT_DXT4: case d9::D3DFMT_DXT5:
            return 8;
        case d9::D3DFMT_DXT1:
            return 4;
        default:
            return 0;
        }
    }

    bool blockCompressed(d9::D3DFORMAT format)
    {
        return format == d9::D3DFMT_DXT1 || format == d9::D3DFMT_DXT2 || format == d9::D3DFMT_DXT3 ||
            format == d9::D3DFMT_DXT4 || format == d9::D3DFMT_DXT5;
    }

    bool depth(d9::D3DFORMAT format)
    {
        return format == d9::D3DFMT_D16 || format == d9::D3DFMT_D15S1 || format == d9::D3DFMT_D24X8 ||
            format == d9::D3DFMT_D24S8 || format == d9::D3DFMT_D24X4S4 || format == d9::D3DFMT_D32;
    }

    bool stencil(d9::D3DFORMAT format)
    {
        return format == d9::D3DFMT_D15S1 || format == d9::D3DFMT_D24S8 || format == d9::D3DFMT_D24X4S4;
    }

    UINT pitch(d9::D3DFORMAT format, UINT width)
    {
        if (blockCompressed(format))
        {
            return std::max(1u, (width + 3) / 4) * (format == d9::D3DFMT_DXT1 ? 8 : 16);
        }
        return width * bitsPerPixel(format) / 8;
    }

    UINT rows(d9::D3DFORMAT format, UINT height)
    {
        return blockCompressed(format) ? std::max(1u, (height + 3) / 4) : height;
    }

    std::string name(d9::D3DFORMAT format)
    {
        switch (format)
        {
        case d9::D3DFMT_A8R8G8B8: return "A8R8G8B8";
        case d9::D3DFMT_X8R8G8B8: return "X8R8G8B8";
        case d9::D3DFMT_A8B8G8R8: return "A8B8G8R8";
        case d9::D3DFMT_X8B8G8R8: return "X8B8G8R8";
        case d9::D3DFMT_R8G8B8: return "R8G8B8";
        case d9::D3DFMT_R5G6B5: return "R5G6B5";
        case d9::D3DFMT_X1R5G5B5: return "X1R5G5B5";
        case d9::D3DFMT_A1R5G5B5: return "A1R5G5B5";
        case d9::D3DFMT_A4R4G4B4: return "A4R4G4B4";
        case d9::D3DFMT_X4R4G4B4: return "X4R4G4B4";
        case d9::D3DFMT_R3G3B2: return "R3G3B2";
        case d9::D3DFMT_L8: return "L8";
        case d9::D3DFMT_A4L4: return "A4L4";
        case d9::D3DFMT_A8L8: return "A8L8";
        case d9::D3DFMT_A8: return "A8";
        case d9::D3DFMT_D16: return "D16";
        case d9::D3DFMT_D15S1: return "D15S1";
        case d9::D3DFMT_D24X8: return "D24X8";
        case d9::D3DFMT_D24S8: return "D24S8";
        case d9::D3DFMT_D24X4S4: return "D24X4S4";
        case d9::D3DFMT_D32: return "D32";
        case d9::D3DFMT_DXT1: return "DXT1";
        case d9::D3DFMT_DXT2: return "DXT2";
        case d9::D3DFMT_DXT3: return "DXT3";
        case d9::D3DFMT_DXT4: return "DXT4";
        case d9::D3DFMT_DXT5: return "DXT5";
        default: return std::format("format {}", static_cast<int>(format));
        }
    }

    Rgb::Rgb(const DDPIXELFORMAT& pf)
    {
        if (!(pf.dwFlags & DDPF_RGB) || (pf.dwFlags & (DDPF_FOURCC | DDPF_PALETTEINDEXED8 | DDPF_PALETTEINDEXED4)))
        {
            return;
        }
        m_bytes = pf.dwRGBBitCount / 8;
        if (m_bytes < 1 || m_bytes > 4 || pf.dwRGBBitCount % 8)
        {
            return;
        }
        m_masks[0] = (pf.dwFlags & DDPF_ALPHAPIXELS) ? pf.dwRGBAlphaBitMask : 0;
        m_masks[1] = pf.dwRBitMask;
        m_masks[2] = pf.dwGBitMask;
        m_masks[3] = pf.dwBBitMask;
        for (int i = 0; i < 4; ++i)
        {
            m_channels[i] = {m_masks[i], lowestBit(m_masks[i]), bitCount(m_masks[i])};
        }
        m_valid = true;
    }

    bool Rgb::sameAs(const Rgb& other) const
    {
        return m_valid && other.m_valid && m_bytes == other.m_bytes && std::memcmp(m_masks, other.m_masks, sizeof(m_masks)) == 0;
    }

    uint32_t Rgb::read(const uint8_t* p) const
    {
        switch (m_bytes)
        {
        case 1: return p[0];
        case 2: return *reinterpret_cast<const uint16_t*>(p);
        case 3: return p[0] | (p[1] << 8) | (p[2] << 16);
        default: return *reinterpret_cast<const uint32_t*>(p);
        }
    }

    void Rgb::write(uint8_t* p, uint32_t pixel) const
    {
        switch (m_bytes)
        {
        case 1: p[0] = static_cast<uint8_t>(pixel); break;
        case 2: *reinterpret_cast<uint16_t*>(p) = static_cast<uint16_t>(pixel); break;
        case 3: p[0] = static_cast<uint8_t>(pixel); p[1] = static_cast<uint8_t>(pixel >> 8); p[2] = static_cast<uint8_t>(pixel >> 16); break;
        default: *reinterpret_cast<uint32_t*>(p) = pixel; break;
        }
    }

    uint32_t Rgb::toArgb(uint32_t pixel) const
    {
        uint32_t argb = 0;
        for (int i = 0; i < 4; ++i)
        {
            const Channel& c = m_channels[i];
            uint32_t v8;
            if (!c.bits)
            {
                v8 = i == 0 ? 255 : 0;
            }
            else
            {
                const uint32_t max = c.bits >= 32 ? 0xFFFFFFFF : (1u << c.bits) - 1;
                const uint32_t v = (pixel & c.mask) >> c.shift;
                v8 = c.bits == 8 ? v : static_cast<uint32_t>((uint64_t(v) * 255 + max / 2) / max);
            }
            argb |= v8 << (24 - i * 8);
        }
        return argb;
    }

    uint32_t Rgb::fromArgb(uint32_t argb) const
    {
        uint32_t pixel = 0;
        for (int i = 0; i < 4; ++i)
        {
            const Channel& c = m_channels[i];
            if (!c.bits)
            {
                continue;
            }
            const uint32_t v8 = (argb >> (24 - i * 8)) & 0xFF;
            const uint32_t max = c.bits >= 32 ? 0xFFFFFFFF : (1u << c.bits) - 1;
            const uint32_t v = c.bits == 8 ? v8 : static_cast<uint32_t>((uint64_t(v8) * max + 127) / 255);
            pixel |= (v << c.shift) & c.mask;
        }
        return pixel;
    }

    bool copy(const Image& from, const RECT& src, const Image& to, const RECT& dst, const DDCOLORKEY* colorKey)
    {
        const LONG sw = src.right - src.left, sh = src.bottom - src.top;
        const LONG dw = dst.right - dst.left, dh = dst.bottom - dst.top;
        if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        {
            return true;
        }
        if (blockCompressed(from.format) || blockCompressed(to.format))
        {
            if (from.format != to.format || sw != dw || sh != dh || colorKey || (src.left | src.top | dst.left | dst.top) & 3)
            {
                return false;
            }
            const UINT blockBytes = from.format == d9::D3DFMT_DXT1 ? 8 : 16;
            const UINT rowBytes = ((sw + 3) / 4) * blockBytes;
            for (LONG y = 0; y < (sh + 3) / 4; ++y)
            {
                std::memcpy(to.bits + (dst.top / 4 + y) * to.pitch + (dst.left / 4) * blockBytes,
                    from.bits + (src.top / 4 + y) * from.pitch + (src.left / 4) * blockBytes, rowBytes);
            }
            return true;
        }

        const UINT fromBpp = bitsPerPixel(from.format) / 8, toBpp = bitsPerPixel(to.format) / 8;
        const Rgb fromRgb(from.pf), toRgb(to.pf);
        const bool same = from.format == to.format && from.format != d9::D3DFMT_UNKNOWN;
        if (same && sw == dw && sh == dh && !colorKey && fromBpp)
        {
            for (LONG y = 0; y < sh; ++y)
            {
                std::memmove(to.bits + (dst.top + y) * to.pitch + dst.left * toBpp,
                    from.bits + (src.top + y) * from.pitch + src.left * fromBpp, size_t(sw) * fromBpp);
            }
            return true;
        }
        if (!fromRgb.valid() || !toRgb.valid())
        {
            return false;
        }
        const bool convert = !fromRgb.sameAs(toRgb);
        for (LONG y = 0; y < dh; ++y)
        {
            const LONG sy = src.top + static_cast<LONG>((int64_t(y) * sh) / dh);
            const uint8_t* srcRow = from.bits + sy * from.pitch;
            uint8_t* dstRow = to.bits + (dst.top + y) * to.pitch;
            for (LONG x = 0; x < dw; ++x)
            {
                const LONG sx = src.left + static_cast<LONG>((int64_t(x) * sw) / dw);
                const uint32_t pixel = fromRgb.read(srcRow + sx * fromRgb.bytes());
                if (colorKey && pixel >= colorKey->dwColorSpaceLowValue && pixel <= colorKey->dwColorSpaceHighValue)
                {
                    continue;
                }
                toRgb.write(dstRow + (dst.left + x) * toRgb.bytes(), convert ? toRgb.fromArgb(fromRgb.toArgb(pixel)) : pixel);
            }
        }
        return true;
    }

    void fill(const Image& image, const RECT& rect, uint32_t value)
    {
        const UINT bytes = bitsPerPixel(image.format) / 8;
        const LONG w = rect.right - rect.left;
        if (w <= 0 || !bytes)
        {
            return;
        }
        for (LONG y = rect.top; y < rect.bottom; ++y)
        {
            uint8_t* row = image.bits + y * image.pitch + rect.left * bytes;
            switch (bytes)
            {
            case 1: std::memset(row, static_cast<int>(value & 0xFF), w); break;
            case 2: std::fill_n(reinterpret_cast<uint16_t*>(row), w, static_cast<uint16_t>(value)); break;
            case 4: std::fill_n(reinterpret_cast<uint32_t*>(row), w, value); break;
            default:
                for (LONG x = 0; x < w; ++x)
                {
                    std::memcpy(row + x * bytes, &value, bytes);
                }
                break;
            }
        }
    }
}
