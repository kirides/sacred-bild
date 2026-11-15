#include "itemicon.h"
#include "game/gear_addr.h"
#include "inventory.h"
#include "world.h"

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

    std::map<uint32_t, uint32_t> g_temporary;       // type -> the item made for this pass
    uint32_t g_madeThisPass = 0;
    constexpr uint32_t kMaxTemporary = 48;
    constexpr uint32_t kScratch = 31;       // an inventory container the game does not use

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
        if (g_madeThisPass >= kMaxTemporary || !World::changesAllowed())
        {
            return 0;
        }
        World::Object hero;
        if (!World::object(World::hero(), hero))
        {
            return 0;
        }
        const char* error = nullptr;
        const uint32_t id = World::spawn(type, hero.position, error);
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
    if (!object && request.type)
    {
        object = temporaryItem(request.type);
    }
    if (!object)
    {
        return false;
    }
    void* item = reinterpret_cast<void*>(object);
    const uint32_t type = at<uint32_t>(object, kType);
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
    Addr::modelRenderer_drawItem(renderer, device, at<uint32_t>(object, kModel), at<uint32_t>(object, kModel + 4), cx, cy, type,
                                 scale, 0, at<uint32_t>(object, kItemFlags) & kRendererFlagsMask, 0, 0);
    return true;
}

void ItemIcon::endPass()
{
    for (const auto& [type, id] : g_temporary)
    {
        const char* error = nullptr;
        Inventory::remove(kScratch, id, error);
        Inventory::destroy(id, error);
    }
    g_temporary.clear();
    g_madeThisPass = 0;
}
