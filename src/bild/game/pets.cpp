#include "game/pets.h"
#include "diag/layout_check.h"
#include "log.h"
#include "patch.h"
#include "sacred/world.h"

#include <windows.h>
#include <d3dtypes.h>
#include <algorithm>
#include <atomic>
#include <unordered_map>

namespace
{
    using namespace Sacred;

    struct Entry
    {
        float scale = 1.0f;
        bool untargetable = false;
    };

    SRWLOCK g_lock = SRWLOCK_INIT;
    std::unordered_map<uint32_t, Entry> g_pets;
    std::atomic<int> g_count{0};
    std::atomic<int> g_untargetable{0};   // (kept for the status of the registry)
    thread_local float t_scale = 1.0f;
    thread_local float t_ax = 0.0f, t_ay = 0.0f;   // the object's screen position (+0x40 / +0x44): the scale is about it

    // Tints: object id -> a colour for the whole model and for the first kParts meshes (0: none).
    constexpr int kParts = 8;
    struct Tint
    {
        uint32_t all = 0;
        uint32_t part[kParts] = {};
        uint32_t texAll = 0;                // tokens of other textures (Sacred Gear's resolver gives the surface)
        uint32_t texPart[kParts] = {};
    };
    std::unordered_map<uint32_t, Tint> g_tints;
    std::atomic<int> g_tintCount{0};
    thread_local const Tint* t_tint = nullptr;      // the object being drawn (valid while its drawModel runs: the map entry is copied below)
    thread_local Tint t_tintCopy;
    thread_local int t_part = 0;

    decltype(Addr::cGranny_renderShadow)::Ptr g_origShadow = nullptr;
    decltype(Addr::cObject3D_drawModel)::Ptr g_origDrawModel = nullptr;

    // A pet's model handle (object +0x44, the second of the pair at +0x40; the first is the same for every object, which once made every shadow in the frame a pet's) with the scale and anchor the shadow pass needs: the shadow is drawn
    // later, by cModelInstance::draw, which knows the handles of the model but not the object.
    struct Shadow
    {
        float scale, ax, ay;
    };
    std::unordered_map<uint32_t, Shadow> g_shadows;
    std::atomic<int> g_shadowCount{0};   // g_shadows.size(), readable without the lock

    // Reads the pet entry of the object `self` and returns whether it is one; the scale and the anchor go to the out parameters.
    bool petOf(const void* self, float& scale, float& ax, float& ay)
    {
        if (g_count.load(std::memory_order_relaxed) <= 0 || !self)
        {
            return false;
        }
        const uint32_t id = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(self) + 0x0C);
        bool found = false;
        AcquireSRWLockShared(&g_lock);
        const auto it = g_pets.find(id);
        if (it != g_pets.end())
        {
            scale = it->second.scale;
            ax = *reinterpret_cast<const float*>(static_cast<const uint8_t*>(self) + 0x7C);
            ay = *reinterpret_cast<const float*>(static_cast<const uint8_t*>(self) + 0x80);
            found = true;
        }
        ReleaseSRWLockShared(&g_lock);
        return found;
    }

    // The model draw of an object. Sets the scale for the model and remembers the handle for the shadow.
    uint32_t __fastcall hookDrawModel(void* self, void* edx, IDirect3DDevice7* device, void* model, void* instance, uint32_t flagsLow,
        uint32_t flagsHigh)
    {
        float scale = 1.0f, ax = 0.0f, ay = 0.0f;
        const bool pet = petOf(self, scale, ax, ay);
        const float before = t_scale, bx = t_ax, by = t_ay;
        const Tint* beforeTint = t_tint;
        const int beforePart = t_part;
        t_tint = nullptr;
        if (g_tintCount.load(std::memory_order_relaxed) > 0 && self)
        {
            const uint32_t id = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(self) + 0x0C);
            AcquireSRWLockShared(&g_lock);
            const auto it = g_tints.find(id);
            if (it != g_tints.end())
            {
                t_tintCopy = it->second;
                t_tint = &t_tintCopy;
                t_part = 0;
            }
            ReleaseSRWLockShared(&g_lock);
        }
        if (pet)
        {
            t_scale = scale;
            t_ax = ax;
            t_ay = ay;
            const uint32_t handle = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(self) + 0x44);
            AcquireSRWLockExclusive(&g_lock);
            g_shadows[handle] = {scale, ax, ay};
            g_shadowCount.store(static_cast<int>(g_shadows.size()), std::memory_order_relaxed);
            ReleaseSRWLockExclusive(&g_lock);
        }
        else if (g_shadowCount.load(std::memory_order_relaxed) > 0 && self)
        {
            // A model handle outlives its pet: once the pet is dismissed or dies the engine hands the handle to another object. Forget
            // it here, before that object's shadow pass, or an NPC's shadow would be drawn at the old pet's scale and anchor.
            const uint32_t handle = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(self) + 0x44);
            AcquireSRWLockExclusive(&g_lock);
            if (g_shadows.erase(handle) != 0)
            {
                g_shadowCount.store(static_cast<int>(g_shadows.size()), std::memory_order_relaxed);
            }
            ReleaseSRWLockExclusive(&g_lock);
        }
        const uint32_t r = g_origDrawModel(self, edx, device, model, instance, flagsLow, flagsHigh);
        t_scale = before;
        t_ax = bx;
        t_ay = by;
        t_tint = beforeTint;
        t_part = beforePart;
        return r;
    }

    // The shadow of a model: found by the model's handle (object +0x44, the shadow pass's handle1), drawn scaled like the model.
    void __fastcall hookShadow(void* self, void* edx, void* device, uint32_t handle0, uint32_t handle1, uint8_t* extra, char flag)
    {
        Shadow sh{1.0f, 0.0f, 0.0f};
        bool pet = false;
        if (g_count.load(std::memory_order_relaxed) > 0)
        {
            AcquireSRWLockShared(&g_lock);
            const auto it = g_shadows.find(handle1);
            if (it != g_shadows.end())
            {
                sh = it->second;
                pet = true;
            }
            ReleaseSRWLockShared(&g_lock);
        }
        const float before = t_scale, bx = t_ax, by = t_ay;
        const Tint* beforeTint = t_tint;     // a shadow is not tinted
        t_tint = nullptr;
        if (pet)
        {
            t_scale = sh.scale;
            t_ax = sh.ax;
            t_ay = sh.ay;
        }
        g_origShadow(self, edx, device, handle0, handle1, extra, flag);
        t_tint = beforeTint;
        t_scale = before;
        t_ax = bx;
        t_ay = by;
    }

    decltype(Addr::worldPick)::Ptr g_origPick = nullptr;

    // Pets that cannot be targeted are taken out of the pick list before every pick.
    uint32_t __fastcall hookPick(cWorldView* view, void* edx, int x, int y, uint32_t exclude, int32_t* rect)
    {
        Pets::filterPick(view);
        return g_origPick(view, edx, x, y, exclude, rect);
    }

    void recount()
    {
        int untargetable = 0;
        for (const auto& kv : g_pets)
        {
            untargetable += kv.second.untargetable ? 1 : 0;
        }
        g_count.store(static_cast<int>(g_pets.size()));
        g_untargetable.store(untargetable);
        if (g_pets.empty())
        {
            g_shadows.clear();
        }
    }
}

