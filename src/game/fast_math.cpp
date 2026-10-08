#include "game/fast_math.h"
#include "game/sacred_addr.h"
#include "config/render.h"
#include "log.h"
#include "patch.h"

#include <windows.h>
#include <emmintrin.h>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace
{
    using namespace Sacred;

    bool installFtol()
    {
        if (!Addr::rtFtol || !IsProcessorFeaturePresent(PF_SSE3_INSTRUCTIONS_AVAILABLE))
        {
            return false;
        }
        // The original's first instructions (push ebp; mov ebp, esp; add esp, -0Ch; wait; fnstcw [ebp-2]; ...) and
        // its length up to `leave; ret` (0x27 bytes): the replacement must fit.
        if (!Patch::verify(Addr::rtFtol, {0x55, 0x8B, 0xEC, 0x83, 0xC4, 0xF4, 0x9B, 0xD9, 0x7D, 0xFE}) ||
            !Patch::verify(Addr::rtFtol + 0x25, {0xC9, 0xC3}))
        {
            return false;
        }
        static constexpr uint8_t kCode[] = {
            0x83, 0xEC, 0x08,               // sub esp, 8
            0xDD, 0x0C, 0x24,               // fisttp qword ptr [esp]
            0x8B, 0x04, 0x24,               // mov eax, [esp]
            0x8B, 0x54, 0x24, 0x04,         // mov edx, [esp + 4]
            0x83, 0xC4, 0x08,               // add esp, 8
            0xC3,                           // ret
        };
        return Patch::write(Addr::rtFtol, kCode, sizeof(kCode));
    }

    // sin(x + quarters * pi / 2): reduced to a quarter period around 0, then Taylor polynomials (error below 1e-10
    // there, far below the float the callers keep).
    double sinQuarters(double x, int quarters)
    {
        constexpr double kTwoOverPi = 0.63661977236758134308;
        constexpr double kPiOverTwo = 1.57079632679489661923;
        const int q = _mm_cvtsd_si32(_mm_set_sd(x * kTwoOverPi));     // rounded to nearest
        const double r = x - q * kPiOverTwo;
        const double r2 = r * r;
        switch ((q + quarters) & 3)
        {
        case 0:
            return r * (1 + r2 * (-1 / 6.0 + r2 * (1 / 120.0 + r2 * (-1 / 5040.0 + r2 * (1 / 362880.0 - r2 / 39916800.0)))));
        case 1:
            return 1 + r2 * (-1 / 2.0 + r2 * (1 / 24.0 + r2 * (-1 / 720.0 + r2 * (1 / 40320.0 - r2 / 3628800.0))));
        case 2:
            return -r * (1 + r2 * (-1 / 6.0 + r2 * (1 / 120.0 + r2 * (-1 / 5040.0 + r2 * (1 / 362880.0 - r2 / 39916800.0)))));
        default:
            return -(1 + r2 * (-1 / 2.0 + r2 * (1 / 24.0 + r2 * (-1 / 720.0 + r2 * (1 / 40320.0 - r2 / 3628800.0)))));
        }
    }

    // The ripple grids of cParticleSystem_stargate (the teleporter) and cParticleSystem_stargate_uw: per grid point
    // and frame a height of 5 cos(2 r - t) and a gray level from the ripple's normal (slope from sin(r - t)) against
    // a fixed light, with r the distance from the grid's center. The game computes them with x87 fcos and fsin
    // (~2.6% of the render thread with a teleporter in view); this does the same arithmetic, with the same float
    // roundings, and the polynomials above for the two functions.
    struct RippleConstants
    {
        float centerX, centerY, half, threeHalves, height, minusOne, one, epsilon, light0, light1, light2, scale;
    };
    RippleConstants g_ripple = {};

    // Where the vertex function reads its constants (`fsub/fmul/fadd/fcom dword ptr [address]`): the opcode bytes and
    // the operand's offset, in the order of RippleConstants.
    struct ConstantOperand
    {
        uint8_t opcode[2];
        uint16_t offset;
    };
    constexpr ConstantOperand kRippleOperands[] = {
        {{0xD8, 0x25}, 0x10}, {{0xD8, 0x25}, 0x21}, {{0xD8, 0x0D}, 0x3D}, {{0xD8, 0x2D}, 0x50},
        {{0xD8, 0x0D}, 0x76}, {{0xD8, 0x0D}, 0x89}, {{0xD8, 0x05}, 0xAF}, {{0xD8, 0x15}, 0xB7},
        {{0xD8, 0x0D}, 0xFC}, {{0xD8, 0x0D}, 0x104}, {{0xD8, 0x0D}, 0x10E}, {{0xD8, 0x0D}, 0x11C},
    };
    static_assert(std::size(kRippleOperands) * sizeof(float) == sizeof(RippleConstants));

    void __fastcall hookRippleVertex(uint8_t* self, void*, int column, int row, float time, int index, uint8_t* vertices,
        uint8_t* vertices2)
    {
        const RippleConstants& k = g_ripple;
        const float dx = static_cast<float>(column) - k.centerX;
        const float dy = static_cast<float>(row) - k.centerY;
        // The distance by the game's inverse square root estimate (0x5F3759DF and one Newton step).
        const double d2 = static_cast<double>(dy) * dy + static_cast<double>(dx) * dx;
        const float d2f = static_cast<float>(d2);
        int32_t bits;
        std::memcpy(&bits, &d2f, sizeof(bits));
        bits = 0x5F3759DF - (bits >> 1);
        float y0;
        std::memcpy(&y0, &bits, sizeof(y0));
        const float r = static_cast<float>((k.threeHalves - d2 * k.half * y0 * y0) * y0 * d2);

        const float cosArg = static_cast<float>(2.0 * r - time);
        const float c = static_cast<float>(sinQuarters(cosArg, 1));
        const float height = static_cast<float>(static_cast<double>(c) * k.height);
        const double s = sinQuarters(static_cast<double>(r) - time, 0);
        const float nx = static_cast<float>(s * dx * k.minusOne / r);
        const double nyFull = s * dy * k.minusOne / r;
        const float ny = static_cast<float>(nyFull);
        const double length = std::sqrt(nyFull * ny + static_cast<double>(nx) * nx + k.one);
        double light = 0.0;
        if (!(length < k.epsilon))
        {
            const float inv = static_cast<float>(k.one / length);
            light = static_cast<double>(ny) * inv * k.light0 + static_cast<double>(inv) * k.minusOne * k.light1 +
                static_cast<double>(inv) * nx * k.light2;
        }
        const auto level = static_cast<uint32_t>(static_cast<int32_t>((light + k.one) * k.scale));
        const uint32_t gray = level | level << 8 | level << 16;

        uint8_t* v = vertices + size_t(index) * 32;
        uint8_t* v2 = vertices2 + size_t(index) * 32;
        std::memcpy(v2 + 4, &height, sizeof(height));
        std::memcpy(v + 4, &height, sizeof(height));
        const auto field = [self](size_t offset) { return *reinterpret_cast<const uint32_t*>(self + offset); };
        const uint32_t table = field(0x20CC);
        const uint32_t cell = field(0xFE78 + size_t(index) * 4);
        uint32_t color = gray, color2 = gray;
        if (table <= 15 && cell <= 15)
        {
            color = field(0x10918 + (size_t(table) * 16 + cell) * 4) | gray;
            color2 = field(0x10918 + (size_t(field(0x20D0)) * 16 + cell) * 4) | gray;
        }
        std::memcpy(v + 16, &color, sizeof(color));
        std::memcpy(v2 + 16, &color2, sizeof(color2));
    }

    // The vertex function called from vtable slot 2 of `vtable` (at +0x1A8), checked; 0 if it isn't the one expected.
    uintptr_t rippleCall(uintptr_t vtable)
    {
        if (!vtable)
        {
            return 0;
        }
        const uintptr_t update = *reinterpret_cast<const uintptr_t*>(vtable + 8);
        const uintptr_t site = update + 0x1A8;
        if (!Patch::verify(site, {0xE8}))
        {
            return 0;
        }
        const uintptr_t target = site + 5 + *reinterpret_cast<const int32_t*>(site + 1);
        if (!Patch::verify(target, {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14, 0xDB, 0x45, 0x08}) ||
            !Patch::verify(target + 0x14, {0xB9, 0xDF, 0x59, 0x37, 0x5F}) ||
            !Patch::verify(target + 0x145, {0x8B, 0x96, 0xCC, 0x20, 0x00, 0x00}) ||
            !Patch::verify(target + 0x154, {0x8B, 0xBC, 0xBE, 0x78, 0xFE, 0x00, 0x00}) ||
            !Patch::verify(target + 0x165, {0x8B, 0x94, 0x96, 0x18, 0x09, 0x01, 0x00}) ||
            !Patch::verify(target + 0x172, {0x8B, 0x96, 0xD0, 0x20, 0x00, 0x00}))
        {
            return 0;
        }
        for (const ConstantOperand& o : kRippleOperands)
        {
            if (!Patch::verify(target + o.offset - 2, {o.opcode[0], o.opcode[1]}))
            {
                return 0;
            }
        }
        return site;
    }

    int installRipple()
    {
        const uintptr_t sites[] = {rippleCall(Addr::cParticleSystemStargate_vtable),
            rippleCall(Addr::cParticleSystemStargateUw_vtable)};
        int installed = 0;
        for (const uintptr_t site : sites)
        {
            if (!site)
            {
                continue;
            }
            const uintptr_t target = site + 5 + *reinterpret_cast<const int32_t*>(site + 1);
            if (!installed)
            {
                float* out = &g_ripple.centerX;
                for (const ConstantOperand& o : kRippleOperands)
                {
                    *out++ = **reinterpret_cast<const float* const*>(target + o.offset);
                }
            }
            installed += Patch::redirectCall(site, reinterpret_cast<const void*>(&hookRippleVertex));
        }
        return installed;
    }
}

void FastMath::install()
{
    if (!Config::render.fastMath)
    {
        return;
    }
    if (installFtol())
    {
        LOG("Fast math: __ftol converts with fisttp");
    }
    if (const int ripples = installRipple())
    {
        LOG("Fast math: {} ripple grid{} (teleporter) without x87 fsin/fcos", ripples, ripples == 1 ? "" : "s");
    }
}
