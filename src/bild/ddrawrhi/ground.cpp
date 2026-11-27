// GroundMesh's buffers and draws on the NVRHI backend (ground.h).
#include "ddrawrhi/ground.h"
#include "ddrawrhi/context.h"
#include "ddrawrhi/device.h"
#include "log.h"

#include <mutex>

namespace DDrawRhi::Ground
{
    struct Mesh
    {
        nvrhi::BufferHandle buffer;
        uint32_t capacity;      // quads
    };

    namespace
    {
        constexpr uint32_t kQuadBytes = 4 * sizeof(DDraw9::Ground::Vertex);
        static_assert(sizeof(DDraw9::Ground::Vertex) == 28);
    }

    bool available(IDirect3DDevice7* device)
    {
        return Device::from(device) != nullptr;
    }

    Mesh* createMesh(IDirect3DDevice7* device, uint32_t capacity)
    {
        if (!Device::from(device) || !capacity)
        {
            return nullptr;
        }
        std::scoped_lock lock(Context::lock());
        nvrhi::BufferHandle buffer = Context::device()->createBuffer(nvrhi::BufferDesc().setByteSize(uint64_t(capacity) * kQuadBytes)
            .setIsVertexBuffer(true).setDebugName("ground quads").setInitialState(nvrhi::ResourceStates::VertexBuffer)
            .setKeepInitialState(true).setSharesMemory(true));     // only written ranges are drawn
        if (!buffer)
        {
            LOG("NVRHI backend: ground: a buffer for {} quads could not be created", capacity);
            return nullptr;
        }
        return new Mesh{buffer, capacity};
    }

    void releaseMesh(Mesh* mesh)
    {
        if (mesh)
        {
            std::scoped_lock lock(Context::lock());
            delete mesh;        // NVRHI keeps the buffer until the GPU is done with it
        }
    }

    bool write(IDirect3DDevice7* device, Mesh* mesh, uint32_t first, const DDraw9::Ground::Vertex* vertices, uint32_t count)
    {
        if (!Device::from(device) || !mesh || first + count > mesh->capacity)
        {
            return false;
        }
        if (!count)
        {
            return true;
        }
        std::scoped_lock lock(Context::lock());
        Context::commands()->writeBuffer(mesh->buffer, vertices, size_t(count) * kQuadBytes, uint64_t(first) * kQuadBytes);
        return true;
    }

    bool draw(IDirect3DDevice7* device, Mesh* mesh, const DDraw9::Ground::Draw& d)
    {
        Device* dev = Device::from(device);
        return dev && mesh && dev->drawGround(mesh->buffer, d.firstQuad, d.quads, d.scale, d.offsetX, d.offsetY, d.z);
    }
}
