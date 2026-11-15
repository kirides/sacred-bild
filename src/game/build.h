#pragma once

namespace Sacred
{
    // Logs the host exe's build and finds the game addresses (Sacred::Addr) from their signatures; true if every
    // one was found.
    bool resolveAddresses();

    // Installs all game hooks; call once from DllMain after resolveAddresses().
    void installHooks();

    // True when the host is Sacred ReBorn's exe: the German build with its code patched. It has its own
    // resolution patches (SR_WIDTH / SR_HEIGHT in Settings.cfg) and loads the language files from disk; valid after resolveAddresses().
    bool reBorn();

    // Logs the host exe's PE timestamp, naming the build if the signatures were checked against it.
    void logHostExe();
}
