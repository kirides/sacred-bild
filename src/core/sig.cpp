#include "sig.h"
#include "log.h"

#include <windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace
{
    struct Pattern
    {
        std::vector<uint8_t> bytes;
        std::vector<uint8_t> fixed;     // 1 = the byte has to match
        uintptr_t hit = 0;
        int hits = 0;
    };

    int hexDigit(char c)
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    }

    bool parse(const char* text, Pattern& out)
    {
        for (const char* p = text; *p;)
        {
            if (*p == ' ')
            {
                ++p;
                continue;
            }
            if (p[0] == '?' && p[1] == '?')
            {
                out.bytes.push_back(0);
                out.fixed.push_back(0);
            }
            else
            {
                const int hi = hexDigit(p[0]), lo = p[1] ? hexDigit(p[1]) : -1;
                if (hi < 0 || lo < 0)
                {
                    return false;
                }
                out.bytes.push_back(static_cast<uint8_t>(hi << 4 | lo));
                out.fixed.push_back(1);
            }
            p += 2;
        }
        return !out.bytes.empty() && out.fixed[0];
    }

    bool matches(const uint8_t* at, const Pattern& p)
    {
        for (size_t i = 1; i < p.bytes.size(); ++i)
        {
            if (p.fixed[i] && at[i] != p.bytes[i])
            {
                return false;
            }
        }
        return true;
    }

    // The host exe's code: the first executable section, and its address in the running image.
    struct Code
    {
        std::span<const uint8_t> bytes;
        uintptr_t va = 0;
    };

    const IMAGE_SECTION_HEADER* codeHeader(const IMAGE_NT_HEADERS* nt)
    {
        const auto* section = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
        {
            if (section->Characteristics & IMAGE_SCN_MEM_EXECUTE)
            {
                return section;
            }
        }
        return nullptr;
    }

    // Whether the file's copy of the code is the code that runs, give or take the few patches of other modules: nearly every
    // 4 KiB block of both is (almost) the same. Encrypted or packed code on disk differs in almost every byte.
    bool resemblesMemory(const std::vector<uint8_t>& file, const uint8_t* memory)
    {
        constexpr size_t kBlock = 4096;
        size_t blocks = 0, same = 0;
        for (size_t at = 0; at < file.size(); at += kBlock)
        {
            const size_t n = std::min(kBlock, file.size() - at);
            size_t different = 0;
            for (size_t i = 0; i < n; ++i)
            {
                different += file[at + i] != memory[at + i];
            }
            ++blocks;
            same += different * 20 <= n;    // up to 5% of a block patched
        }
        return blocks && same * 10 >= blocks * 9;
    }

    // The code section as the exe file on disk has it: what the game's code was before any module patched it. Hooks of
    // another DLL (SacredBild's, Sacred Gear's, a third one's) put jumps over the first bytes of functions; a signature
    // searched in memory would then miss the functions the other module hooked first, depending on the load order. The
    // exe is not relocated (no ASLR), so an address found in the file's copy is valid in memory. Read once per module;
    // nothing if the file cannot be read or is not the image that runs (then the caller searches memory).
    const std::vector<uint8_t>& diskCode(const IMAGE_NT_HEADERS* running, const IMAGE_SECTION_HEADER* section)
    {
        static std::vector<uint8_t> code;
        static bool tried = false;
        if (tried)
        {
            return code;
        }
        tried = true;
        wchar_t path[MAX_PATH] = {};
        if (!GetModuleFileNameW(nullptr, path, MAX_PATH))
        {
            return code;
        }
        HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            LOG("Signatures: cannot open the exe ({}), searching memory", GetLastError());
            return code;
        }
        auto readAt = [&](uint32_t offset, void* out, uint32_t size) {
            OVERLAPPED at{};
            at.Offset = offset;
            DWORD got = 0;
            return ReadFile(file, out, size, &got, &at) && got == size;
        };
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS nt{};
        std::vector<uint8_t> bytes;
        const bool ok = readAt(0, &dos, sizeof(dos)) && dos.e_magic == IMAGE_DOS_SIGNATURE && readAt(dos.e_lfanew, &nt, sizeof(nt)) &&
                        nt.Signature == IMAGE_NT_SIGNATURE && nt.FileHeader.TimeDateStamp == running->FileHeader.TimeDateStamp &&
                        nt.OptionalHeader.ImageBase == running->OptionalHeader.ImageBase && section->SizeOfRawData > 0 &&
                        (bytes.resize(section->Misc.VirtualSize), readAt(section->PointerToRawData, bytes.data(),
                                                                          std::min<uint32_t>(section->SizeOfRawData, section->Misc.VirtualSize)));
        CloseHandle(file);
        if (ok && !resemblesMemory(bytes, reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr)) + section->VirtualAddress))
        {
            // A wrapped exe (Steam's SteamStub) keeps .text encrypted on disk and decrypts it into memory: the file's bytes
            // are noise, and a pattern found there would be a coincidence.
            LOG("Signatures: the exe's code on disk differs from memory throughout (a wrapped exe), searching memory");
            return code;
        }
        if (ok)
        {
            code = std::move(bytes);
        }
        else
        {
            LOG("Signatures: the exe file is not the running image or cannot be read, searching memory");
        }
        return code;
    }

    Code codeSection()
    {
        const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        const IMAGE_SECTION_HEADER* section = codeHeader(nt);
        if (!section)
        {
            return {};
        }
        const uintptr_t va = reinterpret_cast<uintptr_t>(base) + section->VirtualAddress;
        if (const auto& disk = diskCode(nt, section); !disk.empty())
        {
            return {disk, va};
        }
        return {{base + section->VirtualAddress, section->Misc.VirtualSize}, va};
    }
}

