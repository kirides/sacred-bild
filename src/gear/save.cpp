#include "save.h"
#include "log.h"
#include "ui/replace.h"
#include "ui/ui.h"
#include "world/world.h"
#include "engine/gear_addr.h"

#include <windows.h>

#include <cstdio>
#include <atomic>
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
    ULONGLONG g_lastFrame = 0;      // when Save::frame last ran (game frames only)

    bool framesRunning()
    {
        return g_lastFrame != 0 && GetTickCount64() - g_lastFrame < 2000;
    }
    // A savegame window's save waiting for its picture: the world is drawn first, then (before the interface goes on
    // top of it, Save::renderCapture) the engine takes the picture, and the next frame sends the request.
    struct Waiting
    {
        std::atomic<bool> active{false};
        std::atomic<bool> captured{false};
        uint32_t slot = 0;
        std::string name;
        uintptr_t window = 0;
        ULONGLONG since = 0;
    };
    Waiting g_waiting;
    constexpr ULONGLONG kCaptureWaitMs = 1000;     // then the save goes on with the picture the engine has
    // A save the engine runs that no mod asked for: its slot (engine+0x68), seen when its bits (0xC0) went up.
    bool g_foreign = false;
    uint32_t g_foreignSlot = 0;
    Save::FileInfo g_foreignBefore;
    constexpr uintptr_t kEngineSlot = 0x68;     // uint16
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

    bool hideWindow(uintptr_t window)
    {
        __try
        {
            A::cUI_Control_clearFlags(reinterpret_cast<void*>(window), 1);
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

    // The engine's own capture of the back buffer into its savegame picture; no C++ objects here (__try).
    bool captureNow(void* engine)
    {
        __try
        {
            A::cEngine_captureInternal(engine, 1);
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
    // The engine object is only known from the frames of a running game. In the menus (after leaving a game) the last pointer is stale
    // and its flags are whatever the freed memory holds: that read "a save or load is pending" for a while and refused every load.
    if (!framesRunning())
    {
        return false;
    }
    return g_job.active || (engineFlags(flags) && (flags & kBusyBits) != 0);
}

// The checks of blocked() but the windows': for a save from the savegame window (it is open then).
static const char* blockedBesidesWindows();

const char* Save::blocked()
{
    if (const char* why = blockedBesidesWindows())
    {
        return why;
    }
    const Ui::State s = Ui::state();
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

static const char* blockedBesidesWindows()
{
    if (!Save::available())
    {
        return "saving is not available (no game running or the engine was not found)";
    }
    if (!singlePlayer())
    {
        return "not a single player game";
    }
    if (Save::pending())
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

namespace
{
    // Builds the engine's save event and sends it; true when the engine took it (its flags show a save).
    bool submit(uint32_t slot, const std::string& name, bool thumbnail, bool fromWindow, uintptr_t window,
                const char*& error)
    {
        // The engine copies the name while it handles the event, so a buffer of ours is enough.
        // UTF-8 in, the game's single-byte text out (Windows-1252, as its own savegame window writes: umlauts and the
        // like stay, anything else becomes '?').
        char text[kNameMax + 1] = {};
        {
            std::wstring wide(name.size(), wchar_t{});
            const int n = MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), wide.data(),
                                              static_cast<int>(wide.size()));
            wide.resize(n > 0 ? n : 0);
            for (wchar_t& c : wide)
            {
                if (c < 0x20)
                {
                    c = L'?';
                }
            }
            BOOL lossy = FALSE;
            const int bytes = WideCharToMultiByte(1252, 0, wide.data(), static_cast<int>(wide.size()), text, kNameMax,
                                                  "?", &lossy);
            text[bytes > 0 ? bytes : 0] = 0;
        }
        SaveEvent event{};
        event.vtable = A::cSaveEvent_vtable.get();
        event.code = 0xF;
        event.sub = 2;
        event.slot = slot;
        event.thumbnail = thumbnail ? 1 : 0;
        event.checkWindows = fromWindow ? 0 : 1;   // the window's own request (0071CC90 command 2) does not check
        event.name = text;

        const Save::FileInfo before = Save::fileInfo(slot);
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
        if (window && A::cUI_Control_clearFlags)
        {
            hideWindow(window);     // as the window does after its request
        }
        return true;
    }
}

bool Save::request(uint32_t slot, const std::string& name, bool thumbnail, const char*& error, bool fromWindow)
{
    if (slot > kMaxSlot)
    {
        error = "the slot is 0..509";
        return false;
    }
    const uintptr_t window = fromWindow ? Replace::object("UI_WND_SAVEGAME") : 0;
    if (fromWindow)
    {
        if (const char* why = blockedBesidesWindows())
        {
            error = why;
            return false;
        }
        if (!window || !Replace::open("UI_WND_SAVEGAME"))
        {
            error = "the savegame window is not open";
            return false;
        }
        if (Ui::state().messageBox)
        {
            error = "a message box is open";
            return false;
        }
        thumbnail = false;      // the picture comes from renderCapture instead, below
        if (g_waiting.active)
        {
            error = "a save is already waiting for its picture";
            return false;
        }
        if (A::cEngine_captureInternal && g_engine)
        {
            // The engine takes the picture of the back buffer when something asks for it (the quicksave key, its
            // Auto-Save, a request's thumbnail flag); the window leaves it to the moment it opened, which is the
            // last picture taken, whatever that showed (the start menu's screen, when no world frame was taken since).
            // The back buffer holds the world only until the interface is drawn over it: take the picture there, in
            // the next frame's render, and send the request after it.
            g_waiting.active = true;
            g_waiting.captured = false;
            g_waiting.slot = slot;
            g_waiting.name = name;
            g_waiting.window = window;
            g_waiting.since = GetTickCount64();
            g_job = Job{true, slot, fileInfo(slot), g_waiting.since};
            return true;
        }
    }
    else if (const char* why = blocked())
    {
        error = why;
        return false;
    }
    return submit(slot, name, thumbnail, fromWindow, window, error);
}

void Save::renderCapture()
{
    // The render thread, once per frame between the world view and the interface.
    if (!g_waiting.active || g_waiting.captured || !g_engine || !A::cEngine_captureInternal)
    {
        return;
    }
    LOG("Save: the picture of the savegame is taken between the world and the interface");
    captureNow(g_engine);
    g_waiting.captured = true;
}

void Save::frame(void* engine)
{
    g_engine = engine;
    uint32_t flags = 0;
    if (g_waiting.active)
    {
        // The picture was taken in a render since the request (or it never came: the save goes on without).
        if (!g_waiting.captured && GetTickCount64() - g_waiting.since < kCaptureWaitMs)
        {
            return;
        }
        g_waiting.active = false;
        const char* error = "";
        if (!submit(g_waiting.slot, g_waiting.name, false, true, g_waiting.window, error))
        {
            finish(false, error);
        }
        return;
    }
    if (!g_job.active)
    {
        // Somebody else's save: the quicksave key, the savegame window, the game's own Auto-Save.
        const bool running = engineFlags(flags) && (flags & kSaveBits) != 0;
        if (running && !g_foreign)
        {
            uint32_t word = 0;
            g_foreign = readU32(reinterpret_cast<uintptr_t>(engine) + kEngineSlot, word);
            g_foreignSlot = word & 0xFFFF;
            g_foreignBefore = Save::fileInfo(g_foreignSlot);
        }
        else if (!running && g_foreign)
        {
            g_foreign = false;
            const FileInfo after = fileInfo(g_foreignSlot);
            Save::Result r;
            r.slot = g_foreignSlot;
            r.external = true;
            r.info = after;
            r.ok = after.exists && (!g_foreignBefore.exists || after.stamp != g_foreignBefore.stamp ||
                                    after.size != g_foreignBefore.size);
            if (r.ok)
            {
                g_results.push_back(std::move(r));
            }
        }
        return;
    }
    g_foreign = false;
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