void Pets::install()
{
    Patch::begin();
    Patch::hook(g_origShadow, Addr::cGranny_renderShadow, &hookShadow, "cGranny::renderShadow (pets)");
    Patch::hook(g_origDrawModel, Addr::cObject3D_drawModel, &hookDrawModel, "cObject3D::drawModel (pets)");
    Patch::hook(g_origPick, Addr::worldPick, &hookPick, "worldPick (pets)");
    if (Patch::commit())
    {
        LOG("Pets: scale and pick hooks installed");
    }
}

bool Pets::any()
{
    return g_count.load(std::memory_order_relaxed) > 0;
}

void Pets::filterPick(cWorldView* view)
{
    if (!view || !Sacred::Layout::worldPick() || g_untargetable.load(std::memory_order_relaxed) == 0)
    {
        return;
    }
    EnterCriticalSection(&view->pickLock);
    PickEntry* begin = view->pickBegin;
    PickEntry* end = view->pickEnd;
    if (begin && end > begin && static_cast<size_t>(end - begin) <= 4096)
    {
        AcquireSRWLockShared(&g_lock);
        PickEntry* out = begin;
        for (PickEntry* p = begin; p < end; ++p)
        {
            const auto it = g_pets.find(p->id);
            if (it != g_pets.end())
            {
                if (it->second.untargetable)
                {
                    static std::atomic<int> logged{0};
                    if (logged.fetch_add(1) < 2)
                    {
                        LOG("Pets: object {} left out of the pick list", p->id);
                    }
                    continue;
                }
            }
            if (out != p)
            {
                *out = *p;
            }
            ++out;
        }
        ReleaseSRWLockShared(&g_lock);
        view->pickEnd = out;
    }
    LeaveCriticalSection(&view->pickLock);
}

bool Pets::tinting()
{
    return t_tint != nullptr;
}

bool Pets::tintNext(uint32_t& argb, uint32_t& texture)
{
    argb = 0;
    texture = 0;
    if (!t_tint)
    {
        return false;
    }
    const int part = ++t_part;
    uint32_t c = part >= 1 && part <= kParts ? t_tint->part[part - 1] : 0;
    if (!c)
    {
        c = t_tint->all;
    }
    if (c && c != 0xFFFFFFFFu)
    {
        argb = c;
    }
    uint32_t t = part >= 1 && part <= kParts ? t_tint->texPart[part - 1] : 0;
    texture = t ? t : t_tint->texAll;
    return argb != 0 || texture != 0;
}

namespace
{
    using Resolver = IDirectDrawSurface7*(__cdecl*)(IDirect3DDevice7*, unsigned);
    std::atomic<Resolver> g_resolver{nullptr};
}

IDirectDrawSurface7* Pets::resolveTexture(IDirect3DDevice7* device, uint32_t token)
{
    const Resolver r = g_resolver.load();
    return r && token ? r(device, token) : nullptr;
}

