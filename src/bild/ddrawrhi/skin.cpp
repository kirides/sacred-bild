// GpuSkin's buffers and draws on the NVRHI backend (skin.h).
#include "ddrawrhi/skin.h"
#include "ddrawrhi/context.h"
#include "ddrawrhi/device.h"
#include "log.h"

#include <algorithm>
#include <mutex>

namespace DDrawRhi::Skin
{
    struct Mesh
    {
        nvrhi::BufferHandle buffer;
        uint32_t count;
    };

    struct Indices
    {
        nvrhi::BufferHandle arena;
        uint32_t start;
        uint32_t count;
    };

    namespace
    {
        static_assert(sizeof(DDraw9::Skin::Vertex) == 52);
        constexpr uint32_t kArenaIndices = 1u << 20;    // 2 MB of indices per arena, as the Direct3D 9 backend's

        // The arena new pieces go into (Context lock held): appended to with copies, never over what draws use.
        nvrhi::BufferHandle g_arena;
        uint32_t g_arenaUsed = 0;
        uint32_t g_arenaCapacity = 0;
    }

    bool available(IDirect3DDevice7* device)
    {
        return Device::from(device) != nullptr;
    }

    Mesh* createMesh(IDirect3DDevice7* device, const DDraw9::Skin::Vertex* vertices, uint32_t count)
    {
        if (!Device::from(device) || !vertices || !count)
        {
            return nullptr;
        }
        std::scoped_lock lock(Context::lock());
        const uint64_t bytes = uint64_t(count) * sizeof(DDraw9::Skin::Vertex);
        nvrhi::BufferHandle buffer = Context::device()->createBuffer(nvrhi::BufferDesc().setByteSize(bytes).setIsVertexBuffer(true)
            .setDebugName("skinned mesh").setInitialState(nvrhi::ResourceStates::VertexBuffer).setKeepInitialState(true));
        if (!buffer)
        {
            LOG("NVRHI backend: skinning: a buffer for {} vertices could not be created", count);
            return nullptr;
        }
        Context::commands()->writeBuffer(buffer, vertices, static_cast<size_t>(bytes));
        return new Mesh{buffer, count};
    }

    void releaseMesh(Mesh* mesh)
    {
        if (mesh)
        {
            std::scoped_lock lock(Context::lock());
            delete mesh;
        }
    }

    Indices* createIndices(IDirect3DDevice7* device, const WORD* indices, uint32_t count)
    {
        if (!Device::from(device) || !indices || count < 3)
        {
            return nullptr;
        }
        std::scoped_lock lock(Context::lock());
        if (!g_arena || g_arenaUsed + count > g_arenaCapacity)
        {
            const uint32_t capacity = std::max(kArenaIndices, count);
            g_arena = Context::device()->createBuffer(nvrhi::BufferDesc().setByteSize(uint64_t(capacity) * sizeof(WORD))
                .setIsIndexBuffer(true).setDebugName("skinned pieces").setInitialState(nvrhi::ResourceStates::IndexBuffer)
                .setKeepInitialState(true));
            if (!g_arena)
            {
                LOG("NVRHI backend: skinning: an index arena could not be created");
                return nullptr;
            }
            g_arenaUsed = 0;
            g_arenaCapacity = capacity;
        }
        Context::commands()->writeBuffer(g_arena, indices, size_t(count) * sizeof(WORD), uint64_t(g_arenaUsed) * sizeof(WORD));
        auto* piece = new Indices{g_arena, g_arenaUsed, count};
        g_arenaUsed += count;
        return piece;
    }

    void releaseIndices(Indices* indices)
    {
        if (indices)
        {
            std::scoped_lock lock(Context::lock());
            delete indices;
        }
    }

    bool draw(IDirect3DDevice7* device, Mesh* mesh, Indices* indices, const DDraw9::Skin::Draw& d)
    {
        Device* dev = Device::from(device);
        return dev && mesh && indices && dev->drawSkinned(mesh->buffer, mesh->count, indices->arena, indices->start, indices->count, d);
    }
}
