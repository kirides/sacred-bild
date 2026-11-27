#pragma once
#include <windows.h>

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <string>

// The NVRHI device behind [DDraw] Backend=nvrhi (docs/NvrhiRenderer.md): Direct3D 12 or Vulkan, the swap chain on the game
// window and the frame's pacing. One per process. Everything that draws for the new renderer gets its device here and
// records into command lists of its own; the frame is the span between beginFrame and present.
namespace Rhi
{
    enum class Api
    {
        D3D12,
        Vulkan,
    };
    const char* apiName(Api api);

    struct Settings
    {
        Api api = Api::D3D12;
        // The API's own validation (D3D12 debug layer, VK_LAYER_KHRONOS_validation) and NVRHI's validation layer; slow.
        bool validation = false;
        bool vsync = true;
        // Frames the CPU may queue ahead of the GPU (1 = lowest input latency).
        int maxFrameLatency = 1;
    };

    // Creates the device and a swap chain of width x height for `window`; false (and the reason in the log) if the API
    // or a suitable GPU is not there. Nothing is left behind on failure, so another API (or backend) can be tried.
    bool init(HWND window, uint32_t width, uint32_t height, const Settings& settings);
    void shutdown();
    bool ready();

    Api api();
    nvrhi::IDevice* device();
    // The adapter's name as the driver reports it.
    const std::string& adapterName();

    // Waits until the CPU may start the next frame (frame latency, the GPU's frames in flight) and acquires the back
    // buffer; false if there is nothing to draw into (minimized window, lost device).
    bool beginFrame();
    // The back buffer of the frame begun, as a texture and as a framebuffer with that one color target.
    nvrhi::ITexture* backBuffer();
    nvrhi::IFramebuffer* backBufferFramebuffer();
    uint32_t width();
    uint32_t height();
    nvrhi::Format backBufferFormat();
    // Presents the back buffer; every command list of the frame must have been executed.
    void present();

    // A new swap chain size (the window's client size); waits for the GPU.
    void resize(uint32_t width, uint32_t height);

    // Frames presented since init.
    uint64_t frameIndex();

    // A shader from the bytecode tools/re/gen_rhi_shaders.py generates (the DXIL or the SPIR-V array, by the API in use).
    nvrhi::ShaderHandle createShader(nvrhi::ShaderType type, const char* entry, const void* dxil, size_t dxilSize,
        const void* spirv, size_t spirvSize);
    template <size_t D, size_t S>
    nvrhi::ShaderHandle createShader(nvrhi::ShaderType type, const char* entry, const unsigned char (&dxil)[D],
        const unsigned char (&spirv)[S])
    {
        return createShader(type, entry, dxil, D, spirv, S);
    }
}
