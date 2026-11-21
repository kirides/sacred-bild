#include "savegames.h"
#include "automation.h"
#include "overlay.h"
#include "replace.h"
#include "save.h"
#include "ui.h"
#include "game/gear_addr.h"
#include "log.h"
#include "patch.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <deque>

// See savegames.h.

namespace
{
    constexpr uint32_t kChunkName = 0x83, kChunkPicture = 0x93, kChunkWorldDate = 0x95, kChunkHero = 0xC3;
    constexpr size_t kHeader = 0x100, kTableEntries = 0x100, kHeaderTime = 0x5C;
    constexpr int kPictureW = 128, kPictureH = 96;
    constexpr UINT kLoadMessage = 0x8002;       // WM_APP + 2: wParam 1 load, lParam the savegame number (007561E0)
    constexpr uintptr_t kPlayersMulti = 4, kPlayersLocalHero = 0x14;

    std::wstring widen(const std::string& s)
    {
        if (s.empty())
        {
            return {};
        }
        const int n = MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring out(n, L'\0');
        MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
        return out;
    }

    std::string utf8(const wchar_t* text, size_t length)
    {
        if (!length)
        {
            return {};
        }
        const int n = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
        std::string out(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), out.data(), n, nullptr, nullptr);
        return out;
    }

    std::wstring pathOf(uint32_t slot)
    {
        wchar_t name[32];
        swprintf_s(name, L"\\GAME%02u.PAK", slot);
        return widen(Savegames::folder()) + name;
    }

    // GAME<digits>.PAK, any case: the slot.
    bool slotOf(const wchar_t* file, uint32_t& slot)
    {
        if (_wcsnicmp(file, L"GAME", 4) != 0)
        {
            return false;
        }
        const wchar_t* p = file + 4;
        uint32_t n = 0;
        int digits = 0;
        while (*p >= L'0' && *p <= L'9' && digits < 6)
        {
            n = n * 10 + (*p++ - L'0');
            ++digits;
        }
        if (digits < 2 || _wcsicmp(p, L".PAK") != 0 || n > Save::kMaxSlot)
        {
            return false;
        }
        slot = n;
        return true;
    }

    int64_t unixOf(const FILETIME& ft)
    {
        const uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        return static_cast<int64_t>(t / 10000000ULL) - 11644473600LL;
    }

    class File
    {
    public:
        explicit File(const std::wstring& path)
            : m_h(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr))
        {
        }
        ~File()
        {
            if (m_h != INVALID_HANDLE_VALUE)
            {
                CloseHandle(m_h);
            }
        }
        File(const File&) = delete;
        File& operator=(const File&) = delete;

        bool ok() const { return m_h != INVALID_HANDLE_VALUE; }

        bool read(uint32_t offset, void* out, uint32_t size)
        {
            LARGE_INTEGER at{};
            at.QuadPart = offset;
            DWORD got = 0;
            return SetFilePointerEx(m_h, at, nullptr, FILE_BEGIN) && ReadFile(m_h, out, size, &got, nullptr) && got == size;
        }

    private:
        HANDLE m_h;
    };

    struct Chunk
    {
        uint32_t type, offset, size;
    };

    // The header and the chunk table; false if it is no savegame.
    bool readTable(File& f, uint8_t (&header)[kHeader], std::vector<Chunk>& chunks)
    {
        if (!f.read(0, header, kHeader) || std::memcmp(header, "AMS", 3) != 0)
        {
            return false;
        }
        uint32_t count = 0;
        std::memcpy(&count, header + 4, 4);
        count = std::min<uint32_t>(count, kTableEntries);
        chunks.resize(count);
        return !count || f.read(kHeader, chunks.data(), count * sizeof(Chunk));
    }

    const Chunk* chunk(const std::vector<Chunk>& chunks, uint32_t type)
    {
        for (const Chunk& c : chunks)
        {
            if (c.type == type && c.size)
            {
                return &c;
            }
        }
        return nullptr;
    }

    bool parse(const std::wstring& path, Savegames::Info& info)
    {
        File f(path);
        uint8_t header[kHeader] = {};
        std::vector<Chunk> chunks;
        if (!f.ok() || !readTable(f, header, chunks))
        {
            return false;
        }
        info.version = header[3];
        int32_t t[8] = {};
        std::memcpy(t, header + kHeaderTime, sizeof(t));
        if (t[0] >= 1990 && t[0] < 3000 && t[1] >= 1 && t[1] <= 12 && t[2] >= 1 && t[2] <= 31)
        {
            SYSTEMTIME local{static_cast<WORD>(t[0]), static_cast<WORD>(t[1]), 0, static_cast<WORD>(t[2]),
                             static_cast<WORD>(t[4]), static_cast<WORD>(t[5]), static_cast<WORD>(t[6]), 0};
            SYSTEMTIME utc{};
            FILETIME ft{};
            if (TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc) && SystemTimeToFileTime(&utc, &ft))
            {
                info.time = unixOf(ft);
            }
        }
        if (const Chunk* c = chunk(chunks, kChunkName))
        {
            char name[0x41] = {};
            if (f.read(c->offset, name, std::min<uint32_t>(c->size, 0x40)))
            {
                // The game's single-byte text (Windows-1252), as UTF-8.
                wchar_t wide[0x41] = {};
                const int n = MultiByteToWideChar(1252, 0, name, static_cast<int>(strnlen(name, 0x40)), wide, 0x40);
                for (int i = 0; i < n; ++i)
                {
                    if (wide[i] < 0x20)
                    {
                        wide[i] = L'?';
                    }
                }
                info.name = utf8(wide, n > 0 ? n : 0);
            }
        }
        if (const Chunk* c = chunk(chunks, kChunkHero); c && c->size >= 8)
        {
            uint8_t hero[8 + 64 * 2] = {};
            if (f.read(c->offset, hero, std::min<uint32_t>(c->size, sizeof(hero))))
            {
                int32_t level = 0, cls = 0;
                std::memcpy(&level, hero, 4);
                std::memcpy(&cls, hero + 4, 4);
                info.level = level > 0 && level < 1000 ? level : 0;
                info.classId = cls >= 1 && cls <= 9 ? cls : 0;
                wchar_t name[65] = {};
                std::memcpy(name, hero + 8, 64 * 2);
                info.hero = utf8(name, wcsnlen(name, 64));
            }
        }
        if (const Chunk* c = chunk(chunks, kChunkWorldDate))
        {
            wchar_t text[129] = {};
            if (f.read(c->offset, text, std::min<uint32_t>(c->size, 256)))
            {
                info.worldDate = utf8(text, wcsnlen(text, 128));
            }
        }
        const Chunk* pic = chunk(chunks, kChunkPicture);
        info.picture = pic && pic->size >= kPictureW * kPictureH * 2;
        return true;
    }

    // What list() read from the files so far, by file name (Lua thread).
    struct InfoCache
    {
        uint64_t stamp = 0;
        uint64_t size = 0;
        bool ok = false;
        Savegames::Info info;
    };
    std::map<std::wstring, InfoCache> g_infos;

    // The pictures made so far: slot -> {file stamp, picture} (Lua thread).
    struct Cached
    {
        uint64_t stamp = 0;
        uint32_t picture = 0;
        ULONGLONG checked = 0;      // when the file's stamp was last looked at (GetTickCount64)
    };
    std::map<uint32_t, Cached> g_pictures;

    uint64_t stampOf(const std::wstring& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
        {
            return 0;
        }
        return ((static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime) ^
               (static_cast<uint64_t>(data.nFileSizeLow) << 1);
    }

    bool readPicture(const std::wstring& path, std::vector<uint16_t>& pixels)
    {
        File f(path);
        uint8_t header[kHeader] = {};
        std::vector<Chunk> chunks;
        if (!f.ok() || !readTable(f, header, chunks))
        {
            return false;
        }
        const Chunk* c = chunk(chunks, kChunkPicture);
        if (!c || c->size < kPictureW * kPictureH * 2)
        {
            return false;
        }
        pixels.resize(kPictureW * kPictureH);
        if (!f.read(c->offset, pixels.data(), kPictureW * kPictureH * 2))
        {
            return false;
        }
        // The capture leaves the alpha bits as they were in the back buffer; the picture is opaque.
        for (uint16_t& p : pixels)
        {
            p |= 0xF000;
        }
        return true;
    }

    void command(uintptr_t window, uint32_t command)
    {
        __try
        {
            Gear::Addr::cUI_Savegame_command(reinterpret_cast<void*>(window), command);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    // The savegame windows that exist now (the in-game one, the menus' one): their lists read the folder again.
    void refreshWindows()
    {
        if (!Gear::Addr::cUI_Savegame_command)
        {
            return;
        }
        for (const char* name : {"UI_WND_SAVEGAME", "UI_MENU_LOADGAME"})
        {
            if (const uintptr_t w = Replace::object(name))
            {
                command(w, 10);
            }
        }
    }

    bool safeWrite32(uintptr_t address, uint32_t value)
    {
        __try
        {
            *reinterpret_cast<volatile uint32_t*>(address) = value;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool safeRead(uintptr_t address, void* out, size_t size)
    {
        __try
        {
            std::memcpy(out, reinterpret_cast<const void*>(address), size);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool hideWindow(uintptr_t w)
    {
        __try
        {
            Gear::Addr::cUI_Control_clearFlags(reinterpret_cast<void*>(w), 1);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}

std::string Savegames::folder()
{
    std::string p = Overlay::resolve("Save\\GAME00.PAK");
    const size_t slash = p.find_last_of("\\/");
    return slash == std::string::npos ? std::string("Save") : p.substr(0, slash);
}

std::vector<Savegames::Info> Savegames::list()
{
    std::vector<Info> out;
    const std::wstring dir = widen(folder());
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((dir + L"\\*.PAK").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE)
    {
        return out;
    }
    do
    {
        uint32_t slot = 0;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !slotOf(fd.cFileName, slot))
        {
            continue;
        }
        // A savegame whose file did not change since the last listing is not read again: with hundreds of them the
        // screen asks for the list on every refresh.
        const uint64_t stamp = (static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
        const uint64_t size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        const std::wstring name = fd.cFileName;
        auto it = g_infos.find(name);
        if (it != g_infos.end() && it->second.stamp == stamp && it->second.size == size)
        {
            if (it->second.ok)
            {
                out.push_back(it->second.info);
            }
            continue;
        }
        InfoCache c;
        c.stamp = stamp;
        c.size = size;
        c.info.slot = slot;
        c.info.file = utf8(fd.cFileName, wcslen(fd.cFileName));
        c.info.size = size;
        c.info.mtime = unixOf(fd.ftLastWriteTime);
        c.ok = parse(dir + L"\\" + fd.cFileName, c.info);
        if (c.ok)
        {
            out.push_back(c.info);
        }
        g_infos[name] = std::move(c);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    // As the game's list: by file name.
    std::sort(out.begin(), out.end(), [](const Info& a, const Info& b) { return _stricmp(a.file.c_str(), b.file.c_str()) < 0; });
    return out;
}

uint32_t Savegames::picture(uint32_t slot)
{
    // A screen asks for the picture of every row it shows, every frame: the file's stamp (a file system call) is looked at once a second, not every time.
    const ULONGLONG now = GetTickCount64();
    if (const auto known = g_pictures.find(slot); known != g_pictures.end() && known->second.stamp && now - known->second.checked < 1000)
    {
        return known->second.picture;
    }
    const std::wstring path = pathOf(slot);
    const uint64_t stamp = stampOf(path);
    auto it = g_pictures.find(slot);
    if (it != g_pictures.end())
    {
        if (it->second.stamp == stamp && stamp)
        {
            it->second.checked = now;
            return it->second.picture;
        }
        Ui::pictureRelease(it->second.picture);
        g_pictures.erase(it);
    }
    if (!stamp)
    {
        return 0;
    }
    std::vector<uint16_t> pixels;
    Cached c;
    c.stamp = stamp;
    c.checked = now;
    if (readPicture(path, pixels))
    {
        c.picture = Ui::pictureCreate(kPictureW, kPictureH, std::move(pixels));
    }
    g_pictures[slot] = c;   // also "no picture", until the file changes
    return c.picture;
}

bool Savegames::remove(uint32_t slot, const char*& error)
{
    if (slot > Save::kMaxSlot)
    {
        error = "the slot is 0..32766";
        return false;
    }
    if (Save::pending())
    {
        error = "a save or load is pending";
        return false;
    }
    const std::wstring path = pathOf(slot);
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!DeleteFileW(path.c_str()))
    {
        error = GetLastError() == ERROR_FILE_NOT_FOUND ? "no such savegame" : "the file could not be deleted";
        return false;
    }
    LOG("Savegames: deleted GAME{:02}.PAK", slot);
    refreshWindows();
    return true;
}

bool Savegames::load(uint32_t slot, const char*& error)
{
    if (slot > Save::kMaxSlot)
    {
        error = "the slot is 0..32766";
        return false;
    }
    Info info;
    if (!parse(pathOf(slot), info))
    {
        error = "no such savegame";
        return false;
    }
    if (info.version != kFormat)
    {
        error = "the savegame is of another version of the game";
        return false;
    }
    if (Save::pending())
    {
        error = "a save or load is pending";
        return false;
    }
    HWND window = Automation::window();
    uintptr_t players = 0;
    if (!window || !Gear::Addr::g_pNetPlayers || !safeRead(reinterpret_cast<uintptr_t>(Gear::Addr::g_pNetPlayers.get()), &players, sizeof(players)) ||
        !players)
    {
        error = "the game is not ready";
        return false;
    }
    const uintptr_t inGame = Replace::object("UI_WND_SAVEGAME"), inMenus = Replace::object("UI_MENU_LOADGAME");
    if (inGame && !inMenus)
    {
        uint8_t multi = 1;
        if (!safeRead(players + kPlayersMulti, &multi, 1) || multi != 0)
        {
            error = "only in a single player game";
            return false;
        }
    }
    if (!inGame && !inMenus)
    {
        error = "neither in the game nor in the menus";
        return false;
    }
    // As the savegame window's Load in single player (0071CC90 command 6): the window hides, the local hero is id 1,
    // then the game window gets the load message.
    for (uintptr_t w : {inGame, inMenus})
    {
        if (w && (Gear::Addr::cUI_Control_clearFlags) && Replace::open(w == inGame ? "UI_WND_SAVEGAME" : "UI_MENU_LOADGAME"))
        {
            hideWindow(w);
        }
    }
    if (!safeWrite32(players + kPlayersLocalHero, 1))
    {
        error = "cannot write the players object";
        return false;
    }
    if (!PostMessageW(window, kLoadMessage, 1, slot))
    {
        error = "cannot post to the game window";
        return false;
    }
    noteLoad(slot);
    LOG("Savegames: loading GAME{:02}.PAK", slot);
    return true;
}

// --- the game's load / new game command ---------------------------------------------------------------------------------

namespace
{
    std::mutex g_commandLock;
    std::deque<Savegames::Command> g_commands;
    void(__fastcall* g_armaPlay)(void* command, void* edx) = nullptr;

    std::string sanitize(const std::string& s)
    {
        std::string out;
        for (char ch : s)
        {
            const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
            out += ok ? ch : '_';
        }
        return out;
    }

    // The part of the savegame folder that tells it from the game's own Save folder: empty for that one.
    std::string folderTag()
    {
        std::string f = Savegames::folder();
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir = exe;
        dir.resize(dir.find_last_of(L"\\/") == std::wstring::npos ? 0 : dir.find_last_of(L"\\/"));
        std::wstring w = widen(f);
        for (auto& ch : w) { if (ch == L'/') ch = L'\\'; }
        auto lower = [](std::wstring s) { for (auto& ch : s) ch = static_cast<wchar_t>(towlower(ch)); return s; };
        if (w == L"Save" || lower(w) == lower(dir + L"\\Save"))
        {
            return {};
        }
        // Relative to the game folder if it is inside it, else the last two components.
        std::string rel = f;
        if (lower(w).compare(0, dir.size() + 1, lower(dir + L"\\")) == 0)
        {
            rel = utf8(w.c_str() + dir.size() + 1, w.size() - dir.size() - 1);
        }
        else
        {
            const size_t last = w.find_last_of(L'\\');
            const size_t prev = last == std::wstring::npos || last == 0 ? std::wstring::npos : w.find_last_of(L'\\', last - 1);
            rel = utf8(w.c_str() + (prev == std::wstring::npos ? 0 : prev + 1), w.size() - (prev == std::wstring::npos ? 0 : prev + 1));
        }
        std::string tag = sanitize(rel);
        return tag.size() > 48 ? tag.substr(tag.size() - 48) : tag;
    }

    bool readCommand(const void* command, uint16_t& kind, uint32_t& number)
    {
        __try
        {
            kind = *reinterpret_cast<const uint16_t*>(static_cast<const char*>(command) + 4);
            number = *reinterpret_cast<const uint32_t*>(static_cast<const char*>(command) + 8);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void __fastcall hookArmaPlay(void* command, void* edx)
    {
        Savegames::Command c;
        uint16_t kind = 0;
        if (!readCommand(command, kind, c.number))
        {
            g_armaPlay(command, edx);
            return;
        }
        c.kind = kind == 1 ? Savegames::Command::Kind::Load
               : kind == 4 ? Savegames::Command::Kind::LoadHero : Savegames::Command::Kind::New;
        if (c.kind == Savegames::Command::Kind::Load)
        {
            c.scope = Savegames::scopeOf(c.number);
        }
        else if (c.kind == Savegames::Command::Kind::LoadHero)
        {
            c.scope = Savegames::heroScopeOf(c.number);
        }
        {
            std::lock_guard lock(g_commandLock);
            if (g_commands.size() < 32)
            {
                g_commands.push_back(std::move(c));
            }
        }
        g_armaPlay(command, edx);
    }
}

std::string Savegames::scopeOf(uint32_t slot)
{
    char name[24];
    sprintf_s(name, "GAME%02u", slot);
    const std::string tag = folderTag();
    return tag.empty() ? std::string(name) : tag + "-" + name;
}

std::string Savegames::heroScopeOf(uint32_t number)
{
    char name[24];
    sprintf_s(name, "HERO%02u", number);
    return name;
}

void Savegames::noteLoad(uint32_t slot)
{
    Command c;
    c.kind = Command::Kind::Load;
    c.number = slot;
    c.scope = scopeOf(slot);
    std::lock_guard lock(g_commandLock);
    if (g_commands.size() < 32)
    {
        g_commands.push_back(std::move(c));
    }
}

namespace
{
    std::mutex g_reservedLock;
    std::vector<uint32_t> g_reserved;       // sorted
}

void Savegames::setReserved(std::vector<uint32_t> slots)
{
    std::sort(slots.begin(), slots.end());
    slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
    std::lock_guard lock(g_reservedLock);
    g_reserved = std::move(slots);
}

uint32_t Savegames::firstFreeSlot()
{
    std::vector<uint32_t> reserved;
    {
        std::lock_guard lock(g_reservedLock);
        reserved = g_reserved;
    }
    for (uint32_t slot = 1; slot <= Save::kMaxSlot; ++slot)
    {
        if (std::binary_search(reserved.begin(), reserved.end(), slot))
        {
            continue;
        }
        if (GetFileAttributesW(pathOf(slot).c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            return slot;
        }
    }
    return 0;
}

namespace
{
    // The game's own picker (0x619B80) looks at 1..509 and answers 0x1FF when they are all taken; this one goes on to
    // the end of what the file names can hold. The engine (render thread) calls it when a save asks for slot 0xFFFF.
    decltype(Gear::Addr::cSave_firstFreeSlot)::Ptr g_origFirstFree = nullptr;

    uint16_t __cdecl hookFirstFreeSlot()
    {
        const uint32_t slot = Savegames::firstFreeSlot();
        return static_cast<uint16_t>(slot ? slot : Save::kMaxSlot);
    }
}

bool Savegames::install()
{
    if (Gear::Addr::cSave_firstFreeSlot)
    {
        Patch::hook(g_origFirstFree, Gear::Addr::cSave_firstFreeSlot, hookFirstFreeSlot, "cSave_firstFreeSlot");
    }
    else
    {
        LOG("Savegames: the first-free-number function was not found: the game's own saves stop at number 509");
    }
    if (!Gear::Addr::armaPlayExecute)
    {
        LOG("Savegames: the load command was not found: no savegame.load / savegame.new");
        return false;
    }
    return Patch::hook(g_armaPlay, Gear::Addr::armaPlayExecute, hookArmaPlay, "cCommand_armaPlay_execute");
}

bool Savegames::take(Command& out)
{
    std::lock_guard lock(g_commandLock);
    if (g_commands.empty())
    {
        return false;
    }
    out = std::move(g_commands.front());
    g_commands.pop_front();
    return true;
}
