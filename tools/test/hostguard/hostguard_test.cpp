// HostGuard::classify (src/core/host_guard.*): which exes Sacred Gear may run in. Builds small PE images in memory.
// Exit code 0 = all checks passed.
#include "host_guard.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
    int g_failed = 0;

    // A minimal 32-bit image: headers and one readable ".data" section holding `banner` (may be empty).
    std::vector<uint8_t> makeImage(DWORD stamp, const char* banner)
    {
        std::vector<uint8_t> img(0x2000, 0);
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(img.data());
        dos->e_magic = IMAGE_DOS_SIGNATURE;
        dos->e_lfanew = 0x80;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(img.data() + 0x80);
        nt->Signature = IMAGE_NT_SIGNATURE;
        nt->FileHeader.NumberOfSections = 1;
        nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER32);
        nt->FileHeader.TimeDateStamp = stamp;
        auto* section = IMAGE_FIRST_SECTION(nt);
        std::memcpy(section->Name, ".data", 6);
        section->VirtualAddress = 0x1000;
        section->Misc.VirtualSize = 0x1000;
        section->Characteristics = IMAGE_SCN_MEM_READ;
        std::memcpy(img.data() + 0x1000 + 0x100, banner, std::strlen(banner));
        return img;
    }

    void expect(const wchar_t* path, const std::vector<uint8_t>& image, HostGuard::Host want, const char* what)
    {
        const HostGuard::Verdict v = HostGuard::classify(path, image.data());
        if (v.host != want)
        {
            std::printf("FAIL %s: got %d (%s), want %d\n", what, static_cast<int>(v.host), v.reason, static_cast<int>(want));
            ++g_failed;
        }
    }
}

int main()
{
    using H = HostGuard::Host;
    const auto stock = makeImage(0x452F85C7, " Compiled on Oct 13 2006 at 14:26:24");
    const auto closed = makeImage(0x43146D12, " Compiled on Aug 30 2005 at 15:47:06");
    const auto restamped = makeImage(0x12345678, "xx Compiled on Aug 30 2005 at 15:47:06 xx");

    expect(L"C:/Games/Sacred/sacred.exe", stock, H::Game, "stock sacred.exe");
    expect(L"C:/Games/Sacred/SacredReborn.exe", stock, H::Game, "SacredReborn.exe");
    expect(L"C:/Games/Sacred/GameServer.exe", stock, H::GameServer, "gameserver");
    expect(L"C:/Games/Sacred/Config.exe", stock, H::Other, "other exe");
    expect(L"C:\\Games\\Sacred\\sacred.exe", stock, H::Game, "backslash separators");
    // Directory names do not matter, only the exe's own name.
    expect(L"C:/Games/sacred-closednet/sacred.exe", stock, H::Game, "directory with the word in it");
    // The third-party exe, by each independent sign.
    expect(L"C:/Games/Sacred/SacredClosedNet.exe", stock, H::ThirdParty, "by file name");
    expect(L"C:/Games/Sacred/sacred.exe", closed, H::ThirdParty, "renamed to sacred.exe (timestamp)");
    expect(L"C:/Games/Sacred/SacredReborn.exe", closed, H::ThirdParty, "renamed to SacredReborn.exe (timestamp)");
    expect(L"C:/Games/Sacred/sacred.exe", restamped, H::ThirdParty, "renamed and re-stamped (banner string)");
    expect(L"C:/Games/Sacred/whatever.exe", closed, H::ThirdParty, "any other name (timestamp)");
    expect(L"C:/Games/Sacred/sacred.exe", std::vector<uint8_t>(), H::Game, "no image given is not a finding");
    std::printf(g_failed ? "%d failed\n" : "all passed\n", g_failed);
    return g_failed ? 1 : 0;
}
