#include "itemicon.h"
#include "engine/gear_addr.h"
#include "inventory.h"
#include "log.h"
#include "world/world.h"

#include <ddraw.h>
#include <d3d.h>

#include <algorithm>
#include <map>

namespace
{
    using namespace Gear;

    // An object: +0x10 type, +0x14 flags (bit 26: its model state exists). An item (cItemBase): +8 bit 7 resources
    // loaded, +0x40 / +0x44 the two dwords the model renderer takes (its Granny model instance), +0xB0 flags of which the
    // renderer gets 0xF00000.
    constexpr uintptr_t kType = 0x10, kFlags = 0x14, kResources = 0x08, kModel = 0x40, kItemFlags = 0xB0;
    constexpr uint32_t kModelStateFlag = 1u << 26, kResourcesLoaded = 0x80, kRendererFlagsMask = 0xF00000;
    constexpr uintptr_t kRenderFlagsWord = 0x04;
    constexpr float kCellUnits = 32.0f;

    // The model pairs of item types drawn without an item object: loaded through the model loader, kept while icons of the
    // type keep being asked for, given back to the renderer after kModelIdlePasses passes without one.
    struct ModelPair
    {
        uint32_t model0 = 0, model1 = 0;
        bool ok = false;
        uint32_t lastPass = 0;
    };
    std::map<uint32_t, ModelPair> g_models;
    uint32_t g_pass = 0;
    constexpr uint32_t kModelIdlePasses = 2400;     // a few seconds without a request
    constexpr uint32_t kMaxModels = 160;

    std::map<uint32_t, uint32_t> g_temporary;       // type -> the item made for this pass
    uint32_t g_madeThisPass = 0;
    constexpr uint32_t kMaxTemporary = 48;
    constexpr uint32_t kScratch = Inventory::kScratch;       // an inventory container the game does not use

    template <class T>
    T& at(uintptr_t base, uintptr_t offset)
    {
        return *reinterpret_cast<T*>(base + offset);
    }

    // The device's state the model renderer may change, read before and set back after (the renderer sets what it
    // needs through the device and the game's render flags, and does not put it back).
    struct Snapshot
    {
        static constexpr int kRenderStates = 64, kStageTypes = 32;
        // The render states the game's flags and the model renderer touch (the others are not supported by every backend).
        static constexpr int kStates[] = {7, 8, 9, 10, 14, 15, 16, 19, 20, 22, 23, 24, 25, 26, 27, 28, 29, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48};
        IDirect3DDevice7* device;
        bool rs[kRenderStates] = {};
        DWORD rsValue[kRenderStates] = {};
        bool tss[2][kStageTypes] = {};
        DWORD tssValue[2][kStageTypes] = {};
        IDirectDrawSurface7* texture[2] = {};
        D3DMATRIX matrix[3] = {};
        bool matrixOk[3] = {};
        D3DVIEWPORT7 viewport = {};
        bool viewportOk = false;
        void* flags = nullptr;
        uint32_t flagsWord = 0;

        explicit Snapshot(IDirect3DDevice7* d) : device(d)
        {
            for (int i : kStates)
            {
                rs[i] = SUCCEEDED(device->GetRenderState(static_cast<D3DRENDERSTATETYPE>(i), &rsValue[i]));
            }
            for (int s = 0; s < 2; ++s)
            {
                for (int t = 1; t < kStageTypes; ++t)
                {
                    tss[s][t] = SUCCEEDED(device->GetTextureStageState(s, static_cast<D3DTEXTURESTAGESTATETYPE>(t), &tssValue[s][t]));
                }
                device->GetTexture(s, &texture[s]);
            }
            const D3DTRANSFORMSTATETYPE types[3] = {D3DTRANSFORMSTATE_WORLD, D3DTRANSFORMSTATE_VIEW, D3DTRANSFORMSTATE_PROJECTION};
            for (int i = 0; i < 3; ++i)
            {
                matrixOk[i] = SUCCEEDED(device->GetTransform(types[i], &matrix[i]));
            }
            viewportOk = SUCCEEDED(device->GetViewport(&viewport));
            if (Addr::renderFlags_instance)
            {
                flags = Addr::renderFlags_instance();
                if (flags)
                {
                    flagsWord = at<uint32_t>(reinterpret_cast<uintptr_t>(flags), kRenderFlagsWord);
                }
            }
        }

