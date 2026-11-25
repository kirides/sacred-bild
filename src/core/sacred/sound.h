#pragma once
#include "game/sacred_addr.h"

#include <windows.h>

#include <cstddef>

// Sound: the sound system's lock.
namespace Sacred
{
    // A kernel mutex in a small wrapper: constructor ENG 0066F9C0 (CreateMutexA(nullptr, FALSE, nullptr)), close
    // 0066F9E0, lock 0066FA00 (WaitForSingleObject, INFINITE), unlock 0066FA10 (ReleaseMutex). The sound system
    // (one instance, made by ENG 006770E0) holds the only one at +0x60 and takes it in every call into its state,
    // among them the sound commands (ENG 0067A7A0) the object passes issue for each object, every frame.
    struct cMutex
    {
        HANDLE handle;
    };
    static_assert(sizeof(cMutex) == 4);
}
