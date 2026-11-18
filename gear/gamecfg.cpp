#include "gamecfg.h"

#include <windows.h>

#include <cstdint>

namespace
{
    std::map<std::string, std::string> g_values;
    uint64_t g_stamp = 0;
    bool g_read = false;

    std::string trim(const std::string& s)
    {
        size_t a = 0, b = s.size();
        while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n'))
        {
            ++a;
        }
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n'))
        {
            --b;
        }
        return s.substr(a, b - a);
    }
}

const std::map<std::string, std::string>& GameCfg::all()
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(L"Settings.cfg", GetFileExInfoStandard, &data))
    {
        g_values.clear();
        g_read = false;
        return g_values;
    }
    const uint64_t stamp = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                           data.ftLastWriteTime.dwLowDateTime;
    if (g_read && stamp == g_stamp)
    {
        return g_values;
    }
    g_values.clear();
    g_stamp = stamp;
    g_read = true;
    HANDLE file = CreateFileW(L"Settings.cfg", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return g_values;
    }
    std::string text;
    char buffer[4096];
    DWORD got = 0;
    while (text.size() < (1u << 20) && ReadFile(file, buffer, sizeof(buffer), &got, nullptr) && got)
    {
        text.append(buffer, got);
    }
    CloseHandle(file);
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos)
        {
            end = text.size();
        }
        const std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        const size_t colon = line.find(':');
        if (colon == std::string::npos)
        {
            continue;
        }
        const std::string key = trim(line.substr(0, colon));
        if (!key.empty())
        {
            g_values[key] = trim(line.substr(colon + 1));
        }
    }
    return g_values;
}
