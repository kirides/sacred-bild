#include "rhi/backend.h"

#include "log.h"

#include <nvrhi/vulkan.h>
#include <vulkan/vulkan.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

// The one definition of Vulkan-Hpp's dynamic dispatcher, which NVRHI's Vulkan backend calls through (third_party/CMakeLists.txt).
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

// Vulkan 1.3 for Rhi: vulkan-1.dll loaded by hand (never linked), an instance with the Win32 surface, a GPU with a graphics
// queue that presents to the window and the features NVRHI needs (dynamic rendering, synchronization2, timeline semaphores),
// and a swap chain. In 32-bit builds the non-dispatchable handles are uint64_t (third_party/README.md).
namespace Rhi::Detail
{
    namespace
    {
        VKAPI_ATTR VkBool32 VKAPI_CALL debugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
            VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
        {
            if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT && data && data->pMessage)
            {
                static int logged = 0;
                if (logged++ < 200)
                {
                    LOG("RHI: Vulkan validation: {}", data->pMessage);
                }
            }
            return VK_FALSE;
        }

        bool hasExtension(const std::vector<vk::ExtensionProperties>& list, const char* name)
        {
            return std::any_of(list.begin(), list.end(), [&](const vk::ExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
        }

        class VulkanBackend final : public Backend
        {
        public:
            ~VulkanBackend() override
            {
                if (m_device)
                {
                    m_device.waitIdle();
                }
                destroySwapchain();
                m_nvrhi = nullptr;
                for (vk::Semaphore s : m_acquire)
                {
                    m_device.destroySemaphore(s);
                }
                if (m_device)
                {
                    m_device.destroy();
                }
                if (m_surface)
                {
                    m_instance.destroySurfaceKHR(m_surface);
                }
                if (m_messenger)
                {
                    m_instance.destroyDebugUtilsMessengerEXT(m_messenger);
                }
                if (m_instance)
                {
                    m_instance.destroy();
                }
                // vulkan-1.dll stays loaded: drivers may keep threads.
            }

            bool create(HWND window, uint32_t width, uint32_t height, const Settings& settings,
                nvrhi::IMessageCallback* messages) override
            {
                m_vsync = settings.vsync;
                HMODULE lib = LoadLibraryW(L"vulkan-1.dll");
                if (!lib)
                {
                    LOG("RHI: Vulkan: vulkan-1.dll missing (no Vulkan driver installed)");
                    return false;
                }
                const auto getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(lib, "vkGetInstanceProcAddr"));
                if (!getInstanceProcAddr)
                {
                    LOG("RHI: Vulkan: vkGetInstanceProcAddr missing");
                    return false;
                }
                VULKAN_HPP_DEFAULT_DISPATCHER.init(getInstanceProcAddr);
                try
                {
                    return createInstance(settings) && createSurface(window) && pickDevice() && createDevice(messages)
                        && createSwapchain(width, height);
                }
                catch (const vk::SystemError& e)
                {
                    LOG("RHI: Vulkan: {}", e.what());
                    return false;
                }
            }

            nvrhi::IDevice* device() override { return m_nvrhi; }
            const std::string& adapterName() const override { return m_adapterName; }
            uint32_t imageCount() const override { return static_cast<uint32_t>(m_images.size()); }
            nvrhi::ITexture* image(uint32_t index) override { return index < m_images.size() ? m_images[index].Get() : nullptr; }
            nvrhi::Format format() const override { return nvrhi::Format::BGRA8_UNORM; }

            bool acquire(uint32_t& index) override
            {
                if (!m_swapchain)
                {
                    return false;
                }
                m_acquireIndex = (m_acquireIndex + 1) % m_acquire.size();
                const vk::Semaphore semaphore = m_acquire[m_acquireIndex];
                const vk::Result res = m_device.acquireNextImageKHR(m_swapchain, UINT64_MAX, semaphore, vk::Fence(), &m_image);
                if (res != vk::Result::eSuccess && res != vk::Result::eSuboptimalKHR)
                {
                    logOnce("vkAcquireNextImageKHR", res);
                    return false;
                }
                // The frame's first command list waits for the image.
                vulkanDevice()->queueWaitForSemaphore(nvrhi::CommandQueue::Graphics, VkSemaphore(semaphore), 0);
                index = m_image;
                return true;
            }

            bool present() override
            {
                const vk::Semaphore done = m_presentDone[m_image];
                vulkanDevice()->queueSignalSemaphore(nvrhi::CommandQueue::Graphics, VkSemaphore(done), 0);
                // NVRHI submits waits and signals with the next command list: an empty one carries them.
                m_flush->open();
                m_flush->close();
                m_nvrhi->executeCommandList(m_flush);
                const vk::PresentInfoKHR info = vk::PresentInfoKHR().setWaitSemaphores(done).setSwapchains(m_swapchain).setImageIndices(m_image);
                const vk::Result res = m_queue.presentKHR(&info);
                if (res != vk::Result::eSuccess && res != vk::Result::eSuboptimalKHR)
                {
                    logOnce("vkQueuePresentKHR", res);
                    return false;
                }
                return true;
            }

            bool resize(uint32_t width, uint32_t height) override
            {
                m_device.waitIdle();
                destroySwapchain();
                try
                {
                    return createSwapchain(width, height);
                }
                catch (const vk::SystemError& e)
                {
                    LOG("RHI: Vulkan: {}", e.what());
                    return false;
                }
            }

        private:
            nvrhi::vulkan::IDevice* vulkanDevice() const { return static_cast<nvrhi::vulkan::IDevice*>(m_nvrhi.Get()); }

            bool createInstance(const Settings& settings)
            {
                const auto available = vk::enumerateInstanceExtensionProperties();
                m_instanceExtensions = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
                std::vector<const char*> layers;
                const bool debugUtils = settings.validation && hasExtension(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                if (debugUtils)
                {
                    m_instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                }
                if (settings.validation)
                {
                    const auto installed = vk::enumerateInstanceLayerProperties();
                    const bool found = std::any_of(installed.begin(), installed.end(),
                        [](const vk::LayerProperties& l) { return std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0; });
                    if (found)
                    {
                        layers.push_back("VK_LAYER_KHRONOS_validation");
                    }
                    else
                    {
                        LOG("RHI: Vulkan: VK_LAYER_KHRONOS_validation not installed (Vulkan SDK)");
                    }
                }
                const vk::ApplicationInfo app = vk::ApplicationInfo().setPApplicationName("Sacred").setPEngineName("SacredBild")
                    .setApiVersion(VK_API_VERSION_1_3);
                m_instance = vk::createInstance(vk::InstanceCreateInfo().setPApplicationInfo(&app)
                    .setPEnabledExtensionNames(m_instanceExtensions).setPEnabledLayerNames(layers));
                VULKAN_HPP_DEFAULT_DISPATCHER.init(m_instance);
                if (debugUtils)
                {
                    m_messenger = m_instance.createDebugUtilsMessengerEXT(vk::DebugUtilsMessengerCreateInfoEXT()
                        .setMessageSeverity(vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning | vk::DebugUtilsMessageSeverityFlagBitsEXT::eError)
                        .setMessageType(vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral | vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation
                            | vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance)
                        .setPfnUserCallback(reinterpret_cast<vk::PFN_DebugUtilsMessengerCallbackEXT>(&debugMessage)));
                }
                return true;
            }

            bool createSurface(HWND window)
            {
                m_surface = m_instance.createWin32SurfaceKHR(vk::Win32SurfaceCreateInfoKHR()
                    .setHinstance(GetModuleHandleW(nullptr)).setHwnd(window));
                return true;
            }

            bool pickDevice()
            {
                int best = -1;
                for (const vk::PhysicalDevice& gpu : m_instance.enumeratePhysicalDevices())
                {
                    const vk::PhysicalDeviceProperties props = gpu.getProperties();
                    if (props.apiVersion < VK_API_VERSION_1_3 || !hasExtension(gpu.enumerateDeviceExtensionProperties(), VK_KHR_SWAPCHAIN_EXTENSION_NAME))
                    {
                        continue;
                    }
                    vk::PhysicalDeviceVulkan13Features f13;
                    vk::PhysicalDeviceVulkan12Features f12;
                    f12.pNext = &f13;
                    vk::PhysicalDeviceFeatures2 f2;
                    f2.pNext = &f12;
                    gpu.getFeatures2(&f2);
                    if (!f13.dynamicRendering || !f13.synchronization2 || !f12.timelineSemaphore)
                    {
                        continue;
                    }
                    const auto families = gpu.getQueueFamilyProperties();
                    int family = -1;
                    for (uint32_t i = 0; i < families.size(); ++i)
                    {
                        if ((families[i].queueFlags & vk::QueueFlagBits::eGraphics) && gpu.getSurfaceSupportKHR(i, m_surface))
                        {
                            family = static_cast<int>(i);
                            break;
                        }
                    }
                    if (family < 0)
                    {
                        continue;
                    }
                    const int score = props.deviceType == vk::PhysicalDeviceType::eDiscreteGpu ? 3
                        : props.deviceType == vk::PhysicalDeviceType::eIntegratedGpu ? 2 : 1;
                    if (score > best)
                    {
                        best = score;
                        m_gpu = gpu;
                        m_family = family;
                        m_adapterName = props.deviceName.data();
                    }
                }
                if (best < 0)
                {
                    LOG("RHI: Vulkan: no GPU with Vulkan 1.3 (dynamic rendering, synchronization2, timeline semaphores) that presents to the window");
                    return false;
                }
                const vk::PhysicalDeviceProperties props = m_gpu.getProperties();
                LOG("RHI: Vulkan: adapter {} (Vulkan {}.{}.{}, driver {:08x})", m_adapterName, VK_API_VERSION_MAJOR(props.apiVersion),
                    VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion);
                return true;
            }

            bool createDevice(nvrhi::IMessageCallback* messages)
            {
                m_deviceExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
                vk::PhysicalDeviceVulkan13Features f13 = vk::PhysicalDeviceVulkan13Features().setDynamicRendering(true).setSynchronization2(true);
                vk::PhysicalDeviceVulkan12Features f12 = vk::PhysicalDeviceVulkan12Features().setTimelineSemaphore(true).setPNext(&f13);
                vk::PhysicalDeviceFeatures2 f2 = vk::PhysicalDeviceFeatures2().setPNext(&f12);
                const float priority = 1.0f;
                const vk::DeviceQueueCreateInfo queue = vk::DeviceQueueCreateInfo().setQueueFamilyIndex(static_cast<uint32_t>(m_family))
                    .setQueuePriorities(priority);
                m_device = m_gpu.createDevice(vk::DeviceCreateInfo().setPNext(&f2).setQueueCreateInfos(queue)
                    .setPEnabledExtensionNames(m_deviceExtensions));
                VULKAN_HPP_DEFAULT_DISPATCHER.init(m_device);
                m_queue = m_device.getQueue(static_cast<uint32_t>(m_family), 0);

                nvrhi::vulkan::DeviceDesc desc;
                desc.errorCB = messages;
                desc.instance = m_instance;
                desc.physicalDevice = m_gpu;
                desc.device = m_device;
                desc.graphicsQueue = m_queue;
                desc.graphicsQueueIndex = m_family;
                desc.instanceExtensions = m_instanceExtensions.data();
                desc.numInstanceExtensions = m_instanceExtensions.size();
                desc.deviceExtensions = m_deviceExtensions.data();
                desc.numDeviceExtensions = m_deviceExtensions.size();
                m_nvrhi = nvrhi::vulkan::createDevice(desc);
                if (!m_nvrhi)
                {
                    LOG("RHI: Vulkan: NVRHI device creation failed");
                    return false;
                }
                m_flush = m_nvrhi->createCommandList();
                return true;
            }

            bool createSwapchain(uint32_t width, uint32_t height)
            {
                const vk::SurfaceCapabilitiesKHR caps = m_gpu.getSurfaceCapabilitiesKHR(m_surface);
                const auto formats = m_gpu.getSurfaceFormatsKHR(m_surface);
                const bool hasBgra = std::any_of(formats.begin(), formats.end(), [](const vk::SurfaceFormatKHR& f) {
                    return f.format == vk::Format::eB8G8R8A8Unorm && f.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear; });
                if (!hasBgra)
                {
                    LOG("RHI: Vulkan: the surface has no B8G8R8A8_UNORM format");
                    return false;
                }
                const auto modes = m_gpu.getSurfacePresentModesKHR(m_surface);
                auto has = [&](vk::PresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
                // VSync off: tearing (immediate) where the driver offers it, else mailbox (no tearing, newest frame wins).
                m_mode = m_vsync ? vk::PresentModeKHR::eFifo
                    : has(vk::PresentModeKHR::eImmediate) ? vk::PresentModeKHR::eImmediate
                    : has(vk::PresentModeKHR::eMailbox) ? vk::PresentModeKHR::eMailbox : vk::PresentModeKHR::eFifo;
                uint32_t count = std::max(3u, caps.minImageCount);
                if (caps.maxImageCount)
                {
                    count = std::min(count, caps.maxImageCount);
                }
                vk::Extent2D extent(width, height);
                if (caps.currentExtent.width != UINT32_MAX)
                {
                    extent = caps.currentExtent;
                }
                m_swapchain = m_device.createSwapchainKHR(vk::SwapchainCreateInfoKHR()
                    .setSurface(m_surface).setMinImageCount(count).setImageFormat(vk::Format::eB8G8R8A8Unorm)
                    .setImageColorSpace(vk::ColorSpaceKHR::eSrgbNonlinear).setImageExtent(extent).setImageArrayLayers(1)
                    .setImageUsage(vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst)
                    .setImageSharingMode(vk::SharingMode::eExclusive).setPreTransform(caps.currentTransform)
                    .setCompositeAlpha(vk::CompositeAlphaFlagBitsKHR::eOpaque).setPresentMode(m_mode).setClipped(true));
                const auto images = m_device.getSwapchainImagesKHR(m_swapchain);
                for (const vk::Image& img : images)
                {
                    nvrhi::TextureDesc desc;
                    desc.width = extent.width;
                    desc.height = extent.height;
                    desc.format = nvrhi::Format::BGRA8_UNORM;
                    desc.debugName = "swap chain image";
                    desc.isRenderTarget = true;
                    desc.initialState = nvrhi::ResourceStates::Present;
                    desc.keepInitialState = true;
                    m_images.push_back(m_nvrhi->createHandleForNativeTexture(nvrhi::ObjectTypes::VK_Image,
                        nvrhi::Object(static_cast<uint64_t>(VkImage(img))), desc));
                    m_presentDone.push_back(m_device.createSemaphore(vk::SemaphoreCreateInfo()));
                }
                while (m_acquire.size() < images.size() + 1)
                {
                    m_acquire.push_back(m_device.createSemaphore(vk::SemaphoreCreateInfo()));
                }
                return true;
            }

            void destroySwapchain()
            {
                m_images.clear();
                if (m_device)
                {
                    for (vk::Semaphore s : m_presentDone)
                    {
                        m_device.destroySemaphore(s);
                    }
                }
                m_presentDone.clear();
                if (m_swapchain)
                {
                    m_device.destroySwapchainKHR(m_swapchain);
                    m_swapchain = nullptr;
                }
            }

            void logOnce(const char* call, vk::Result res)
            {
                if (!m_failed)
                {
                    m_failed = true;
                    LOG("RHI: Vulkan: {} failed ({})", call, vk::to_string(res));
                }
            }

            vk::Instance m_instance;
            vk::DebugUtilsMessengerEXT m_messenger;
            vk::SurfaceKHR m_surface;
            vk::PhysicalDevice m_gpu;
            int m_family = -1;
            vk::Device m_device;
            vk::Queue m_queue;
            vk::SwapchainKHR m_swapchain;
            vk::PresentModeKHR m_mode = vk::PresentModeKHR::eFifo;
            std::vector<nvrhi::TextureHandle> m_images;
            std::vector<vk::Semaphore> m_presentDone;     // per image: rendering done, presentation may start
            std::vector<vk::Semaphore> m_acquire;         // ring: image acquired, rendering may start
            size_t m_acquireIndex = 0;
            uint32_t m_image = 0;
            std::vector<const char*> m_instanceExtensions;
            std::vector<const char*> m_deviceExtensions;
            nvrhi::DeviceHandle m_nvrhi;
            nvrhi::CommandListHandle m_flush;
            std::string m_adapterName;
            bool m_vsync = true;
            bool m_failed = false;
        };
    }

    std::unique_ptr<Backend> makeVulkan()
    {
        return std::make_unique<VulkanBackend>();
    }
}
