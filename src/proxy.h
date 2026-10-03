#pragma once
#include <string>

namespace Proxy
{
    // Loads the chained ddraw (DDrawCompat or the system DLL) and resolves our forwarded exports.
    bool init(const std::wstring& gameDir);
}