        ~Snapshot()
        {
            // The game's render flags first: setting one applies its device states, the explicit values follow.
            if (flags && Addr::renderFlags_set)
            {
                const uint32_t now = at<uint32_t>(reinterpret_cast<uintptr_t>(flags), kRenderFlagsWord);
                for (uint32_t bit = 1; bit; bit <<= 1)
                {
                    if ((now ^ flagsWord) & bit)
                    {
                        Addr::renderFlags_set(flags, bit, (flagsWord & bit) ? 1 : 0);
                    }
                }
            }
            for (int s = 0; s < 2; ++s)
            {
                for (int t = 1; t < kStageTypes; ++t)
                {
                    if (tss[s][t])
                    {
                        device->SetTextureStageState(s, static_cast<D3DTEXTURESTAGESTATETYPE>(t), tssValue[s][t]);
                    }
                }
                device->SetTexture(s, texture[s]);
                if (texture[s])
                {
                    texture[s]->Release();
                }
            }
            for (int i : kStates)
            {
                if (rs[i])
                {
                    device->SetRenderState(static_cast<D3DRENDERSTATETYPE>(i), rsValue[i]);
                }
            }
            const D3DTRANSFORMSTATETYPE types[3] = {D3DTRANSFORMSTATE_WORLD, D3DTRANSFORMSTATE_VIEW, D3DTRANSFORMSTATE_PROJECTION};
            for (int i = 0; i < 3; ++i)
            {
                if (matrixOk[i])
                {
                    device->SetTransform(types[i], &matrix[i]);
                }
            }
            if (viewportOk)
            {
                device->SetViewport(&viewport);
            }
        }
    };

