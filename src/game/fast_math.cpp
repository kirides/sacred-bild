#include "game/fast_math.h"
#include "game/sacred_addr.h"
#include "config/render.h"
#include "log.h"
#include "patch.h"

#include <windows.h>
#include <cstdint>

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
}
