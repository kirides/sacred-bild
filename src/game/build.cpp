#include "game/build.h"
#include "game/sacred_de.h"
#include "game/frame_hooks.h"
#include "game/granny_async.h"
#include "game/map_cache.h"
#include "game/resolution.h"
#include "game/ui_canvas.h"
#include "net/connection.h"
#include "net/lan_client.h"
#include "log.h"
#include "patch.h"

#include <windows.h>

bool Sacred::isSupportedBuild()
{
    return hostExeIs(Addr::kTimestamp, Addr::kSizeOfImage, Addr::kEntryPoint);
}

bool Sacred::hostExeIs(uint32_t timestamp, uint32_t sizeOfImage, uint32_t entryPoint)
{
    const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    const auto ts = nt->FileHeader.TimeDateStamp;
    const auto size = nt->OptionalHeader.SizeOfImage;
    const auto entry = nt->OptionalHeader.AddressOfEntryPoint;
    LOG("Host exe: timestamp={:08x} sizeOfImage={:08x} entry={:08x}", ts, size, entry);
    return reinterpret_cast<uintptr_t>(base) == 0x400000 && ts == timestamp && size == sizeOfImage &&
        entry == entryPoint;
}

void Sacred::installHooks()
{
    GrannyAsync::install();
    Patch::begin();
    FrameHooks::install();
    Resolution::install();
    UiCanvas::install();
    MapCache::install();
    if (Patch::commit())
    {
        LOG("Game hooks installed");
    }
    Connection::install();
    LanClient::install();
}
