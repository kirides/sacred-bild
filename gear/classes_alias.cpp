#include "memfault.h"
#include "classes.h"
#include "log.h"
#include "lua_host.h"
#include "game/gear_addr.h"
#include "patch.h"
#include "world.h"

#include <windows.h>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>

// Alias abilities, the rune class checks, hero records and texts for custom classes (classes.h).
namespace
{
    using namespace Gear;

    constexpr size_t kPool1Stride = 0x78, kPool2Stride = 0x94, kPool1Max = 160, kPool2Max = 64;
    alignas(16) uint8_t g_pool1[kPool1Max * kPool1Stride];
    alignas(16) uint8_t g_pool2[kPool2Max * kPool2Stride];

    struct AliasInfo
    {
        uint32_t base = 0;
        int which = 0;
        size_t index = 0;
    };
    std::map<uint32_t, AliasInfo> g_aliases;
    size_t g_used1 = 0, g_used2 = 0;
    std::mutex g_aliasLock;
    std::atomic<bool> g_usableActive{false}, g_haveAliases{false};

    decltype(Addr::abilityLookup1)::Ptr g_origLookup1 = nullptr;
    decltype(Addr::abilityLookup2)::Ptr g_origLookup2 = nullptr;
    decltype(Addr::abilityUsable1)::Ptr g_origUsable1 = nullptr;
    decltype(Addr::abilityUsable2)::Ptr g_origUsable2 = nullptr;
    decltype(Addr::abilityLinkedSpell)::Ptr g_origLinked = nullptr;
    // art alias -> spell alias: the spell record the copy takes its numbers from (see Classes::aliasLink)
    std::map<uint32_t, uint32_t> g_links;
    decltype(Addr::textResources_get)::Ptr g_origTextGet = nullptr;

    // Gear's own texts: three pointers (begin, end, capacity) like the engine's string entries, at stable addresses.
    struct VirtualText
    {
        std::wstring text;
        const wchar_t* entry[3] = {};
    };
    std::map<uint32_t, VirtualText> g_texts;
    uint32_t g_nextText = Classes::kTextFirst;
    std::mutex g_textLock;
    std::atomic<bool> g_haveTexts{false};

    const wchar_t* const* __fastcall hookTextGet(void* self, void* edx, uint32_t id)
    {
        if (id >= Classes::kTextFirst && id <= Classes::kTextFirst + 0xFFFF && g_haveTexts.load(std::memory_order_acquire))
        {
            std::lock_guard lock(g_textLock);
            const auto it = g_texts.find(id);
            if (it != g_texts.end())
            {
                return it->second.entry;
            }
        }
        return g_origTextGet(self, edx, id);
    }

    void storeText(VirtualText& v, const std::wstring& text)
    {
        v.text = text;
        v.entry[0] = v.text.c_str();
        v.entry[1] = v.text.c_str() + v.text.size();
        v.entry[2] = v.text.c_str() + v.text.size() + 1;
    }

    constexpr uintptr_t kTypeSize = 0x80;

    template <class T>
    bool readAt(uintptr_t base, uintptr_t offset, T& out)
    {
        __try
        {
            std::memcpy(&out, reinterpret_cast<const void*>(base + offset), sizeof(T));
            return true;
        }
        __except (Gear::isMemoryFault(GetExceptionCode()) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            return false;
        }
    }

    uint8_t* recordAt(int which, size_t index)
    {
        return which == 1 ? g_pool1 + index * kPool1Stride : g_pool2 + index * kPool2Stride;
    }

    uint8_t* aliasRecord(int16_t id, int which)
    {
        if (!g_haveAliases.load(std::memory_order_acquire) || id < 0)
        {
            return nullptr;
        }
        std::lock_guard lock(g_aliasLock);
        const auto it = g_aliases.find(static_cast<uint32_t>(static_cast<uint16_t>(id)));
        if (it == g_aliases.end() || it->second.which != which)
        {
            return nullptr;
        }
        return recordAt(which, it->second.index);
    }

