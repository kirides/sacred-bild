#pragma once
#include <format>
#include <string>
#include <string_view>
#include <type_traits>

// std::format with the formatting engine compiled once (fmt.cpp) instead of in every translation unit that formats
// something, which made up a large part of a fresh build. The format string is still checked at compile time.
// Arguments are checked as their decayed types, so string literals of different lengths and lvalues vs. rvalues
// share one instantiation.
namespace Fmt
{
    std::string vformat(std::string_view fmt, std::format_args args);
    std::wstring vformat(std::wstring_view fmt, std::wformat_args args);

    template <class... Args>
    std::string format(std::format_string<std::decay_t<Args>...> fmt, Args&&... args)
    {
        return vformat(fmt.get(), std::make_format_args(args...));
    }

    template <class... Args>
    std::wstring format(std::wformat_string<std::decay_t<Args>...> fmt, Args&&... args)
    {
        return vformat(fmt.get(), std::make_wformat_args(args...));
    }
}
