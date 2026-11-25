#include "buffs.h"
#include "log.h"
#include "patch.h"
#include "world.h"
#include "game/gear_addr.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

// Gear buffs (see buffs.h; RE notes in docs/RE_NOTES.md, "Buffs").
namespace Buffs
{
    const uint16_t kFields[] = {
        0x66, 0x6A, 0x6E, 0x72,                         // resistances: physical, fire, magic, poison
        0x76, 0x7A, 0x7E, 0x82, 0x86, 0x8A, 0x8E, 0x92, // damage bonuses: min physical, fire, magic, poison; then max
        0x96, 0x9A, 0x9E, 0xA2,
        0xA6, 0xAA, 0xAE, 0xB2, 0xB6, 0xBA, 0xBE, 0xC2,
        0xC6, 0xCA, 0xCE, 0xD2,                         // factors (1 before)
        0xD6, 0xDA, 0xDE, 0xE2,
        0xE6, 0xEA, 0xEE, 0xF2,                         // factors (1 before): speed, armour, regeneration ...
    };
    const size_t kFieldCount = std::size(kFields);

    const uint16_t kSpeedFields[] = {0x3C, 0x3E, 0x40};
    const size_t kSpeedFieldCount = std::size(kSpeedFields);

    bool isSpeedField(uint16_t offset)
    {
        return std::find(std::begin(kSpeedFields), std::end(kSpeedFields), offset) != std::end(kSpeedFields);
    }

    bool isField(uint16_t offset)
    {
        return isSpeedField(offset) ||
               std::find(std::begin(kFields), std::end(kFields), offset) != std::end(kFields);
    }
}

namespace
{
    namespace Addr = Gear::Addr;

    constexpr uintptr_t kStats = 0x3A8;         // the creature's stats block
    constexpr uintptr_t kStatsOwner = 4;        // stats +4: its creature
    constexpr uintptr_t kId = 0x0C;
    constexpr uintptr_t kOverlay = 0x60, kOverlayLeft = 0x68;

    struct Running
    {
        Buffs::Buff buff;
        ULONGLONG ends = 0;     // GetTickCount64 at the end, 0: none
    };

    std::map<uint32_t, std::vector<Running>> g_buffs;
    std::vector<std::pair<uint32_t, std::string>> g_ended;
    decltype(Addr::cStats_recalc)::Ptr g_origRecalc = nullptr;
    bool g_installed = false;

