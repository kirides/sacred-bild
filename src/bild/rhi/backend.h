#pragma once
#include "rhi/rhi.h"

#include <memory>
#include <vector>

// What rhi.cpp needs from each API: the device and the swap chain. rhi_d3d12.cpp and rhi_vulkan.cpp implement it.
namespace Rhi::Detail
{
    class Backend
    {
    public:
        virtual ~Backend() = default;

        // Device and queue; false with the reason logged. `messages` receives NVRHI's messages.
        virtual bool createDevice(const Settings& settings, nvrhi::IMessageCallback* messages) = 0;
        // The swap chain on `window` (once).
        virtual bool attachWindow(HWND window, uint32_t width, uint32_t height) = 0;
        virtual nvrhi::IDevice* device() = 0;
        virtual const std::string& adapterName() const = 0;

        // The swap chain's images, wrapped for NVRHI; recreated by resize.
        virtual uint32_t imageCount() const = 0;
        virtual nvrhi::ITexture* image(uint32_t index) = 0;
        virtual nvrhi::Format format() const = 0;

        // Before the frame's first draw: waits for the frame latency and acquires the next image (its index in `index`).
        virtual bool acquire(uint32_t& index) = 0;
        // After the frame's last command list.
        virtual bool present() = 0;
        // Resizes the swap chain; the caller made sure the GPU is idle and released its framebuffers.
        virtual bool resize(uint32_t width, uint32_t height) = 0;
    };

    std::unique_ptr<Backend> makeD3D12();
    std::unique_ptr<Backend> makeVulkan();
}
