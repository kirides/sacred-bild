#include "game/build.h"
#include "game/sacred_de.h"
#include "game/frame_hooks.h"
#include "game/resolution.h"
#include "game/ui_canvas.h"
#include "log.h"
#include "patch.h"

#include <windows.h>

bool Sacred::isSupportedBuild()
{
    const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    const auto ts = nt->FileHeader.TimeDateStamp;
    const auto size = nt->OptionalHeader.SizeOfImage;
    const auto entry = nt->OptionalHeader.AddressOfEntryPoint;
    LOG("Host exe: timestamp={:08x} sizeOfImage={:08x} entry={:08x}", ts, size, entry);
    return reinterpret_cast<uintptr_t>(base) == 0x400000 && ts == Addr::kTimestamp &&
        size == Addr::kSizeOfImage && entry == Addr::kEntryPoint;
}

void Sacred::installHooks()
{
    Patch::begin();
    FrameHooks::install();
    Resolution::install();
    UiCanvas::install();
    if (Patch::commit())
    {
        LOG("Game hooks installed");
    }
}