namespace
{
    size_t resolveIn(std::span<const Sig::Entry> entries, std::span<const uint8_t> code, uintptr_t codeVa, const uint8_t* live);
}

size_t Sig::resolve(std::span<const Entry> entries)
{
    const Code code = codeSection();
    // the file's copy: also compare each hit with memory (a difference is another module's patch, logged)
    const auto* live = reinterpret_cast<const uint8_t*>(code.va);
    return resolveIn(entries, code.bytes, code.va, code.bytes.data() == live ? nullptr : live);
}

size_t Sig::resolve(std::span<const Entry> entries, std::span<const uint8_t> code, uintptr_t codeVa)
{
    return resolveIn(entries, code, codeVa, nullptr);
}

namespace
{
size_t resolveIn(std::span<const Sig::Entry> entries, std::span<const uint8_t> code, uintptr_t codeVa, const uint8_t* live)
{
    using namespace Sig;
    std::vector<Pattern> patterns(entries.size());
    std::array<std::vector<uint32_t>, 256> byFirstByte;
    for (size_t i = 0; i < entries.size(); ++i)
    {
        if (parse(entries[i].pattern, patterns[i]))
        {
            byFirstByte[patterns[i].bytes[0]].push_back(static_cast<uint32_t>(i));
        }
        else
        {
            LOG("Signature {}: invalid pattern", entries[i].name);
        }
    }

    auto search = [&](const uint8_t* data, size_t size, const std::vector<uint32_t>& which) {
        std::array<std::vector<uint32_t>, 256> byFirst;
        for (const uint32_t i : which)
        {
            patterns[i].hits = 0;
            byFirst[patterns[i].bytes[0]].push_back(i);
        }
        for (size_t at = 0; at < size; ++at)
        {
            for (const uint32_t i : byFirst[data[at]])
            {
                Pattern& p = patterns[i];
                if (at + p.bytes.size() <= size && matches(data + at, p) && ++p.hits == 1)
                {
                    p.hit = at;
                }
            }
        }
    };
    std::vector<uint32_t> all;
    for (const auto& list : byFirstByte)
    {
        all.insert(all.end(), list.begin(), list.end());
    }
    search(code.data(), code.size(), all);

    // Entries the file's copy does not resolve exactly once are looked up in memory, which may be all that matches (the file
    // differs there, or the exe is not the one the file's copy belongs to).
    std::vector<const uint8_t*> source(entries.size(), code.data());
    if (live)
    {
        std::vector<uint32_t> retry;
        for (const uint32_t i : all)
        {
            if (patterns[i].hits != 1)
            {
                retry.push_back(i);
            }
        }
        if (!retry.empty())
        {
            search(live, code.size(), retry);
            for (const uint32_t i : retry)
            {
                if (patterns[i].hits == 1)
                {
                    LOG("Signature {}: not in the exe file, found in memory", entries[i].name);
                    source[i] = live;
                }
            }
        }
    }

    size_t failed = 0;
    for (size_t i = 0; i < entries.size(); ++i)
    {
        const Entry& e = entries[i];
        const Pattern& p = patterns[i];
        if (p.hits != 1)
        {
            LOG("Signature {}: {} matches", e.name, p.hits);
            *e.out.address = 0;
            ++failed;
            continue;
        }
        if (live && source[i] != live && (live[p.hit] != p.bytes[0] || !matches(live + p.hit, p)))
        {
            // The bytes in memory are not the file's any more: something patched them before this module looked (the other
            // of SacredBild / Sacred Gear, or a third DLL). The address comes from the file and is right; a patch of ours
            // there would meet the other one.
            size_t first = 0;
            while (first < p.bytes.size() && !(p.fixed[first] && live[p.hit + first] != p.bytes[first]))
            {
                ++first;
            }
            LOG("Signature {}: warning, the code at {:08X} is patched in memory (first changed byte +{}): another module hooked it first",
                e.name, codeVa + p.hit, first);
        }
        const size_t at = p.hit + e.offset;
        if (e.take == Take::Abs32)
        {
            uint32_t value;
            std::memcpy(&value, source[i] + at, 4);
            *e.out.address = value;
        }
        else if (e.take == Take::Rel32)
        {
            int32_t rel;
            std::memcpy(&rel, source[i] + at, 4);
            *e.out.address = codeVa + at + 4 + static_cast<uintptr_t>(static_cast<intptr_t>(rel));
        }
        else
        {
            *e.out.address = codeVa + at;
        }
    }
    return failed;
}
}