    // 1 loaded, 0 not loadable, -1 fault.
    int callLoadModels(uint32_t type, uint32_t* out)
    {
        __try
        {
            void* tm = Addr::g_pTypeManager ? *reinterpret_cast<void**>(Addr::g_pTypeManager.address) : nullptr;
            void* loader = Addr::modelLoader_instance ? Addr::modelLoader_instance() : nullptr;
            if (!tm || !loader || type == 0 || type >= 0x7E60)
            {
                return 0;
            }
            const char* name = Addr::cTypeManager_modelName(tm, type);
            void* description = Addr::cTypeManager_modelDescription(tm, type);
            if (!name || !name[0])
            {
                return 0;
            }
            out[0] = out[1] = 0;
            return (Addr::modelLoader_load(loader, name, out, description) & 0xFF) && out[0] ? 1 : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    bool callFreeModels(uint32_t m0, uint32_t m1)
    {
        __try
        {
            void* renderer = Addr::modelRenderer_instance ? Addr::modelRenderer_instance() : nullptr;
            if (renderer && Addr::modelRenderer_free)
            {
                Addr::modelRenderer_free(renderer, m0, m1);
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The model pair of an item type, loaded once; false if the type has none.
    bool modelsFor(uint32_t type, uint32_t& m0, uint32_t& m1)
    {
        auto found = g_models.find(type);
        if (found == g_models.end())
        {
            if (g_models.size() >= kMaxModels)
            {
                return false;
            }
            ModelPair pair;
            uint32_t out[2] = {};
            pair.ok = callLoadModels(type, out) > 0;
            pair.model0 = out[0];
            pair.model1 = out[1];
            if (!pair.ok)
            {
                LOG("ItemIcon: no model pair for item type {} through the loader: the temporary item path draws it", type);
            }
            found = g_models.emplace(type, pair).first;
        }
        found->second.lastPass = g_pass;
        m0 = found->second.model0;
        m1 = found->second.model1;
        return found->second.ok;
    }

    // The temporary item of `type` for this pass: made at the hero's feet, at once moved into a container nobody uses
    // (so it is never in the world for the world pass to see), drawn, taken out and destroyed in endPass(). Destroying
    // an item that is still in the world after its model state was made crashes the game a moment later.
    uintptr_t temporaryItem(uint32_t type)
    {
        const auto found = g_temporary.find(type);
        if (found != g_temporary.end())
        {
            return World::itemObject(found->second);
        }
        if (g_madeThisPass >= kMaxTemporary || !World::inventoryChangesAllowed())
        {
            return 0;
        }
        World::Object hero;
        if (!World::object(World::hero(), hero))
        {
            return 0;
        }
        const char* error = nullptr;
        // Local only, in a network game too: nothing about this item reaches the peers or the server.
        Inventory::LocalOnly localOnly;
        const uint32_t id = World::createObject(type, hero.position, false, error);
        ++g_madeThisPass;
        if (!id)
        {
            return 0;
        }
        // Out of the world at once (the world pass must never see it, and the sector lists forget it): into a container
        // nobody uses, as an item the player picks up goes into the backpack. It leaves the same way.
        uint32_t w = 1, h = 1, cx = 0, cy = 0;
        Inventory::itemSize(id, w, h);
        if (!Inventory::findSpace(kScratch, w, h, cx, cy) || !Inventory::put(kScratch, id, cx, cy, error))
        {
            Inventory::destroy(id, error);
            return 0;
        }
        g_temporary[type] = id;
        return World::itemObject(id);
    }
}

bool ItemIcon::available()
{
    return Addr::cItem_loadResources && Addr::modelRenderer_instance && Addr::modelRenderer_drawItem && Addr::renderFlags_instance &&
           Addr::renderFlags_set && Addr::cItemType_size;
}

bool ItemIcon::draw(void* device, const Request& request)
{
    if (!device || !available() || request.w <= 0 || request.h <= 0)
    {
        return false;
    }
    uintptr_t object = request.itemId ? World::itemObject(request.itemId) : 0;
    uint32_t type = request.type, model0 = 0, model1 = 0, rendererFlags = 0;
    bool haveModels = false;
    if (!object && request.type)
    {
        // An item that is not in the game (a stored one): its icon needs no item object, only the type's model pair.
        haveModels = modelsFor(request.type, model0, model1);
        if (!haveModels)
        {
            object = temporaryItem(request.type);     // not loadable that way: the old path
        }
    }
    if (!object && !haveModels)
    {
        return false;
    }
    if (object)
    {
        void* item = reinterpret_cast<void*>(object);
        type = at<uint32_t>(object, kType);
        // As the inventory does before it draws an item: its resources, then its model state.
        if (!(at<uint8_t>(object, kResources) & kResourcesLoaded))
        {
            Addr::cItem_loadResources(item);
        }
        if (!(at<uint32_t>(object, kFlags) & kModelStateFlag))
        {
            using ModelStateFn = void(__thiscall*)(void* self, int a);
            const void* const* vtable = *reinterpret_cast<const void* const* const*>(item);
            reinterpret_cast<ModelStateFn>(const_cast<void*>(vtable[0x28 / 4]))(item, 1);
        }
        model0 = at<uint32_t>(object, kModel);
        model1 = at<uint32_t>(object, kModel + 4);
        rendererFlags = at<uint32_t>(object, kItemFlags) & kRendererFlagsMask;
    }
    uint16_t cells[2] = {1, 1};
    Addr::cItemType_size(nullptr, type, &cells[0], &cells[1]);
    const float naturalW = std::max<uint16_t>(cells[0], 1) * kCellUnits, naturalH = std::max<uint16_t>(cells[1], 1) * kCellUnits;
    const float scale = std::min(request.w / naturalW, request.h / naturalH);
    const int cx = static_cast<int>(request.x + request.w * 0.5f + 0.5f), cy = static_cast<int>(request.y + request.h * 0.5f + 0.5f);

    Snapshot snapshot(static_cast<IDirect3DDevice7*>(device));
    void* flags = Addr::renderFlags_instance();
    Addr::renderFlags_set(flags, 1, 0);     // the inventory's first move: no Z test
    void* renderer = Addr::modelRenderer_instance();
    if (!renderer)
    {
        return false;
    }
    Addr::modelRenderer_drawItem(renderer, device, model0, model1, cx, cy, type, scale, 0, rendererFlags, 0, 0);
    return true;
}

void ItemIcon::endPass()
{
    Inventory::LocalOnly localOnly;
    for (const auto& [type, id] : g_temporary)
    {
        const char* error = nullptr;
        Inventory::remove(kScratch, id, error);
        Inventory::destroy(id, error);
    }
    g_temporary.clear();
    g_madeThisPass = 0;
    // Model pairs no icon has asked for lately go back to the renderer.
    ++g_pass;
    for (auto it = g_models.begin(); it != g_models.end();)
    {
        if (g_pass - it->second.lastPass > kModelIdlePasses)
        {
            if (it->second.ok)
            {
                callFreeModels(it->second.model0, it->second.model1);
            }
            it = g_models.erase(it);
        }
        else
        {
            ++it;
        }
    }
}
