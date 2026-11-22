#include "pfx.h"

#include "game/gear_addr.h"
#include "log.h"
#include "lua_host.h"
#include "patch.h"
#include "world.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <map>
#include <set>
#include <unordered_map>

namespace
{
    using namespace Gear;

    constexpr uint32_t kFxFirst = 0x300, kFxLast = 0x49D;   // the object types cObjectFX's setup knows (the byte table at 0x5A3570)
    constexpr uintptr_t kFxHandle = 0x6E;                   // cObjectFX: the u16 handle of its particle system
    constexpr uintptr_t kFxObjectId = 0x0C, kFxOwner = 0x30, kFxLevel = 0xE4;   // cObjectFX: its id, the object that made it, the cast level
    constexpr uintptr_t kAbilityInFlight = 0x120;                              // a creature: the art or spell it is casting
    constexpr uintptr_t kMgrVector = 0x18, kMgrVectorEnd = 0x1C;
    constexpr uint32_t kFvfLit = 0x1E2;                     // the vertices the sprite drawing sends: x y z, reserved, diffuse, specular, u v (32 bytes)
    constexpr size_t kVertexSize = 32, kDiffuseOffset = 16;
    constexpr int kDrawPrimitiveSlot = 25;                  // IDirect3DDevice7::DrawPrimitive (+100)

#include "game/gear_pfx.inc"

    // A new effect waits for its first update, which decides its variant (the owner and level are set by then).
    struct Pending
    {
        uintptr_t fx = 0;
        uint32_t type = 0;
    };

    std::map<uint32_t, Pfx::Variant> g_variants;
    std::unordered_map<uintptr_t, Pending> g_pending;
    std::unordered_map<uint32_t, uint32_t> g_fxCaster;      // effect object id -> the creature that made it (through its parents)
    std::unordered_map<uint32_t, uint32_t> g_fxVariant;     // effect object id -> its variant
    std::unordered_map<uintptr_t, uint32_t> g_instances;    // particle system -> variant id
    std::set<uint32_t> g_watch;
    bool g_watchAll = false;
    uint64_t g_applied = 0;

    const Pfx::Variant* g_tint = nullptr;                   // the variant whose system is being drawn (vertex colours)

    decltype(Addr::cObjectFX_setup)::Ptr g_origSetup = nullptr;
    decltype(Addr::particleMgr_add)::Ptr g_origAdd = nullptr;
    decltype(Addr::particleMgr_update)::Ptr g_origUpdate = nullptr;
    decltype(Addr::textureMgr_find)::Ptr g_origFind = nullptr;
    uintptr_t g_origDraw = 0;

    bool g_installed = false;