    bool safeCopy(void* dst, const void* src, size_t size)
    {
        __try
        {
            std::memcpy(dst, src, size);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    template <class T>
    bool readAt(uintptr_t base, uintptr_t offset, T& out)
    {
        return base && safeCopy(&out, reinterpret_cast<const void*>(base + offset), sizeof(T));
    }

    template <class T>
    bool writeAt(uintptr_t base, uintptr_t offset, const T& value)
    {
        return base && safeCopy(reinterpret_cast<void*>(base + offset), &value, sizeof(T));
    }

    // A creature in the world (with a stats block): its address, else 0.
    uintptr_t creature(uint32_t id)
    {
        World::Object o;
        if (!World::object(id, o) || !o.creature)
        {
            return 0;
        }
        return World::objectAddress(id);
    }

    // What the creature's Gear buffs add to its freshly worked out values: sums first, then factors.
    void addBuffs(uintptr_t stats)
    {
        uintptr_t owner = 0;
        uint32_t id = 0;
        if (g_buffs.empty() || !readAt(stats, kStatsOwner, owner) || !readAt(owner, kId, id))
        {
            return;
        }
        const auto it = g_buffs.find(id);
        if (it == g_buffs.end())
        {
            return;
        }
        for (int pass = 0; pass < 2; ++pass)
        {
            for (const Running& r : it->second)
            {
                for (const Buffs::Mod& m : r.buff.mods)
                {
                    if (m.factor != (pass == 1))
                    {
                        continue;
                    }
                    if (Buffs::isSpeedField(m.offset))
                    {
                        // After the engine's own cap (220, 260 for some creatures): this is what lifts it.
                        int16_t sv = 0;
                        if (readAt(stats, m.offset, sv))
                        {
                            const double nv = m.factor ? sv * static_cast<double>(m.value) : sv + static_cast<double>(m.value);
                            writeAt(stats, m.offset, static_cast<int16_t>(std::clamp(std::lround(nv), 0L,
                                static_cast<long>(Buffs::kMaxSpeed))));
                        }
                        continue;
                    }
                    float v = 0;
                    if (readAt(stats, m.offset, v))
                    {
                        v = m.factor ? v * m.value : v + m.value;
                        writeAt(stats, m.offset, v);
                    }
                }
            }
        }
    }

    void __fastcall hookRecalc(void* stats, void* edx, uint32_t a, uint32_t b)
    {
        g_origRecalc(stats, edx, a, b);
        __try
        {
            addBuffs(reinterpret_cast<uintptr_t>(stats));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    bool callRecalc(uintptr_t stats)
    {
        const auto fn = g_origRecalc ? g_origRecalc : Addr::cStats_recalc.ptr();
        __try
        {
            if (g_origRecalc)
            {
                hookRecalc(reinterpret_cast<void*>(stats), nullptr, 0, 1);
            }
            else
            {
                fn(reinterpret_cast<void*>(stats), nullptr, 0, 1);
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Shows the overlay of the newest running buff that has one (none left: the slot is cleared if it shows one of
    // ours).
    void showOverlay(uint32_t id, int ended)
    {
        const uintptr_t c = creature(id);
        if (!c)
        {
            return;
        }
        int best = 0;
        double left = 0;
        const auto it = g_buffs.find(id);
        if (it != g_buffs.end())
        {
            const ULONGLONG now = GetTickCount64();
            for (const Running& r : it->second)
            {
                if (r.buff.overlay > 0)
                {
                    best = r.buff.overlay;
                    left = r.ends ? (r.ends > now ? (r.ends - now) / 1000.0 : 0) : 3600;
                }
            }
        }
        if (best > 0)
        {
            Buffs::setOverlay(id, best, static_cast<float>(left));
            return;
        }
        int32_t shown = 0;
        if (ended > 0 && readAt(c, kOverlay, shown) && shown == ended)
        {
            Buffs::setOverlay(id, 0, 0);
        }
    }
}

namespace Buffs
{
    void install()
    {
        if (g_installed)
        {
            return;
        }
        g_installed = true;
        if (!Addr::cStats_recalc)
        {
            LOG("Buffs: cStats::recalc was not found: gear.buffs changes no values");
            return;
        }
        Patch::hook(g_origRecalc, Addr::cStats_recalc, hookRecalc, "cStats_recalc");
    }

    void frame()
    {
        if (g_buffs.empty())
        {
            return;
        }
        const ULONGLONG now = GetTickCount64();
        for (auto it = g_buffs.begin(); it != g_buffs.end();)
        {
            const uint32_t id = it->first;
            const uintptr_t c = creature(id);
            std::vector<Running>& list = it->second;
            int endedOverlay = 0;
            bool changed = false;
            for (auto r = list.begin(); r != list.end();)
            {
                if (!c || (r->ends && now >= r->ends))
                {
                    g_ended.emplace_back(id, r->buff.key);
                    if (r->buff.overlay > 0)
                    {
                        endedOverlay = r->buff.overlay;
                    }
                    r = list.erase(r);
                    changed = true;
                }
                else
                {
                    ++r;
                }
            }
            if (list.empty())
            {
                it = g_buffs.erase(it);
            }
            else
            {
                ++it;
            }
            if (changed && c)
            {
                recalc(id);
                showOverlay(id, endedOverlay);
            }
        }
    }

    void clear()
    {
        for (const auto& [id, list] : g_buffs)
        {
            for (const Running& r : list)
            {
                g_ended.emplace_back(id, r.buff.key);
            }
        }
        g_buffs.clear();
    }

    bool apply(uint32_t id, const Buff& buff, const char*& error)
    {
        if (!creature(id))
        {
            error = "not a creature in the world";
            return false;
        }
        if (buff.key.empty())
        {
            error = "a buff needs a key";
            return false;
        }
        for (const Mod& m : buff.mods)
        {
            if (!isField(m.offset))
            {
                error = "a buff changes a field cStats::recalc does not work out";
                return false;
            }
        }
        std::vector<Running>& list = g_buffs[id];
        list.erase(std::remove_if(list.begin(), list.end(), [&](const Running& r) { return r.buff.key == buff.key; }),
            list.end());
        Running r;
        r.buff = buff;
        r.ends = buff.seconds > 0 ? GetTickCount64() + static_cast<ULONGLONG>(buff.seconds * 1000.0) : 0;
        list.push_back(std::move(r));
        recalc(id);
        showOverlay(id, 0);
        return true;
    }

    bool remove(uint32_t id, const std::string& key)
    {
        const auto it = g_buffs.find(id);
        if (it == g_buffs.end())
        {
            return false;
        }
        int endedOverlay = 0;
        bool found = false;
        for (auto r = it->second.begin(); r != it->second.end();)
        {
            if (r->buff.key == key)
            {
                if (r->buff.overlay > 0)
                {
                    endedOverlay = r->buff.overlay;
                }
                g_ended.emplace_back(id, key);
                r = it->second.erase(r);
                found = true;
            }
            else
            {
                ++r;
            }
        }
        if (it->second.empty())
        {
            g_buffs.erase(it);
        }
        if (found)
        {
            recalc(id);
            showOverlay(id, endedOverlay);
        }
        return found;
    }

    std::vector<Buff> list(uint32_t id)
    {
        std::vector<Buff> out;
        const auto it = g_buffs.find(id);
        if (it == g_buffs.end())
        {
            return out;
        }
        const ULONGLONG now = GetTickCount64();
        for (const Running& r : it->second)
        {
            Buff b = r.buff;
            b.left = r.ends ? (r.ends > now ? (r.ends - now) / 1000.0 : 0) : 0;
            out.push_back(std::move(b));
        }
        return out;
    }

    std::vector<std::pair<uint32_t, std::string>> takeEnded()
    {
        std::vector<std::pair<uint32_t, std::string>> out;
        out.swap(g_ended);
        return out;
    }

    bool fields(uint32_t id, std::vector<std::pair<uint16_t, float>>& out)
    {
        out.clear();
        const uintptr_t c = creature(id);
        if (!c)
        {
            return false;
        }
        for (uint16_t f : kFields)
        {
            float v = 0;
            if (!readAt(c + kStats, f, v))
            {
                return false;
            }
            out.emplace_back(f, v);
        }
        return true;
    }

    bool speeds(uint32_t id, std::vector<std::pair<uint16_t, int>>& out)
    {
        out.clear();
        const uintptr_t c = creature(id);
        if (!c)
        {
            return false;
        }
        for (uint16_t f : kSpeedFields)
        {
            int16_t v = 0;
            if (!readAt(c + kStats, f, v))
            {
                return false;
            }
            out.emplace_back(f, v);
        }
        return true;
    }

    bool overlay(uint32_t id, int& which, float& seconds)
    {
        const uintptr_t c = creature(id);
        int32_t w = 0;
        float s = 0;
        if (!c || !readAt(c, kOverlay, w) || !readAt(c, kOverlayLeft, s))
        {
            return false;
        }
        which = w;
        seconds = s;
        return true;
    }

    bool setOverlay(uint32_t id, int which, float seconds)
    {
        const uintptr_t c = creature(id);
        if (!c)
        {
            return false;
        }
        const int32_t w = which > 0 ? which : 0;
        const float s = which > 0 ? std::max(0.0f, seconds) : 0.0f;
        return writeAt(c, kOverlay, w) && writeAt(c, kOverlayLeft, s);
    }

    bool recalc(uint32_t id)
    {
        const uintptr_t c = creature(id);
        return c && (Addr::cStats_recalc || g_origRecalc) && callRecalc(c + kStats);
    }
}
