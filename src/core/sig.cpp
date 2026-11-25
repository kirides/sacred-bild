#include "sig.h"
#include "log.h"

#include <windows.h>
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

    // The host exe's code: the first executable section.
    std::span<const uint8_t> codeSection()
    {
        const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        const auto* section = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
        {
            if (section->Characteristics & IMAGE_SCN_MEM_EXECUTE)
            {
                return {base + section->VirtualAddress, section->Misc.VirtualSize};
            }
        }
        return {};
    }
}

size_t Sig::resolve(std::span<const Entry> entries)
{
    const auto code = codeSection();
    return resolve(entries, code, reinterpret_cast<uintptr_t>(code.data()));
}

size_t Sig::resolve(std::span<const Entry> entries, std::span<const uint8_t> code, uintptr_t codeVa)
{
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

    const uint8_t* data = code.data();
    const size_t size = code.size();
    for (size_t at = 0; at < size; ++at)
    {
        for (const uint32_t i : byFirstByte[data[at]])
        {
            Pattern& p = patterns[i];
            if (at + p.bytes.size() <= size && matches(data + at, p) && ++p.hits == 1)
            {
                p.hit = at;
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
        const size_t at = p.hit + e.offset;
        if (e.take == Take::Abs32)
        {
            uint32_t value;
            std::memcpy(&value, data + at, 4);
            *e.out.address = value;
        }
        else
        {
            *e.out.address = codeVa + at;
        }
    }
    return failed;
}
