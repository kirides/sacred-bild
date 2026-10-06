#pragma once
#include "fmt.h"

#include <string_view>

// Timestamped lines in SacredBild.log (SacredBild-server.log in gameserver.exe), from any thread.
namespace Log
{
    void init(const wchar_t* path);
    void write(std::string_view line);

    template <class... Args>
    void info(std::format_string<std::decay_t<Args>...> fmt, Args&&... args)
    {
        write(Fmt::vformat(fmt.get(), std::make_format_args(args...)));
    }
}

#define LOG(...) ::Log::info(__VA_ARGS__)
