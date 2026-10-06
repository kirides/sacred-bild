#include "fmt.h"

std::string Fmt::vformat(std::string_view fmt, std::format_args args)
{
    return std::vformat(fmt, args);
}

std::wstring Fmt::vformat(std::wstring_view fmt, std::wformat_args args)
{
    return std::vformat(fmt, args);
}
