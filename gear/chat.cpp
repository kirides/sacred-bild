#include "chat.h"
#include "log.h"
#include "netmsg.h"
#include "patch.h"
#include "game/gear_addr.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>

namespace
{
    constexpr uint32_t kMainList = 0x14C;       // the UI manager's message list (0075A770: add ecx, 0x14C)
    constexpr uint32_t kNames = 0x384C;         // the players' object (g_pNetPlayers): the names the chat shows, a wstring {begin, end, cap} per slot
    constexpr size_t kMaxUnits = 2048;
    constexpr size_t kQueueMax = 512;
    constexpr DWORD kHideMs = 500;
    constexpr DWORD kPairMs = 250;              // the chat case adds its line at once; a late line is not paired
    constexpr DWORD kEchoMs = 200;              // a window's list getting the same line as the main list

    struct WStr
    {
        const wchar_t* begin;
        const wchar_t* end;
        const wchar_t* cap;
    };

    decltype(Gear::Addr::cUI_TextLines_add)::Ptr g_orig = nullptr;
    bool g_installed = false;
    std::atomic<DWORD> g_hideUntil{0};
    std::atomic<bool> g_hideOn{false};
    std::atomic<uint32_t> g_seen{0};

    std::mutex g_lock;
    std::deque<Chat::Line> g_lines;

    struct Pending
    {
        DWORD thread = 0;
        DWORD at = 0;
        uint16_t type = 0;
        uint32_t slot = 0;
        std::wstring name;
        std::wstring message;
    };
    Pending g_pending;              // under g_lock
    std::wstring g_lastMain;        // under g_lock: the last line of the main list, for the window lists' echo
    DWORD g_lastMainAt = 0;

    bool copyWide(const wchar_t* begin, const wchar_t* end, std::wstring& out)
    {
        out.clear();
        __try
        {
            if (!begin || end < begin)
            {
                return false;
            }
            size_t n = static_cast<size_t>(end - begin);
            if (n > kMaxUnits)
            {
                n = kMaxUnits;
            }
            while (n && begin[n - 1] == 0)
            {
                --n;
            }
            out.assign(begin, n);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            out.clear();
            return false;
        }
        return true;
    }

