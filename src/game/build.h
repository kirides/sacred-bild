#pragma once
#include <cstdint>

namespace Sacred
{
    // True when the host exe is the build our address table was made for.
    bool isSupportedBuild();

    // Logs the host exe's PE header and compares it with the build an address table was made for.
    bool hostExeIs(uint32_t timestamp, uint32_t sizeOfImage, uint32_t entryPoint);

    // Installs all game hooks; call once from DllMain after the build check.
    void installHooks();
}
