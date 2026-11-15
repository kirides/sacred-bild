#include "save.h"
#include "ui.h"
#include "world.h"
#include "game/gear_addr.h"

#include <windows.h>

#include <cstdio>
#include <deque>
#include <string_view>

namespace
{
    namespace A = Gear::Addr;

    constexpr uintptr_t kEngineFlags = 0x54;    // dword: 0x40 / 0x80 save queued / being written, 0x200 load queued
    constexpr uint32_t kSaveBits = 0xC0, kBusyBits = 0x2C0;
    constexpr uintptr_t kNetPlayersMulti = 4;   // byte at g_pNetPlayers + 4: 0 in single player
    constexpr size_t kNameMax = 60;             // the engine copies the name unbounded into 64 bytes (engine+0x6A)

    // The savegame window's event (0x24 bytes, built by the factory 0x8080A0): vtable, code 0xF, sub 2 (save).
    struct SaveEvent
    {
        const void* vtable;
        uint32_t code;          // +4, what vtable slot 1 returns
        uint32_t sub;           // +8
        uint32_t slot;          // +0xC
        uint32_t flags;         // +0x10: stored in engine+0xAC (the savegame header)
        uint32_t thumbnail;     // +0x14
        uint32_t checkWindows;  // +0x18: the engine refuses while a trade / chest / ... window is open
        uint32_t unused;        // +0x1C
        const char* name;       // +0x20
    };
    static_assert(sizeof(SaveEvent) == 0x24);

    void* g_engine = nullptr;

    struct Job
    {
        bool active = false;
        uint32_t slot = 0;
        Save::FileInfo before;
        ULONGLONG started = 0;
    };
    Job g_job;
    std::deque<Save::Result> g_results;

    bool readU32(uintptr_t address, uint32_t& out)
    {
        __try
        {
            out = *reinterpret_cast<const uint32_t*>(address);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool readU8(uintptr_t address, uint8_t& out)
    {
        __try
        {
            out = *reinterpret_cast<const uint8_t*>(address);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The engine call; no C++ objects here (__try).
    bool sendEvent(void* engine, SaveEvent* event)
    {
        __try
        {
            // cEngine::receiveEvent is slot 0 of the engine's vtable (SacredBild patches the function's first bytes).
            using ReceiveEvent = uint32_t(__thiscall*)(void*, void*);
            const auto vtable = *reinterpret_cast<ReceiveEvent* const* const>(engine);
            vtable[0](engine, event);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool engineFlags(uint32_t& flags)
    {
        return g_engine && readU32(reinterpret_cast<uintptr_t>(g_engine) + kEngineFlags, flags);
    }

    bool singlePlayer()
    {
        uint8_t* players = nullptr;
        if (!A::g_pNetPlayers)
        {
            return false;
        }
        uint32_t p = 0;
        if (!readU32(A::g_pNetPlayers, p) || !p)
        {
            return false;
        }
        players = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(p));
        uint8_t multi = 1;
        return readU8(reinterpret_cast<uintptr_t>(players) + kNetPlayersMulti, multi) && multi == 0;
    }

    std::wstring pathOf(uint32_t slot)
    {
        wchar_t buffer[64];
        swprintf_s(buffer, L"Save\\GAME%02u.PAK", slot);
        return buffer;
    }

    void finish(bool ok, std::string message)
    {
        Save::Result r;
        r.slot = g_job.slot;
        r.ok = ok;
        r.message = std::move(message);
        r.info = Save::fileInfo(g_job.slot);
        g_job.active = false;
        g_results.push_back(std::move(r));
    }
}

bool Save::available()
{
    return g_engine && A::cSaveEvent_vtable;
}

bool Save::pending()
{
    uint32_t flags = 0;
    return g_job.active || (engineFlags(flags) && (flags & kBusyBits) != 0);
}

const char* Save::blocked()
{
    if (!available())
    {
        return "saving is not available (no game running or the engine was not found)";
    }
    if (!singlePlayer())
    {
        return "not a single player game";
    }
    if (pending())
    {
        return "a save or load is pending";
    }
    const uint32_t hero = World::hero();
    World::Object o;
    if (!hero || !World::object(hero, o) || o.health <= 0)
    {
        return "no living hero";
    }
    const Ui::State s = Ui::state();
    if (!s.available || std::string_view(s.mode) != "game")
    {
        return std::string_view(s.mode) == "cinematic" ? "a cinematic is playing" : "not in the game";
    }
    if (!s.exclusive.empty())
    {
        return "a window is open";
    }
    if (s.escapeMenu)
    {
        return "the escape menu is open";
    }
    if (s.messageBox)
    {
        return "a message box is open";
    }
    return nullptr;
}

Save::FileInfo Save::fileInfo(uint32_t slot)
{
    FileInfo info;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(pathOf(slot).c_str(), GetFileExInfoStandard, &data) ||
        (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
    {
        return info;
    }
    info.exists = true;
    info.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    info.stamp = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
    info.mtime = static_cast<int64_t>(info.stamp / 10000000ULL) - 11644473600LL;
    return info;
}

bool Save::request(uint32_t slot, const std::string& name, bool thumbnail, const char*& error)
{
    if (slot > kMaxSlot)
    {
        error = "the slot is 0..509";
        return false;
    }
    if (const char* why = blocked())
    {
        error = why;
        return false;
    }
    // The engine copies the name while it handles the event, so a buffer of ours is enough.
    char text[kNameMax + 1] = {};
    for (size_t i = 0; i < name.size() && i < kNameMax; ++i)
    {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        text[i] = c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?';
    }
    SaveEvent event{};
    event.vtable = A::cSaveEvent_vtable.get();
    event.code = 0xF;
    event.sub = 2;
    event.slot = slot;
    event.thumbnail = thumbnail ? 1 : 0;
    event.checkWindows = 1;
    event.name = text;

    const FileInfo before = fileInfo(slot);
    if (!sendEvent(g_engine, &event))
    {
        error = "the engine faulted on the save request";
        return false;
    }
    uint32_t flags = 0;
    if (!engineFlags(flags) || !(flags & kSaveBits))
    {
        error = "the engine refused the save (a trade, chest or other window is open)";
        return false;
    }
    g_job = Job{true, slot, before, GetTickCount64()};
    return true;
}

void Save::frame(void* engine)
{
    g_engine = engine;
    if (!g_job.active)
    {
        return;
    }
    uint32_t flags = 0;
    if (!engineFlags(flags))
    {
        finish(false, "the engine state is unreadable");
        return;
    }
    if (flags & kSaveBits)
    {
        if (GetTickCount64() - g_job.started > 60000)
        {
            finish(false, "the engine did not write the savegame within 60 s");
        }
        return;
    }
    const FileInfo after = fileInfo(g_job.slot);
    if (!after.exists)
    {
        finish(false, "the savegame file does not exist after the write");
    }
    else if (g_job.before.exists && after.stamp == g_job.before.stamp && after.size == g_job.before.size)
    {
        finish(false, "the savegame file did not change");
    }
    else
    {
        finish(true, {});
    }
}

bool Save::takeResult(Result& out)
{
    if (g_results.empty())
    {
        return false;
    }
    out = std::move(g_results.front());
    g_results.pop_front();
    return true;
}
