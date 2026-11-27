#include "rhi/rhi.h"
#include "rhi/backend.h"

#include "log.h"

#include <nvrhi/validation.h>

#include <algorithm>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace Rhi
{
    namespace
    {
        // NVRHI's messages into SacredBild.log; repeated texts once (a broken state reports every frame).
        class MessageLog final : public nvrhi::IMessageCallback
        {
        public:
            void message(nvrhi::MessageSeverity severity, const char* text) override
            {
                std::scoped_lock lock(m_mutex);
                if (m_seen.size() < 512 && !m_seen.insert(text).second)
                {
                    return;
                }
                static constexpr const char* kNames[] = {"info", "warning", "error", "fatal"};
                LOG("RHI {}: {}", kNames[std::clamp(static_cast<int>(severity), 0, 3)], text);
            }

        private:
            std::mutex m_mutex;
            std::set<std::string> m_seen;
        };

        MessageLog g_messages;
        std::unique_ptr<Detail::Backend> g_backend;
        nvrhi::DeviceHandle g_device;           // the backend's, or NVRHI's validation layer over it
        Settings g_settings;
        uint32_t g_width = 0, g_height = 0;
        uint32_t g_image = 0;
        bool g_inFrame = false;
        uint64_t g_frames = 0;
        std::vector<nvrhi::FramebufferHandle> g_framebuffers;
        // One query per frame in flight, signalled after its last command list: beginFrame waits on the oldest.
        std::deque<nvrhi::EventQueryHandle> g_inFlight;
        std::vector<nvrhi::EventQueryHandle> g_freeQueries;

        uint32_t framesInFlight()
        {
            return static_cast<uint32_t>(std::clamp(g_settings.maxFrameLatency, 1, 3)) + 1;
        }

        void makeFramebuffers()
        {
            g_framebuffers.clear();
            for (uint32_t i = 0; i < g_backend->imageCount(); ++i)
            {
                g_framebuffers.push_back(g_device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(g_backend->image(i))));
            }
        }

        void waitIdle()
        {
            if (g_device)
            {
                g_device->waitForIdle();
                g_inFlight.clear();
                g_device->runGarbageCollection();
            }
        }
    }

    const char* apiName(Api api)
    {
        return api == Api::Vulkan ? "Vulkan" : "Direct3D 12";
    }

    bool init(HWND window, uint32_t width, uint32_t height, const Settings& settings)
    {
        shutdown();
        g_settings = settings;
        g_backend = settings.api == Api::Vulkan ? Detail::makeVulkan() : Detail::makeD3D12();
        if (!g_backend || !g_backend->create(window, width, height, settings, &g_messages))
        {
            g_backend.reset();
            LOG("RHI: {} not available", apiName(settings.api));
            return false;
        }
        g_device = g_backend->device();
        if (settings.validation)
        {
            g_device = nvrhi::validation::createValidationLayer(g_device);
        }
        g_width = width;
        g_height = height;
        makeFramebuffers();
        LOG("RHI: {} on {}, swap chain {}x{} ({} images), vsync {}, frame latency {}{}", apiName(settings.api),
            g_backend->adapterName(), width, height, g_backend->imageCount(), settings.vsync ? "on" : "off",
            settings.maxFrameLatency, settings.validation ? ", validation" : "");
        return true;
    }

    void shutdown()
    {
        if (!g_backend)
        {
            return;
        }
        waitIdle();
        g_freeQueries.clear();
        g_framebuffers.clear();
        g_device = nullptr;
        g_backend.reset();
        g_inFrame = false;
        g_frames = 0;
    }

    bool ready()
    {
        return g_backend != nullptr;
    }

    Api api()
    {
        return g_settings.api;
    }

    nvrhi::IDevice* device()
    {
        return g_device;
    }

    const std::string& adapterName()
    {
        static const std::string none;
        return g_backend ? g_backend->adapterName() : none;
    }

    bool beginFrame()
    {
        if (!g_backend || g_width == 0 || g_height == 0)
        {
            return false;
        }
        while (g_inFlight.size() >= framesInFlight())
        {
            nvrhi::EventQueryHandle oldest = std::move(g_inFlight.front());
            g_inFlight.pop_front();
            g_device->waitEventQuery(oldest);
            g_freeQueries.push_back(std::move(oldest));
        }
        if (!g_backend->acquire(g_image))
        {
            return false;
        }
        g_inFrame = true;
        return true;
    }

    nvrhi::ITexture* backBuffer()
    {
        return g_backend && g_inFrame ? g_backend->image(g_image) : nullptr;
    }

    nvrhi::IFramebuffer* backBufferFramebuffer()
    {
        return g_backend && g_inFrame && g_image < g_framebuffers.size() ? g_framebuffers[g_image].Get() : nullptr;
    }

    uint32_t width()
    {
        return g_width;
    }

    uint32_t height()
    {
        return g_height;
    }

    nvrhi::Format backBufferFormat()
    {
        return g_backend ? g_backend->format() : nvrhi::Format::UNKNOWN;
    }

    void present()
    {
        if (!g_backend || !g_inFrame)
        {
            return;
        }
        g_inFrame = false;
        g_backend->present();
        nvrhi::EventQueryHandle query;
        if (!g_freeQueries.empty())
        {
            query = std::move(g_freeQueries.back());
            g_freeQueries.pop_back();
            g_device->resetEventQuery(query);
        }
        else
        {
            query = g_device->createEventQuery();
        }
        g_device->setEventQuery(query, nvrhi::CommandQueue::Graphics);
        g_inFlight.push_back(std::move(query));
        g_device->runGarbageCollection();
        ++g_frames;
    }

    void resize(uint32_t width, uint32_t height)
    {
        if (!g_backend || (width == g_width && height == g_height))
        {
            return;
        }
        waitIdle();
        g_framebuffers.clear();
        if (width && height && g_backend->resize(width, height))
        {
            makeFramebuffers();
        }
        LOG("RHI: swap chain {}x{} -> {}x{}", g_width, g_height, width, height);
        g_width = width;
        g_height = height;
    }

    uint64_t frameIndex()
    {
        return g_frames;
    }

    nvrhi::ShaderHandle createShader(nvrhi::ShaderType type, const char* entry, const void* dxil, size_t dxilSize,
        const void* spirv, size_t spirvSize)
    {
        if (!g_device)
        {
            return nullptr;
        }
        const bool vulkan = g_settings.api == Api::Vulkan;
        nvrhi::ShaderDesc desc;
        desc.shaderType = type;
        desc.entryName = entry;
        desc.debugName = entry;
        nvrhi::ShaderHandle shader = g_device->createShader(desc, vulkan ? spirv : dxil, vulkan ? spirvSize : dxilSize);
        if (!shader)
        {
            LOG("RHI: shader {} could not be created", entry);
        }
        return shader;
    }
}