    bool readText(const void* text, std::wstring& out)
    {
        WStr s{};
        __try
        {
            std::memcpy(&s, text, sizeof(s));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return copyWide(s.begin, s.end, out);
    }

    uintptr_t mainList()
    {
        __try
        {
            const uint8_t* manager = Gear::Addr::g_pUiManager ? *Gear::Addr::g_pUiManager.get() : nullptr;
            return manager ? reinterpret_cast<uintptr_t>(manager) + kMainList : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    bool nameEntry(uint32_t slot, WStr& s)
    {
        __try
        {
            const uint8_t* net = *Gear::Addr::g_pNetPlayers.get();
            if (!net)
            {
                return false;
            }
            std::memcpy(&s, net + kNames + slot * 12, sizeof(s));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return true;
    }

    std::wstring senderName(uint32_t slot)
    {
        std::wstring out;
        WStr s{};
        if (slot < 1 || slot > 16)
        {
            return out;
        }
        if (Gear::Addr::g_pNetPlayers && nameEntry(slot, s) && s.begin <= s.end && s.end - s.begin <= 64)
        {
            copyWide(s.begin, s.end, out);
        }
        if (out.empty())
        {
            out = NetMsg::heroName(slot);
        }
        return out;
    }

    bool endsWith(const std::wstring& text, const std::wstring& tail)
    {
        return !tail.empty() && text.size() >= tail.size() && text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
    }

    uint32_t defaultColor(uint32_t channel)
    {
        switch (channel)
        {
        case 2: return 0xFF00A0FF;
        case 3: return 0xFFFF6347;
        case 4: return 0xFF00FF50;
        default: return 0xFFFFFFFF;
        }
    }

    // Under g_lock. Fills kind, sender and message; true if the line belongs in the queue.
    bool classify(Chat::Line& line, bool mainList)
    {
        const DWORD now = GetTickCount();
        line.main = mainList;
        if (!mainList)
        {
            // a window's own list: the private message window and the own lines get the main list's line too
            if (line.text == g_lastMain && now - g_lastMainAt <= kEchoMs)
            {
                return false;
            }
        }
        if (g_pending.at && now - g_pending.at > kPairMs)
        {
            g_pending = Pending{};
        }
        if (g_pending.at && g_pending.thread == GetCurrentThreadId() &&
            (endsWith(line.text, g_pending.message) || (!g_pending.name.empty() && line.text.find(g_pending.name) != std::wstring::npos)))
        {
            line.slot = g_pending.slot;
            line.sender = g_pending.name;
            line.message = g_pending.message;
            line.kind = g_pending.slot == 0 ? Chat::Kind::Server
                      : g_pending.type == 3 ? Chat::Kind::Private
                      : g_pending.type == 4 ? Chat::Kind::Team
                                            : Chat::Kind::Global;
            g_pending = Pending{};
        }
        else if (line.channel == 0x10)
        {
            line.kind = Chat::Kind::Notice;
        }
        else if (line.channel == 4)
        {
            line.kind = Chat::Kind::Own;
        }
        else if (line.channel == 3)
        {
            line.kind = Chat::Kind::Server;
        }
        else if (line.channel == 2)
        {
            line.kind = Chat::Kind::Private;
        }
        else
        {
            line.kind = Chat::Kind::System;
        }
        if (mainList)
        {
            g_lastMain = line.text;
            g_lastMainAt = now;
        }
        return true;
    }

    void __fastcall hookAdd(void* lines, void* edx, uint32_t channel, uint32_t font, const void* text, uint32_t color)
    {
        const bool isMain = reinterpret_cast<uintptr_t>(lines) == mainList();
        const uint16_t ch = static_cast<uint16_t>(channel);
        bool hide = false;
        Chat::Line line;
        if (text && readText(text, line.text) && !line.text.empty())
        {
            line.channel = ch;
            line.color = color ? color : defaultColor(ch);
            hide = isMain && ch >= 1 && ch <= 4 && g_hideOn.load(std::memory_order_relaxed) &&
                   static_cast<int32_t>(g_hideUntil.load(std::memory_order_relaxed) - GetTickCount()) > 0;
            line.hidden = hide;
            if (g_seen.fetch_add(1) < 3)
            {
                LOG("Chat: line on channel {} ({} list){}", ch, isMain ? "main" : "a window's", hide ? ", hidden" : "");
            }
            std::lock_guard lock(g_lock);
            if (classify(line, isMain) && g_lines.size() < kQueueMax)
            {
                g_lines.push_back(std::move(line));
            }
        }
        if (!hide)
        {
            g_orig(lines, edx, channel, font, text, color);
        }
    }
}

bool Chat::install()
{
    if (!Gear::Addr::cUI_TextLines_add || !Gear::Addr::g_pUiManager)
    {
        LOG("Chat: the message list was not found: no gear.chat lines");
        return false;
    }
    g_installed = Patch::hook(g_orig, Gear::Addr::cUI_TextLines_add, hookAdd, "cUI_TextLines_add");
    return g_installed;
}

bool Chat::available()
{
    return g_installed;
}

void Chat::noteNet(uint16_t type, uint32_t slot, const uint16_t* text, size_t units)
{
    Pending p;
    p.thread = GetCurrentThreadId();
    p.at = GetTickCount();
    if (p.at == 0)
    {
        p.at = 1;
    }
    p.type = type;
    p.slot = slot;
    p.name = senderName(slot);
    p.message.assign(reinterpret_cast<const wchar_t*>(text), units);
    std::lock_guard lock(g_lock);
    g_pending = std::move(p);
}

bool Chat::take(Line& out)
{
    std::lock_guard lock(g_lock);
    if (g_lines.empty())
    {
        return false;
    }
    out = std::move(g_lines.front());
    g_lines.pop_front();
    return true;
}

void Chat::setHidden(bool hidden)
{
    g_hideOn = hidden;
    if (hidden)
    {
        g_hideUntil = GetTickCount() + kHideMs;
    }
}

bool Chat::hidden()
{
    return g_hideOn && static_cast<int32_t>(g_hideUntil.load() - GetTickCount()) > 0;
}

namespace
{
    bool callAdd(uintptr_t list, uint16_t channel, const WStr* text, uint32_t color)
    {
        __try
        {
            Gear::Addr::cUI_TextLines_add(reinterpret_cast<void*>(list), channel, 1, text, color);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return true;
    }
}

bool Chat::addForTest(const std::wstring& text, uint16_t channel, uint32_t color)
{
    const uintptr_t list = mainList();
    if (!g_installed || !list || text.empty())
    {
        return false;
    }
    std::wstring copy = text;
    const WStr s{copy.data(), copy.data() + copy.size(), copy.data() + copy.size()};
    return callAdd(list, channel, &s, color);
}