    template <class T>
    bool readAt(const void* base, uintptr_t offset, T& out)
    {
        __try
        {
            out = *reinterpret_cast<const T*>(reinterpret_cast<uintptr_t>(base) + offset);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The system of a handle in the particle manager, 0 if there is none.
    uintptr_t systemOf(void* manager, uint32_t handle)
    {
        uintptr_t begin = 0, end = 0;
        if (!manager || !readAt(manager, kMgrVector, begin) || !readAt(manager, kMgrVectorEnd, end) || !begin)
        {
            return 0;
        }
        handle &= 0xFFFF;
        if (begin + (handle + 1) * 4 > end)
        {
            return 0;
        }
        uintptr_t system = 0;
        return readAt(reinterpret_cast<void*>(begin), handle * 4, system) ? system : 0;
    }

    void* manager()
    {
        return Addr::g_pParticleMgr ? *Addr::g_pParticleMgr : nullptr;
    }

    // ---- colours ---------------------------------------------------------------------------------------------------------

    void toHsv(float r, float g, float b, float& h, float& s, float& v)
    {
        const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
        v = mx;
        s = mx > 0 ? d / mx : 0;
        if (d <= 0)
        {
            h = 0;
        }
        else if (mx == r)
        {
            h = 60 * std::fmod((g - b) / d, 6.0f);
        }
        else if (mx == g)
        {
            h = 60 * ((b - r) / d + 2);
        }
        else
        {
            h = 60 * ((r - g) / d + 4);
        }
        if (h < 0)
        {
            h += 360;
        }
    }

    void fromHsv(float h, float s, float v, float& r, float& g, float& b)
    {
        h = std::fmod(h, 360.0f);
        if (h < 0)
        {
            h += 360;
        }
        const float c = v * s, x = c * (1 - std::fabs(std::fmod(h / 60, 2.0f) - 1)), m = v - c;
        float rr = 0, gg = 0, bb = 0;
        if (h < 60) { rr = c; gg = x; }
        else if (h < 120) { rr = x; gg = c; }
        else if (h < 180) { gg = c; bb = x; }
        else if (h < 240) { gg = x; bb = c; }
        else if (h < 300) { rr = x; bb = c; }
        else { rr = c; bb = x; }
        r = rr + m;
        g = gg + m;
        b = bb + m;
    }

    uint8_t clampByte(float v)
    {
        return static_cast<uint8_t>(std::clamp(v + 0.5f, 0.0f, 255.0f));
    }

    uint32_t recolor(const Pfx::Variant& v, uint32_t argb)
    {
        float a = static_cast<float>(argb >> 24), r = static_cast<float>((argb >> 16) & 0xFF), g = static_cast<float>((argb >> 8) & 0xFF),
              b = static_cast<float>(argb & 0xFF);
        if (v.hasHue)
        {
            float h, s, val;
            toHsv(r / 255, g / 255, b / 255, h, s, val);
            fromHsv(h + v.hue, s, val, r, g, b);
            r *= 255;
            g *= 255;
            b *= 255;
        }
        if (v.hasColorize)
        {
            // The vertex keeps its brightness; how far it moves to the colour follows its own saturation, so white cores stay white.
            float h, s, val;
            toHsv(r / 255, g / 255, b / 255, h, s, val);
            const float tr = static_cast<float>((v.colorize >> 16) & 0xFF), tg = static_cast<float>((v.colorize >> 8) & 0xFF),
                        tb = static_cast<float>(v.colorize & 0xFF);
            const float tm = std::max({tr, tg, tb, 1.0f});
            const float k = std::min(1.0f, s * 1.5f);
            const float peak = val * 255;
            r = peak * (1 - k + k * tr / tm);
            g = peak * (1 - k + k * tg / tm);
            b = peak * (1 - k + k * tb / tm);
        }
        if (v.hasMultiply)
        {
            a = a * static_cast<float>(v.multiply >> 24) / 255;
            r = r * static_cast<float>((v.multiply >> 16) & 0xFF) / 255;
            g = g * static_cast<float>((v.multiply >> 8) & 0xFF) / 255;
            b = b * static_cast<float>(v.multiply & 0xFF) / 255;
        }
        a *= v.alpha;
        return static_cast<uint32_t>(clampByte(a)) << 24 | static_cast<uint32_t>(clampByte(r)) << 16 | static_cast<uint32_t>(clampByte(g)) << 8 |
               clampByte(b);
    }

    bool changesColor(const Pfx::Variant& v)
    {
        return v.hasHue || v.hasColorize || v.hasMultiply || v.alpha != 1.0f;
    }

    // ---- the device's DrawPrimitive ---------------------------------------------------------------------------------------

    using DrawPrimitiveFn = HRESULT(__stdcall*)(void* self, DWORD type, DWORD fvf, void* vertices, DWORD count, DWORD flags);
    DrawPrimitiveFn g_origDrawPrimitive = nullptr;
    std::set<void**> g_patchedVtables;

    HRESULT __stdcall hookDrawPrimitive(void* self, DWORD type, DWORD fvf, void* vertices, DWORD count, DWORD flags)
    {
        if (g_tint && fvf == kFvfLit && vertices && count)
        {
            static thread_local std::vector<uint8_t> copy;
            copy.resize(static_cast<size_t>(count) * kVertexSize);
            std::memcpy(copy.data(), vertices, copy.size());
            for (size_t i = 0; i < count; ++i)
            {
                uint32_t* diffuse = reinterpret_cast<uint32_t*>(copy.data() + i * kVertexSize + kDiffuseOffset);
                *diffuse = recolor(*g_tint, *diffuse);
            }
            return g_origDrawPrimitive(self, type, fvf, copy.data(), count, flags);
        }
        return g_origDrawPrimitive(self, type, fvf, vertices, count, flags);
    }

    // Puts hookDrawPrimitive into the vtable of the device object (once per vtable; the call is slot 25 of a Direct3D 7 device).
    void patchDevice(void* device)
    {
        void** vtable = nullptr;
        if (!device || !readAt(device, 0, vtable) || !vtable)
        {
            return;
        }
        void* current = nullptr;
        if (!readAt(vtable, kDrawPrimitiveSlot * sizeof(void*), current) || !current)
        {
            return;
        }
        if (current == reinterpret_cast<void*>(hookDrawPrimitive))
        {
            return;
        }
        // A vtable that is patched already but was changed back by another layer is patched again; the previous function is the one to call.
        DWORD old = 0;
        void** slot = vtable + kDrawPrimitiveSlot;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old))
        {
            return;
        }
        g_origDrawPrimitive = reinterpret_cast<DrawPrimitiveFn>(current);
        *slot = reinterpret_cast<void*>(hookDrawPrimitive);
        VirtualProtect(slot, sizeof(void*), old, &old);
        if (g_patchedVtables.insert(vtable).second)
        {
            LOG("Pfx: patched DrawPrimitive of the device vtable {}", static_cast<const void*>(vtable));
        }
    }

