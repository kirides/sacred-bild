#pragma once
#include <cstdint>
#include <vector>

// The NVRHI backend's pipelines kept between runs ([Render] PipelineCache): Direct3D 12's pipeline library and Vulkan's
// VkPipelineCache, saved next to the game as SacredBild-pipelines-<api>.bin.
//
// Stale data never reaches a draw: the file carries this ddraw.dll's build (any rebuild, and with it any shader change,
// discards it), the GPU and its driver version, and a hash of its contents (a torn or damaged file is discarded); the
// driver checks its own data again (Vulkan's cache header, Direct3D 12's library refuses another driver or adapter);
// and a pipeline the cache can't give is made as without it and stored. The file is written to a temporary one and
// renamed over the old, in the background.
namespace Rhi::PipelineCache
{
    // Who the data is for: the API, the GPU and its driver.
    struct Identity
    {
        uint32_t api = 0;           // Rhi::Api
        uint32_t vendor = 0, device = 0;
        uint64_t driver = 0;        // the driver's version
        uint64_t extra = 0;         // anything else that must match (Vulkan: the pipelineCacheUUID, hashed)
    };

    // The saved data for `id`, empty if there is none or it doesn't fit (then the file is deleted, logged).
    std::vector<uint8_t> load(const Identity& id);
    // Saves `data` for `id` (a copy is written on a background thread).
    void save(const Identity& id, std::vector<uint8_t> data);

    uint64_t hash(const void* data, size_t size);
}
