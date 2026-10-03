#pragma once
#include <format>
#include <string_view>

namespace Log
{
    void init(const wchar_t* path);
    void write(std::string_view line);

    template <class... Args>
    void info(std::format_string<Args...> fmt, Args&&... args)
    {
        write(std::format(fmt, std::forward<Args>(args)...));
    }
}

#define LOG(...) ::Log::info(__VA_ARGS__)
