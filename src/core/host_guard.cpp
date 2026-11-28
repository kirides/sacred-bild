#include "host_guard.h"

#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <cstring>
#include <string>

namespace
{
    // Builds Gear must never run in, by the PE header's build timestamp.
    constexpr DWORD kThirdPartyStamps[] = {
        0x43146D12,     // SacredClosedNet.exe: " Compiled on Aug 30 2005 at 15:47:06"
    };

    // The build banner the exe carries as a string, for the same builds (survives a rename and a re-stamped header).
    constexpr char kThirdPartyBanners[][40] = {
        " Compiled on Aug 30 2005 at 15:47:06",
    };

    // Words in the file name that mark a third-party exe.
    constexpr const wchar_t* kThirdPartyNames[] = {L"closednet"};

    std::wstring lower(std::wstring_view s)
    {
        std::wstring out(s);
        for (wchar_t& c : out)
        {
            c = static_cast<wchar_t>(std::towlower(c));
        }
        return out;
    }

    std::wstring fileName(std::wstring_view path)
    {
        const size_t slash = path.find_last_of(L"\\/");
        return lower(slash == std::wstring_view::npos ? path : path.substr(slash + 1));
    }

    bool hasBanner(const uint8_t* base, const IMAGE_NT_HEADERS32* nt)
    {
        const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
        {
            if (std::memcmp(section->Name, ".rsrc", 6) == 0 || !(section->Characteristics & IMAGE_SCN_MEM_READ))
            {
                continue;
            }
            const char* first = reinterpret_cast<const char*>(base + section->VirtualAddress);
            const char* last = first + section->Misc.VirtualSize;
            for (const auto& banner : kThirdPartyBanners)
            {
                const size_t n = std::strlen(banner);
                if (std::search(first, last, banner, banner + n) != last)
                {
                    return true;
                }
            }
        }
        return false;
    }
}

HostGuard::Verdict HostGuard::classify(std::wstring_view exePath, const void* imageBase)
{
    const std::wstring name = fileName(exePath);
    for (const wchar_t* word : kThirdPartyNames)
    {
        if (name.find(word) != std::wstring::npos)
        {
            return {Host::ThirdParty, "the exe's file name marks a third-party build"};
        }
    }
    if (imageBase)
    {
        const auto* base = static_cast<const uint8_t*>(imageBase);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE)
        {
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE)
            {
                if (std::find(std::begin(kThirdPartyStamps), std::end(kThirdPartyStamps), nt->FileHeader.TimeDateStamp) !=
                    std::end(kThirdPartyStamps))
                {
                    return {Host::ThirdParty, "the exe's build timestamp is a third-party build's"};
                }
                if (hasBanner(base, nt))
                {
                    return {Host::ThirdParty, "the exe carries a third-party build's banner string"};
                }
            }
        }
    }
    if (name == L"sacred.exe" || name == L"sacredreborn.exe")
    {
        return {Host::Game, "the game"};
    }
    if (name == L"gameserver.exe")
    {
        return {Host::GameServer, "the game's server"};
    }
    return {Host::Other, "not one of the game's exe names"};
}

const HostGuard::Verdict& HostGuard::verdict()
{
    static const Verdict v = []
    {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        return classify(path, GetModuleHandleW(nullptr));
    }();
    return v;
}

bool HostGuard::gearAllowed()
{
    const Host h = verdict().host;
    return h == Host::Game || h == Host::GameServer;
}
