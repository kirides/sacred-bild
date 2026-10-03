#pragma once

namespace Sacred
{
    // True when the host exe is the build our address table was made for.
    bool isSupportedBuild();

    // Installs all game hooks; call once from DllMain after the build check.
    void installHooks();
}