    // ---- the sprite drawing ----------------------------------------------------------------------------------------------

    uintptr_t g_drawReturn = 0;
    bool g_inDraw = false;

    // Called on entry of the shared sprite drawing: does this system look different?
    extern "C" int __cdecl pfxDrawEnter(void* system, void* device)
    {
        if (g_instances.empty() || g_inDraw)
        {
            return 0;
        }
        const auto it = g_instances.find(reinterpret_cast<uintptr_t>(system));
        if (it == g_instances.end())
        {
            return 0;
        }
        const auto v = g_variants.find(it->second);
        if (v == g_variants.end() || !changesColor(v->second))
        {
            return 0;
        }
        patchDevice(device);
        g_tint = &v->second;
        g_inDraw = true;
        return 1;
    }

    extern "C" void __cdecl pfxDrawLeave()
    {
        g_tint = nullptr;
        g_inDraw = false;
    }

    // The detour of the shared drawing (ret 0x60, thiscall: ECX the system, [esp + 4] the device): it asks pfxDrawEnter, and swaps the
    // return address for a stub that ends the tint, so the 22 stack words and the callee's cleanup stay as the engine has them.
    __declspec(naked) void hookDrawSprites()
    {
        __asm
        {
            push ecx
            push dword ptr [esp + 8]
            push ecx
            call pfxDrawEnter
            add esp, 8
            mov edx, eax
            pop ecx
            test edx, edx
            jz pfxPlain
            mov eax, [esp]
            mov g_drawReturn, eax
            mov dword ptr [esp], offset pfxDone
        pfxPlain:
            jmp g_origDraw
        pfxDone:
            call pfxDrawLeave
            push g_drawReturn
            ret
        }
    }

    // ---- the effect setup ------------------------------------------------------------------------------------------------

    void* g_textureManager = nullptr;       // what the engine's lookups use as `this` (captured from the first call)

    uint32_t __fastcall hookFind(void* self, void* edx, const char* name)
    {
        g_textureManager = self;
        return g_origFind(self, edx, name);
    }