    uint8_t* __cdecl hookLookup1(int16_t id)
    {
        if (uint8_t* r = aliasRecord(id, 1))
        {
            return r;
        }
        return g_origLookup1(id);
    }

    uint16_t artInFlight(const void* hero)
    {
        __try
        {
            return *reinterpret_cast<const uint16_t*>(reinterpret_cast<uintptr_t>(hero) + 0x120);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    // The spell lookup by id (ENG 004D2310). The handlers of the spell-linked arts (the Dark Elf's Poison Mist ...) do not ask the
    // art's record which spell it takes: they push the spell's id (12 for Poison Mist) as a constant and call this function from the
    // handler code (ESI = the hero). So when such a call comes from the hero handlers' code range and the hero has an art alias in
    // flight (+0x120) that is linked to a spell alias, and the id is the spell its base art takes, the answer is the spell alias.
    uint8_t* linkedAnswer(const void* hero, int16_t id, uintptr_t ret)
    {
        if (ret < 0x4FF0B0 || ret >= 0x520000 || !hero)
        {
            return nullptr;
        }
        const uint16_t art = artInFlight(hero);
        std::lock_guard lock(g_aliasLock);
        const auto link = g_links.find(art);
        if (link == g_links.end())
        {
            return nullptr;
        }
        const auto a = g_aliases.find(art);
        const auto s = g_aliases.find(link->second);
        if (a == g_aliases.end() || s == g_aliases.end() || g_origLookup1 == nullptr || g_origLinked == nullptr)
        {
            return nullptr;
        }
        uint8_t* baseRecord = g_origLookup1(static_cast<int16_t>(a->second.base));
        if (!baseRecord || (g_origLinked(baseRecord, nullptr) & 0xFFFF) != static_cast<uint16_t>(id))
        {
            return nullptr;
        }
        return recordAt(s->second.which, s->second.index);
    }

    uint8_t* __cdecl lookup2Impl(const void* hero, int16_t id, uintptr_t ret)
    {
        if (uint8_t* r = aliasRecord(id, 2))
        {
            return r;
        }
        if (g_haveAliases.load(std::memory_order_acquire) && !g_links.empty())
        {
            if (uint8_t* r = linkedAnswer(hero, id, ret))
            {
                return r;
            }
        }
        return g_origLookup2(id);
    }

    // cdecl (id): forwards (ESI, id, the return address) to lookup2Impl.
    // the naked stub reads `id` from the stack in assembly
#pragma warning(suppress : 4100)
    __declspec(naked) uint8_t* __cdecl hookLookup2(int16_t id)
    {
        __asm
        {
            push dword ptr [esp]
            push dword ptr [esp + 8]
            push esi
            call lookup2Impl
            add esp, 12
            ret
        }
    }

    // The alias a pool record answers to; 0 for the game's own records.
    uint32_t aliasOfRecord(const uint8_t* record)
    {
        std::lock_guard lock(g_aliasLock);
        for (const auto& [id, info] : g_aliases)
        {
            if (record == recordAt(info.which, info.index))
            {
                return id;
            }
        }
        return 0;
    }

    uint32_t usable(uint32_t vanilla, uint8_t* record, uint32_t heroType, char flag)
    {
        if (!g_usableActive.load(std::memory_order_relaxed) || !flag || !record || heroType < 1 || heroType > 9)
        {
            return vanilla;
        }
        int16_t id = 0;
        std::memcpy(&id, record, 2);
        const uint32_t alias = aliasOfRecord(record);
        int answer = 0;
        if (LuaHost::runeUsable(static_cast<int>(heroType), alias ? static_cast<int>(alias) : id, (vanilla & 0xFF) != 0 ? 1 : 0, answer) ==
            LuaHost::Answer::Value)
        {
            return answer ? 1 : 0;
        }
        return vanilla;
    }

    uint32_t __fastcall hookUsable1(uint8_t* record, void* edx, uint32_t heroType, char flag)
    {
        return usable(g_origUsable1(record, edx, heroType, flag), record, heroType, flag);
    }

    uint32_t __fastcall hookUsable2(uint8_t* record, void* edx, uint32_t heroType, char flag)
    {
        return usable(g_origUsable2(record, edx, heroType, flag), record, heroType, flag);
    }

    uint32_t __fastcall hookLinkedSpell(uint8_t* record, void* edx)
    {
        const uint32_t vanilla = g_origLinked(record, edx);
        if (vanilla == 0 || !g_haveAliases.load(std::memory_order_acquire))
        {
            return vanilla;
        }
        const uint32_t alias = aliasOfRecord(record);
        if (alias)
        {
            std::lock_guard lock(g_aliasLock);
            const auto it = g_links.find(alias);
            if (it != g_links.end())
            {
                return it->second;
            }
        }
        return vanilla;
    }

    const uint8_t* findBase(uint32_t base, int& which)
    {
        __try
        {
            if (base >= 1000 && base < 2000)
            {
                which = 1;
                return g_origLookup1(static_cast<int16_t>(base));
            }
            if (base >= 1 && base < 1000)
            {
                which = 2;
                return g_origLookup2(static_cast<int16_t>(base));
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return nullptr;
    }

    bool heroRecordAddress(uint32_t type, uintptr_t& out)
    {
        if (!Addr::g_pTypeManager || type < 1 || type > 9)
        {
            return false;
        }
        uintptr_t types = 0;
        uint16_t index = 0;
        uintptr_t begin = 0;
        if (!readAt(Addr::g_pTypeManager.address, 0, types) || !types || !readAt(types, type * kTypeSize + 0x1A, index) ||
            !readAt(types, 0x563008, begin) || !begin)
        {
            return false;
        }
        out = begin + index * 0x56u;
        return true;
    }
}

bool Classes::installAliases()
{
    bool ok = true;
    if (Addr::abilityLookup1 && Addr::abilityLookup2)
    {
        ok = Patch::hook(g_origLookup1, Addr::abilityLookup1, hookLookup1, "abilityLookup1") && ok;
        ok = Patch::hook(g_origLookup2, Addr::abilityLookup2, hookLookup2, "abilityLookup2") && ok;
    }
    if (Addr::abilityLinkedSpell)
    {
        ok = Patch::hook(g_origLinked, Addr::abilityLinkedSpell, hookLinkedSpell, "abilityLinkedSpell") && ok;
    }
    if (Addr::textResources_get)
    {
        ok = Patch::hook(g_origTextGet, Addr::textResources_get, hookTextGet, "textResources_get") && ok;
    }
    if (Addr::abilityUsable1 && Addr::abilityUsable2)
    {
        ok = Patch::hook(g_origUsable1, Addr::abilityUsable1, hookUsable1, "abilityUsable1") && ok;
        ok = Patch::hook(g_origUsable2, Addr::abilityUsable2, hookUsable2, "abilityUsable2") && ok;
    }
    return ok;
}

void Classes::setUsableActive(bool active)
{
    g_usableActive.store(active);
}

bool Classes::aliasCreate(uint32_t alias, uint32_t base, const char*& error)
{
    const bool artAlias = alias >= kAliasArtFirst && alias <= kAliasArtLast;
    if (!artAlias && !(alias >= kAliasSpellFirst && alias <= kAliasSpellLast))
    {
        error = "an alias id is 2000..4999 (a copy of an art) or 100..199 (a copy of a spell)";
        return false;
    }
    if (!g_origLookup1 || !g_origLookup2)
    {
        error = "the ability lookups were not found";
        return false;
    }
    int which = 0;
    const uint8_t* source = findBase(base, which);
    if (!source || (which == 1) != artAlias)
    {
        error = "the base ability does not exist, or is not an art (alias 2000..4999) / spell (alias 100..199)";
        return false;
    }
    std::lock_guard lock(g_aliasLock);
    if (g_aliases.count(alias))
    {
        error = "the alias exists already";
        return false;
    }
    size_t& used = which == 1 ? g_used1 : g_used2;
    const size_t cap = which == 1 ? kPool1Max : kPool2Max;
    size_t index = 0;
    for (; index < used; ++index)
    {
        bool taken = false;
        for (const auto& [id, info] : g_aliases)
        {
            taken = taken || (info.which == which && info.index == index);
        }
        if (!taken)
        {
            break;
        }
    }
    if (index >= cap)
    {
        error = "no room for another alias of that kind";
        return false;
    }
    if (index == used)
    {
        ++used;
    }
    std::memcpy(recordAt(which, index), source, which == 1 ? kPool1Stride : kPool2Stride);
    g_aliases[alias] = {base, which, index};
    g_haveAliases.store(true, std::memory_order_release);
    return true;
}

bool Classes::aliasRemove(uint32_t alias)
{
    std::lock_guard lock(g_aliasLock);
    g_links.erase(alias);
    return g_aliases.erase(alias) != 0;
}

bool Classes::aliasLink(uint32_t artAlias, uint32_t spellAlias, const char*& error)
{
    std::lock_guard lock(g_aliasLock);
    const auto a = g_aliases.find(artAlias);
    if (a == g_aliases.end() || a->second.which != 1)
    {
        error = "the art alias does not exist";
        return false;
    }
    if (spellAlias == 0)
    {
        g_links.erase(artAlias);
        return true;
    }
    const auto s = g_aliases.find(spellAlias);
    if (s == g_aliases.end() || s->second.which != 2)
    {
        error = "the spell alias does not exist";
        return false;
    }
    g_links[artAlias] = spellAlias;
    return true;
}

namespace
{
    // The spell alias an art alias is linked to (0: none); `base` becomes the base art id of an alias.
    uint32_t resolveLink(uint32_t& base)
    {
        std::lock_guard lock(g_aliasLock);
        const auto link = g_links.find(base);
        if (link != g_links.end())
        {
            return link->second;
        }
        const auto alias = g_aliases.find(base);
        if (alias != g_aliases.end())
        {
            base = alias->second.base;
        }
        return 0;
    }
}

uint32_t Classes::linkedSpell(uint32_t base)
{
    if (const uint32_t linked = resolveLink(base))
    {
        return linked;
    }
    if (!Addr::abilityLinkedSpell || !g_origLookup1)
    {
        return 0;
    }
    __try
    {
        uint8_t* record = g_origLookup1(static_cast<int16_t>(base));
        return record ? (g_origLinked ? g_origLinked(record, nullptr) : 0) & 0xFFFF : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

void Classes::aliasClear()
{
    std::lock_guard lock(g_aliasLock);
    g_links.clear();
    g_aliases.clear();
}

bool Classes::aliasPool(int which, uintptr_t& first, size_t& count)
{
    first = reinterpret_cast<uintptr_t>(which == 1 ? g_pool1 : g_pool2);
    count = which == 1 ? g_used1 : g_used2;
    return true;
}

uint32_t Classes::aliasId(int which, size_t index, uint32_t& base)
{
    std::lock_guard lock(g_aliasLock);
    for (const auto& [id, info] : g_aliases)
    {
        if (info.which == which && info.index == index)
        {
            base = info.base;
            return id;
        }
    }
    return 0;
}

bool Classes::heroRecord(uint32_t type, uint8_t (&out)[0x56])
{
    uintptr_t address = 0;
    return heroRecordAddress(type, address) && readAt(address, 0, out);
}

bool Classes::setHeroRecord(uint32_t type, uint32_t offset, const uint8_t* bytes, uint32_t length)
{
    uintptr_t address = 0;
    if (offset + length > 0x56 || !heroRecordAddress(type, address))
    {
        return false;
    }
    __try
    {
        std::memcpy(reinterpret_cast<void*>(address + offset), bytes, length);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

uint32_t Classes::addText(const char* utf8, const char*& error)
{
    wchar_t wide[1024] = {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, wide, 1023);
    if (n < 1)
    {
        error = "the text is not valid UTF-8 or too long";
        return 0;
    }
    std::lock_guard lock(g_textLock);
    if (g_nextText > kTextFirst + 0xFFFF)
    {
        error = "no room for another text";
        return 0;
    }
    const uint32_t id = g_nextText++;
    storeText(g_texts[id], wide);
    g_haveTexts.store(true, std::memory_order_release);
    return id;
}

namespace
{
    bool engineSetText(uint32_t id, const char* utf8, const char*& error)
    {
        if (!Addr::textResources_set || !Addr::textResources_instance || !Addr::engine_alloc)
        {
            error = "the game's text functions were not found";
            return false;
        }
        wchar_t wide[1024] = {};
        const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, wide, 1023);
        if (n < 1)
        {
            error = "the text is not valid UTF-8 or too long";
            return false;
        }
        const int length = n - 1;
        __try
        {
            Addr::textResources_instance();
            auto* buffer = static_cast<wchar_t*>(Addr::engine_alloc(static_cast<uint32_t>((length + 1) * 2)));
            if (!buffer)
            {
                error = "out of memory";
                return false;
            }
            std::memcpy(buffer, wide, length * 2);
            buffer[length] = 0;
            using SetFn = void(__stdcall*)(uint32_t id, wchar_t* begin, wchar_t* end, wchar_t* capacity);
            reinterpret_cast<SetFn>(Addr::textResources_set)(id, buffer, buffer + length, buffer + length + 1);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            error = "the game faulted while setting the text";
            return false;
        }
    }
}

bool Classes::setText(uint32_t id, const char* utf8, const char*& error)
{
    if (id >= kTextFirst && id <= kTextFirst + 0xFFFF)
    {
        wchar_t wide[1024] = {};
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, wide, 1023) < 1)
        {
            error = "the text is not valid UTF-8 or too long";
            return false;
        }
        std::lock_guard lock(g_textLock);
        if (!g_texts.count(id))
        {
            error = "no such text of Gear's own";
            return false;
        }
        storeText(g_texts[id], wide);
        return true;
    }
    return engineSetText(id, utf8, error);
}

bool Classes::cursor(uint32_t exclude, Cursor& out)
{
    if (!Addr::g_uiEventTarget || !Addr::cWorldView_pick || !Addr::g_pApp)
    {
        LOG("Classes: cursor: addresses missing");
        return false;
    }
    uintptr_t engine = 0, view = 0;
    uint16_t index = 0;
    uint8_t* app = *Addr::g_pApp.get();
    HWND window = nullptr;
    if (!readAt(Addr::g_uiEventTarget.address, 0, engine) || !engine || !readAt(engine, 0x48, index) || index >= 8 ||
        !readAt(engine, 8 + index * 4, view) || !view || !app || !readAt(reinterpret_cast<uintptr_t>(app), 0xB0, window) || !window)
    {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 3)
        {
            LOG("Classes: cursor: engine {:08x} index {} view {:08x} app {:08x} window {:08x}", engine, index, view, reinterpret_cast<uintptr_t>(app),
                reinterpret_cast<uintptr_t>(window));
        }
        return false;
    }
    POINT p{};
    if (!GetCursorPos(&p) || !ScreenToClient(window, &p))
    {
        LOG("Classes: cursor: no cursor position ({})", GetLastError());
        return false;
    }
    out.pixelX = p.x;
    out.pixelY = p.y;
    __try
    {
        using PickFn = uint32_t(__fastcall*)(void* view, void* edx, int x, int y, uint32_t exclude, int32_t* rect);
        out.object = reinterpret_cast<PickFn>(Addr::cWorldView_pick - 0x30)(reinterpret_cast<void*>(view), nullptr, p.x, p.y, exclude, nullptr);
        struct Point
        {
            uint8_t head[4];
            int32_t x, y;
            uint8_t layer;
            uint8_t pad[3];
        } point = {};
        using ScreenToWorld = void(__fastcall*)(void* self, void* edx, int x, int y, Point* out);
        uintptr_t vtable = 0, fn = 0;
        std::memcpy(&vtable, reinterpret_cast<void*>(view), 4);
        std::memcpy(&fn, reinterpret_cast<void*>(vtable + 0x18), 4);
        reinterpret_cast<ScreenToWorld>(fn)(reinterpret_cast<void*>(view), nullptr, p.x, p.y, &point);
        out.x = point.x;
        out.y = point.y;
        out.layer = point.layer;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG("Classes: cursor: the game faulted on the pick or the view");
        return false;
    }
}

namespace
{
    std::set<uint32_t> g_definedTypes;

    bool typeRecordAddress(uint32_t type, uintptr_t& out)
    {
        if (!Addr::g_pTypeManager || type < 1 || type >= 0x7E60)
        {
            return false;
        }
        uintptr_t types = 0;
        if (!readAt(Addr::g_pTypeManager.address, 0, types) || !types)
        {
            return false;
        }
        out = types + type * kTypeSize;
        return true;
    }

    bool copyTypeRecord(uintptr_t dst, uintptr_t src, uint32_t art, uint32_t nameText)
    {
        __try
        {
            std::memcpy(reinterpret_cast<void*>(dst), reinterpret_cast<const void*>(src), kTypeSize);
            if (art)
            {
                const uint16_t a = static_cast<uint16_t>(art);
                std::memcpy(reinterpret_cast<void*>(dst + 0x6A), &a, 2);
            }
            if (nameText)
            {
                std::memcpy(reinterpret_cast<void*>(dst + 0x20), &nameText, 4);
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool recordEmpty(uintptr_t record)
    {
        uint8_t bytes[kTypeSize] = {};
        if (!readAt(record, 0, bytes))
        {
            return false;
        }
        for (uint8_t b : bytes)
        {
            if (b)
            {
                return false;
            }
        }
        return true;
    }

    int callUseItem(uintptr_t creature, uint32_t item)
    {
        __try
        {
            return Addr::cCreature_useItem(reinterpret_cast<void*>(creature), item) & 0xFF ? 1 : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }
}

bool Classes::defineItemType(uint32_t type, uint32_t base, uint32_t art, uint32_t nameText, const char*& error)
{
    uintptr_t dst = 0, src = 0;
    if (!typeRecordAddress(type, dst) || !typeRecordAddress(base, src))
    {
        error = "the type table is not there, or a type id is out of 1..0x7E5F";
        return false;
    }
    if (type == base || (!g_definedTypes.count(type) && !recordEmpty(dst)))
    {
        error = "the type record is taken by the game";
        return false;
    }
    if (recordEmpty(src))
    {
        error = "the base type has no record";
        return false;
    }
    if (!copyTypeRecord(dst, src, art, nameText))
    {
        error = "the game faulted on the type record";
        return false;
    }
    g_definedTypes.insert(type);
    return true;
}

bool Classes::useItem(uint32_t creature, uint32_t item, const char*& error)
{
    const uintptr_t o = World::objectAddress(creature);
    if (!o || !Addr::cCreature_useItem)
    {
        error = "no such creature, or the game's item use was not found";
        return false;
    }
    const int r = callUseItem(o, item);
    if (r < 0)
    {
        error = "the game faulted on the item";
        return false;
    }
    if (r == 0)
    {
        error = "the game refused the item (not a potion, scroll or rune, or not usable now)";
        return false;
    }
    return true;
}

uint32_t Classes::artInFlight(uint32_t id)
{
    const uintptr_t o = World::objectAddress(id);
    uint16_t art = 0;
    return o && readAt(o, 0x120, art) ? art : 0;
}
