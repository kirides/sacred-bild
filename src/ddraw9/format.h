#pragma once
#include "ddraw9/d3d9_api.h"

#include <cstdint>
#include <string>

// DirectDraw pixel formats <-> Direct3D 9 formats, and pixel copies between plain RGB formats.
namespace DDraw9::Format
{
    // D3DFMT_UNKNOWN if Direct3D 9 has no matching format.
    d9::D3DFORMAT fromPixelFormat(const DDPIXELFORMAT& pf);
    DDPIXELFORMAT toPixelFormat(d9::D3DFORMAT format);

    UINT bitsPerPixel(d9::D3DFORMAT format);    // DXT1: 4, DXT2-5: 8
    bool blockCompressed(d9::D3DFORMAT format);
    bool depth(d9::D3DFORMAT format);
    bool stencil(d9::D3DFORMAT format);
    // Bytes per row of a tightly packed surface (per row of 4x4 blocks for DXT) and the number of such rows.
    UINT pitch(d9::D3DFORMAT format, UINT width);
    UINT rows(d9::D3DFORMAT format, UINT height);
    std::string name(d9::D3DFORMAT format);

    // A plain RGB format (8 to 32 bits per pixel, optional alpha) described by its masks. Pixels convert through
    // 8-bit ARGB.
    class Rgb
    {
    public:
        explicit Rgb(const DDPIXELFORMAT& pf);
        bool valid() const { return m_valid; }
        UINT bytes() const { return m_bytes; }
        bool sameAs(const Rgb& other) const;
        uint32_t read(const uint8_t* p) const;
        void write(uint8_t* p, uint32_t pixel) const;
        uint32_t toArgb(uint32_t pixel) const;
        uint32_t fromArgb(uint32_t argb) const;

    private:
        struct Channel
        {
            uint32_t mask = 0;
            int shift = 0;
            int bits = 0;
        };
        Channel m_channels[4];  // a, r, g, b
        uint32_t m_masks[4] = {};
        UINT m_bytes = 0;
        bool m_valid = false;
    };

    struct Image
    {
        uint8_t* bits = nullptr;
        int pitch = 0;
        UINT width = 0, height = 0;
        d9::D3DFORMAT format = d9::D3DFMT_UNKNOWN;
        DDPIXELFORMAT pf = {};
    };

    // Copies `src` of `from` to `dst` of `to`, converting between plain RGB formats and stretching with nearest
    // sampling. Pixels matching `colorKey` (inclusive range, in the source format) are skipped. Block-compressed
    // formats copy only between identical formats and sizes, on block boundaries.
    bool copy(const Image& from, const RECT& src, const Image& to, const RECT& dst, const DDCOLORKEY* colorKey);
    // Fills `rect` with a pixel value in the image's format.
    void fill(const Image& image, const RECT& rect, uint32_t value);
}