bool Pets::scaling()
{
    return t_scale != 1.0f;
}

void Pets::scaleWorld(_D3DMATRIX& m)
{
    const float s = t_scale;
    if (s == 1.0f)
    {
        return;
    }
    // World models are drawn with vertices already in world space (the matrix is the identity; the shadow pass uses a matrix that
    // flattens them onto the ground), so the pet grows about its own render position at ground level: first A, then the old matrix,
    // out = A * m with A = [s 0 0 0; 0 s 0 0; 0 0 s 0; ax(1-s) ay(1-s) 0 1].
    const float tx = t_ax * (1.0f - s), ty = t_ay * (1.0f - s);
    D3DMATRIX r;
    const float* in = &m._11;
    float* out = &r._11;
    for (int j = 0; j < 4; ++j)
    {
        out[0 * 4 + j] = s * in[0 * 4 + j];
        out[1 * 4 + j] = s * in[1 * 4 + j];
        out[2 * 4 + j] = s * in[2 * 4 + j];
        out[3 * 4 + j] = tx * in[0 * 4 + j] + ty * in[1 * 4 + j] + in[3 * 4 + j];
    }
    m = r;
}

// Sacred Gear: registers (or, with scale 1 and untargetable 0, removes) an object. Returns the number of pets.
extern "C" int __cdecl SacredBild_PetSet(unsigned id, float scale, int untargetable)
{
    AcquireSRWLockExclusive(&g_lock);
    if ((scale == 1.0f || !(scale > 0.0f)) && !untargetable)
    {
        g_pets.erase(id);
    }
    else
    {
        Entry& e = g_pets[id];
        e.scale = std::clamp(scale, 0.1f, 4.0f);
        e.untargetable = untargetable != 0;
    }
    recount();
    const int n = g_count.load();
    ReleaseSRWLockExclusive(&g_lock);
    return n;
}

// Sacred Gear: forgets every pet (a savegame was loaded: the object ids mean other objects now).
extern "C" void __cdecl SacredBild_PetClear()
{
    AcquireSRWLockExclusive(&g_lock);
    g_pets.clear();
    recount();
    ReleaseSRWLockExclusive(&g_lock);
}

// Sacred Gear: tints an object's model: `part` 0 the whole model, 1..8 one mesh (in drawing order); `argb` 0 or 0xFFFFFFFF removes it.
extern "C" int __cdecl SacredBild_TintSet(unsigned id, unsigned part, unsigned argb)
{
    AcquireSRWLockExclusive(&g_lock);
    if (part <= static_cast<unsigned>(kParts))
    {
        const uint32_t value = argb == 0xFFFFFFFFu ? 0 : argb;
        if (value || g_tints.count(id))
        {
            Tint& t = g_tints[id];
            if (part == 0)
            {
                t.all = value;
            }
            else
            {
                t.part[part - 1] = value;
            }
            bool any = t.all != 0 || t.texAll != 0;
            for (int k = 0; k < kParts; ++k)
            {
                any = any || t.part[k] != 0 || t.texPart[k] != 0;
            }
            if (!any)
            {
                g_tints.erase(id);
            }
        }
    }
    g_tintCount.store(static_cast<int>(g_tints.size()));
    const int n = g_tintCount.load();
    ReleaseSRWLockExclusive(&g_lock);
    return n;
}

// Sacred Gear: forgets every tint (a savegame was loaded: the object ids mean other objects now).
extern "C" void __cdecl SacredBild_TintClear()
{
    AcquireSRWLockExclusive(&g_lock);
    g_tints.clear();
    g_tintCount.store(0);
    ReleaseSRWLockExclusive(&g_lock);
}

// Sacred Gear: Sacred Gear's function that gives the surface of a texture token (a Texture.pak image or a picture of its own).
extern "C" void __cdecl SacredBild_TextureResolver(IDirectDrawSurface7* (__cdecl* resolver)(IDirect3DDevice7*, unsigned))
{
    g_resolver.store(resolver);
}

// Sacred Gear: draws an object's mesh (`part` 1..8, 0 the whole model) with another texture; `token` 0 takes it away.
extern "C" int __cdecl SacredBild_TextureSet(unsigned id, unsigned part, unsigned token)
{
    AcquireSRWLockExclusive(&g_lock);
    if (part <= static_cast<unsigned>(kParts) && (token || g_tints.count(id)))
    {
        Tint& t = g_tints[id];
        if (part == 0)
        {
            t.texAll = token;
        }
        else
        {
            t.texPart[part - 1] = token;
        }
        bool any = t.all != 0 || t.texAll != 0;
        for (int i = 0; i < kParts; ++i)
        {
            any = any || t.part[i] != 0 || t.texPart[i] != 0;
        }
        if (!any)
        {
            g_tints.erase(id);
        }
    }
    g_tintCount.store(static_cast<int>(g_tints.size()));
    const int n = g_tintCount.load();
    ReleaseSRWLockExclusive(&g_lock);
    return n;
}
