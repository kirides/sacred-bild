#include "game/language.h"
#include "game/sacred_addr.h"
#include "log.h"
#include "patch.h"

#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using namespace Sacred;

    // thiscall on the text table, hooked as fastcall.
    using LoadTextFn = int(__fastcall*)(void* table, void* edx, const char* path);
    using SetPakPathFn = void(__cdecl*)(const char* path);
    using OperatorNewFn = void*(__cdecl*)(size_t size);

    LoadTextFn g_origLoadText = nullptr;
    SetPakPathFn g_origSetPakPath = nullptr;
    // The game's own: the text table's buffer is freed by the game at exit.
    OperatorNewFn g_operatorNew = nullptr;

    constexpr size_t kPakPathSize = 256;            // cMSS_setPakPath's buffer
    constexpr size_t kMaxTextBytes = 64 << 20;      // global.res is about 3 MB

    // Reads a file of at most `limit` bytes, or with `head` its first `limit` bytes (paths relative to the working
    // directory, like the game's); returns ERROR_SUCCESS or the error.
    DWORD readFile(const char* path, std::vector<uint8_t>& out, size_t limit, bool head = false)
    {
        out.clear();
        HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return GetLastError();
        }
        DWORD error = ERROR_SUCCESS;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size))
        {
            error = GetLastError();
        }
        else if (!head && static_cast<uint64_t>(size.QuadPart) > limit)
        {
            error = ERROR_FILE_TOO_LARGE;
        }
        else
        {
            out.resize(static_cast<size_t>(std::min<uint64_t>(size.QuadPart, limit)));
            DWORD read = 0;
            if (!ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr))
            {
                error = GetLastError();
            }
            else if (read != out.size())
            {
                error = ERROR_HANDLE_EOF;
            }
        }
        CloseHandle(file);
        return error;
    }

    bool missing(DWORD error)
    {
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }

    uint32_t u32(const std::vector<uint8_t>& data, size_t offset)
    {
        uint32_t v;
        std::memcpy(&v, data.data() + offset, sizeof(v));
        return v;
    }

    // global.res as the game reads it (Sacred::TextTable): every string inside the file, ids ascending.
    bool isTextTable(const std::vector<uint8_t>& data, uint32_t& count)
    {
        if (data.size() < 4)
        {
            return false;
        }
        count = u32(data, 0);
        if (count == 0 || count > (data.size() - 4) / 16)
        {
            return false;
        }
        uint32_t previous = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            const size_t entry = 4 + static_cast<size_t>(i) * 16;
            const uint32_t id = u32(data, entry);
            const uint64_t end = 4ull + u32(data, entry + 4) + u32(data, entry + 12);
            if ((i > 0 && id <= previous) || end > data.size())
            {
                return false;
            }
            previous = id;
        }
        return true;
    }

    // The sound pak: "SND" and a version byte, then the number of sounds; the game addresses sounds by slot.
    bool soundCount(const char* path, uint32_t& count)
    {
        std::vector<uint8_t> header;
        if (readFile(path, header, 8, true) != ERROR_SUCCESS || header.size() < 8 || std::memcmp(header.data(), "SND", 3) != 0)
        {
            return false;
        }
        count = u32(header, 4);
        return true;
    }

    std::string_view code(int index)
    {
        const char* c = reinterpret_cast<const char*>(Addr::g_languageCodes + index * 16);
        return {c, strnlen(c, 16)};
    }

    // The game's language code, "" if g_language holds no valid index.
    std::string_view currentCode()
    {
        const int index = *reinterpret_cast<const int*>(Addr::g_language);
        return index >= 0 && index < Addr::languageCount ? code(index) : std::string_view{};
    }

    std::string_view trim(std::string_view s)
    {
        const size_t begin = s.find_first_not_of(" \t\r");
        return begin == std::string_view::npos ? std::string_view{} : s.substr(begin, s.find_last_not_of(" \t\r") - begin + 1);
    }

    // LANGUAGE from settings.cfg ("KEY : value" lines); "" if it isn't set.
    std::string settingsLanguage()
    {
        std::vector<uint8_t> file;
        if (readFile("Settings.cfg", file, 1 << 20) != ERROR_SUCCESS)
        {
            return {};
        }
        std::string_view text(reinterpret_cast<const char*>(file.data()), file.size());
        while (!text.empty())
        {
            const size_t end = text.find('\n');
            const std::string_view line = text.substr(0, end);
            text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
            const size_t colon = line.find(':');
            if (colon != std::string_view::npos && trim(line.substr(0, colon)) == "LANGUAGE")
            {
                return std::string(trim(line.substr(colon + 1)));
            }
        }
        return {};
    }

    // The game takes LANGUAGE only as one of its codes exactly as written; anything else silently leaves it at DE.
    void logLanguage()
    {
        const std::string setting = settingsLanguage();
        std::string codes;
        bool known = setting.empty();
        for (int i = 0; i < Addr::languageCount; ++i)
        {
            codes += (i ? " " : "") + std::string(code(i));
            known |= setting == code(i);
        }
        if (!known)
        {
            LOG("Language: settings.cfg has LANGUAGE : {}, which is not one of the game's codes ({}, in upper case); "
                "the game uses {}", setting, codes, currentCode());
        }
        else
        {
            LOG("Language: {} (settings.cfg LANGUAGE : {})", currentCode(), setting);
        }
    }

    // `path` is ".\SCRIPTS\<code>\global.res", which the exe would ignore.
    int __fastcall hookLoadText(void* table, void* edx, const char* path)
    {
        logLanguage();
        std::vector<uint8_t> file;
        const DWORD error = path ? readFile(path, file, kMaxTextBytes) : ERROR_INVALID_PARAMETER;
        uint32_t count = 0;
        if (error == ERROR_SUCCESS && isTextTable(file, count))
        {
            if (void* data = g_operatorNew(file.size()))
            {
                std::memcpy(data, file.data(), file.size());
                auto* t = static_cast<uint8_t*>(table);
                *reinterpret_cast<void**>(t + TextTable::data) = data;
                *reinterpret_cast<uint32_t*>(t + TextTable::size) = static_cast<uint32_t>(file.size());
                LOG("Language: text from {} ({} strings)", path, count);
                return 1;
            }
        }
        if (error == ERROR_SUCCESS)
        {
            LOG("Language: {} is not a text file the game can read, using the exe's own text", path);
        }
        else if (missing(error))
        {
            LOG("Language: no {}, using the exe's own text", path);
        }
        else
        {
            LOG("Language: can't read {} (error {}), using the exe's own text", path ? path : "(null)", error);
        }
        return g_origLoadText(table, edx, path);
    }

    // `path` is ".\PAK\SOUND.PAK"; the language's own is ".\PAK\SOUND.<code>.PAK".
    void __cdecl hookSetPakPath(const char* path)
    {
        const std::string_view original = path ? path : "";
        const size_t dot = original.find_last_of('.');
        const std::string_view language = currentCode();
        if (!language.empty() && dot != std::string_view::npos && original.find_first_of("\\/", dot) == std::string_view::npos)
        {
            const std::string localized = std::string(original.substr(0, dot)) + "." + std::string(language) +
                std::string(original.substr(dot));
            uint32_t count = 0;
            uint32_t ownCount = 0;
            if (localized.size() >= kPakPathSize)
            {
                LOG("Language: {} is too long a path for the game, speech from {}", localized, original);
            }
            else if (GetFileAttributesA(localized.c_str()) == INVALID_FILE_ATTRIBUTES)
            {
                LOG("Language: no {}, speech from {}", localized, original);
            }
            else if (!soundCount(localized.c_str(), count))
            {
                LOG("Language: {} is not a sound file the game can read, speech from {}", localized, original);
            }
            else if (soundCount(path, ownCount) && count != ownCount)
            {
                LOG("Language: {} has {} sounds, {} has {}; speech from {}", localized, count, original, ownCount, original);
            }
            else
            {
                LOG("Language: speech from {}", localized);
                g_origSetPakPath(localized.c_str());
                return;
            }
        }
        g_origSetPakPath(path);
    }

    // The target of the `call rel32` at `site`, 0 if there is none.
    uintptr_t callTarget(uintptr_t site)
    {
        if (!Patch::verify(site, {0xE8}))
        {
            return 0;
        }
        int32_t rel;
        std::memcpy(&rel, reinterpret_cast<const void*>(site + 1), sizeof(rel));
        return site + 5 + rel;
    }
}

void Language::install()
{
    g_operatorNew = reinterpret_cast<OperatorNewFn>(callTarget(Addr::textTableAllocCall));
    if (g_operatorNew)
    {
        Patch::hook(g_origLoadText, Addr::cTextTable_load, &hookLoadText, "cTextTable_load");
    }
    else
    {
        LOG("Language: unexpected code at textTableAllocCall; the text stays the exe's own");
    }
    Patch::hook(g_origSetPakPath, Addr::cMSS_setPakPath, &hookSetPakPath, "cMSS_setPakPath");
}