    uint32_t __fastcall hookAdd(void* mgr, void* edx, void* system)
    {
        const uint32_t handle = g_origAdd(mgr, edx, system);
        const uintptr_t key = reinterpret_cast<uintptr_t>(system);
        if (!g_instances.empty())
        {
            g_instances.erase(key);     // a new system at an address a dead variant's had
        }
        g_pending.erase(key);
        return handle;
    }

    // Replaces the texture handle of `from` by the one of `to` in the system object (the constructors store the handles the lookup gave
    // them in fields; the object's size is known from the effect type, so the scan stays inside it).
    void swapTextures(uintptr_t system, uint32_t fxType, const Pfx::Variant& v)
    {
        if (v.textures.empty() || !g_textureManager || fxType < kFxFirst || fxType > kFxLast)
        {
            return;
        }
        const size_t size = kFxClasses[fxType - kFxFirst].size;
        if (size < 0x80)
        {
            return;
        }
        for (const auto& [from, to] : v.textures)
        {
            const uint32_t hf = g_origFind(g_textureManager, nullptr, from.c_str());
            const uint32_t ht = g_origFind(g_textureManager, nullptr, to.c_str());
            if (hf < 0x10 || !ht || hf == ht)
            {
                LOG("Pfx: variant {} texture {} -> {} not applied (handles {:X} -> {:X})", v.id, from, to, hf, ht);
                continue;
            }
            size_t replaced = 0;
            __try
            {
                uint32_t* words = reinterpret_cast<uint32_t*>(system);
                for (size_t i = 0x20 / 4; i < size / 4; ++i)
                {
                    if (words[i] == hf)
                    {
                        words[i] = ht;
                        ++replaced;
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            if (!replaced)
            {
                LOG("Pfx: variant {} texture {} is not used by effect {:X}", v.id, from, fxType);
            }
        }
    }

    // The first update of a new effect's system: the effect object has its owner (+0x30) and level (+0xE4) by now. Decides the variant.
    void resolve(uintptr_t system, const Pending& p)
    {
        uint32_t objectId = 0, owner = 0, level = 0;
        readAt(reinterpret_cast<void*>(p.fx), kFxObjectId, objectId);
        readAt(reinterpret_cast<void*>(p.fx), kFxOwner, owner);
        readAt(reinterpret_cast<void*>(p.fx), kFxLevel, level);
        uint32_t creature = 0, parentVariant = 0;
        if (owner)
        {
            if (const auto c = g_fxCaster.find(owner); c != g_fxCaster.end())
            {
                creature = c->second;
            }
            else
            {
                World::Object o;
                if (World::object(owner, o) && o.creature)
                {
                    creature = owner;
                }
            }
            if (const auto pv = g_fxVariant.find(owner); pv != g_fxVariant.end())
            {
                parentVariant = pv->second;
            }
        }
        if (g_fxCaster.size() > 8192 || g_fxVariant.size() > 8192)
        {
            g_fxCaster.clear();
            g_fxVariant.clear();
        }
        if (objectId && creature)
        {
            g_fxCaster[objectId] = creature;
        }
        uint32_t chosen = 0;
        if ((g_watchAll || g_watch.count(p.type)))
        {
            uint32_t ability = 0;
            if (creature)
            {
                // The art or spell in flight is a word of the creature object (+0x120).
                uint16_t a = 0;
                if (const uintptr_t c = World::objectAddress(creature); c && readAt(reinterpret_cast<void*>(c), kAbilityInFlight, a))
                {
                    ability = a;
                }
            }
            uint32_t answer = 0;
            if (LuaHost::pfxSelect(p.type, creature, ability, level, parentVariant, answer) == LuaHost::Answer::Value)
            {
                chosen = answer;
            }
        }
        if (!chosen)
        {
            chosen = parentVariant;     // the effects of an effect (an impact, a trail) look like their parent
        }
        const auto it = chosen ? g_variants.find(chosen) : g_variants.end();
        if (it == g_variants.end())
        {
            return;
        }
        g_instances[system] = chosen;
        if (objectId)
        {
            g_fxVariant[objectId] = chosen;
        }
        ++g_applied;
        swapTextures(system, p.type, it->second);
    }

    void __fastcall hookUpdate(void* mgr, void* edx, uint32_t handle, float dt)
    {
        if (!g_pending.empty() || !g_instances.empty())
        {
            const uintptr_t system = systemOf(mgr, handle);
            if (!g_pending.empty())
            {
                if (const auto p = g_pending.find(system); p != g_pending.end())
                {
                    const Pending pending = p->second;
                    g_pending.erase(p);
                    resolve(system, pending);
                }
            }
            const auto it = g_instances.find(system);
            if (it != g_instances.end())
            {
                const auto v = g_variants.find(it->second);
                if (v != g_variants.end())
                {
                    dt *= v->second.speed;
                }
            }
        }
        g_origUpdate(mgr, edx, handle, dt);
    }

    void __fastcall hookSetup(void* fx, void* edx, uint32_t effect)
    {
        g_origSetup(fx, edx, effect);
        const bool interested = g_watchAll || g_watch.count(effect) || !g_fxVariant.empty();
        if (!interested)
        {
            return;
        }
        uint16_t handle = 0;
        if (readAt(fx, kFxHandle, handle))
        {
            if (const uintptr_t system = systemOf(manager(), handle))
            {
                g_pending[system] = Pending{reinterpret_cast<uintptr_t>(fx), effect};
            }
        }
    }
}

bool Pfx::install()
{
    if (g_installed)
    {
        return true;
    }
    if (!Addr::cObjectFX_setup || !Addr::particleMgr_add || !Addr::particleMgr_update || !Addr::g_pParticleMgr || !Addr::textureMgr_find ||
        !Addr::particle_drawSprites)
    {
        LOG("Pfx: addresses missing, gear.pfx changes no effects");
        return false;
    }
    g_installed = true;
    bool ok = true;
    ok = Patch::hook(g_origSetup, Addr::cObjectFX_setup, hookSetup, "cObjectFX_setup") && ok;
    ok = Patch::hook(g_origAdd, Addr::particleMgr_add, hookAdd, "particleMgr_add") && ok;
    ok = Patch::hook(g_origUpdate, Addr::particleMgr_update, hookUpdate, "particleMgr_update") && ok;
    ok = Patch::hook(g_origFind, Addr::textureMgr_find, hookFind, "textureMgr_find") && ok;
    g_origDraw = Addr::particle_drawSprites;
    ok = Patch::hook(reinterpret_cast<void**>(&g_origDraw), reinterpret_cast<void*>(hookDrawSprites), "particle_drawSprites") && ok;
    LOG("Pfx: hooks queued");
    return ok;
}

bool Pfx::define(const Variant& variant, const char*& error)
{
    if (variant.id == 0)
    {
        error = "a variant needs an id";
        return false;
    }
    if (!(variant.speed > 0.01f && variant.speed < 100))
    {
        error = "speed must be between 0.01 and 100";
        return false;
    }
    g_variants[variant.id] = variant;
    // Systems that carry an older definition of the id now follow the new one (the map holds ids, not copies).
    return true;
}

bool Pfx::undefine(uint32_t id)
{
    for (auto it = g_instances.begin(); it != g_instances.end();)
    {
        it = it->second == id ? g_instances.erase(it) : std::next(it);
    }
    return g_variants.erase(id) != 0;
}

const Pfx::Variant* Pfx::find(uint32_t id)
{
    const auto it = g_variants.find(id);
    return it == g_variants.end() ? nullptr : &it->second;
}

void Pfx::watch(const std::vector<uint32_t>& fxTypes, bool all)
{
    g_watch.clear();
    g_watch.insert(fxTypes.begin(), fxTypes.end());
    g_watchAll = all;
}

size_t Pfx::liveCount()
{
    return g_instances.size();
}

uint64_t Pfx::appliedCount()
{
    return g_applied;
}
