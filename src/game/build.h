#pragma once

namespace Sacred
{
    // Logs the host exe's build and finds the game addresses (Sacred::Addr) from their signatures; true if every
    // one was found.
    bool resolveAddresses();

    // Installs all game hooks; call once from DllMain after resolveAddresses().
    void installHooks();

    // Logs the host exe's PE timestamp, naming the build if the signatures were checked against it.
    void logHostExe();
}
