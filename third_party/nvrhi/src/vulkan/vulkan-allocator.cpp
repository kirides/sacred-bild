/*
* Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

#include "vulkan-backend.h"

// sacred-bild: placed resources. Vulkan Memory Allocator's implementation, calling Vulkan through the pointers the
// device is made with (vulkan-1.dll is loaded at run time, never linked).
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

namespace nvrhi::vulkan
{

    static vk::MemoryPropertyFlags pickBufferMemoryProperties(const BufferDesc& d)
    {
        vk::MemoryPropertyFlags flags{};

        switch(d.cpuAccess)
        {
        case CpuAccessMode::None:
            flags = vk::MemoryPropertyFlagBits::eDeviceLocal;
            break;
        case CpuAccessMode::Read:
            flags = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCached;
            break;
        case CpuAccessMode::Write:
            flags = vk::MemoryPropertyFlagBits::eHostVisible;
            break;
        }

        return flags;
    }

    vk::Result VulkanAllocator::allocateBufferMemory(Buffer *buffer, bool enableDeviceAddress) const
    {
        // sacred-bild: placed resources: GPU-only buffers bound to a block of Vulkan Memory Allocator.
        if (m_Context.vma && buffer->desc.sharesMemory && buffer->desc.cpuAccess == CpuAccessMode::None && !enableDeviceAddress &&
            (buffer->desc.sharedResourceFlags & SharedResourceFlags::Shared) == 0)
        {
            VmaAllocationCreateInfo createInfo = {};
            createInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            VmaAllocationInfo info = {};
            VmaAllocation allocation = nullptr;
            VkResult vr = vmaAllocateMemoryForBuffer(m_Context.vma, buffer->buffer, &createInfo, &allocation, &info);
            if (vr == VK_SUCCESS)
            {
                vr = vmaBindBufferMemory(m_Context.vma, allocation, buffer->buffer);
                if (vr != VK_SUCCESS)
                {
                    vmaFreeMemory(m_Context.vma, allocation);
                    return vk::Result(vr);
                }
                buffer->managed = true;
                buffer->placed = allocation;
                buffer->memory = vk::DeviceMemory(info.deviceMemory);
                return vk::Result::eSuccess;
            }
        }

        // figure out memory requirements
        vk::MemoryRequirements memRequirements;
        m_Context.device.getBufferMemoryRequirements(buffer->buffer, &memRequirements);

        // allocate memory
        const bool enableMemoryExport = (buffer->desc.sharedResourceFlags & SharedResourceFlags::Shared) != 0;
        const vk::Result res = allocateMemory(buffer, memRequirements, pickBufferMemoryProperties(buffer->desc), enableDeviceAddress, enableMemoryExport, VK_NULL_HANDLE, buffer->buffer);   // sacred-bild: x86
        CHECK_VK_RETURN(res)

        m_Context.device.bindBufferMemory(buffer->buffer, buffer->memory, 0);

        return vk::Result::eSuccess;
    }

    void VulkanAllocator::freeBufferMemory(Buffer *buffer) const
    {
        if (buffer->placed)    // sacred-bild: placed resources
        {
            vmaFreeMemory(m_Context.vma, buffer->placed);
            buffer->placed = nullptr;
            buffer->memory = vk::DeviceMemory(nullptr);
            return;
        }
        freeMemory(buffer);
    }

    vk::Result VulkanAllocator::allocateTextureMemory(Texture *texture) const
    {
        // sacred-bild: placed resources: bound to a block of Vulkan Memory Allocator.
        if (m_Context.vma && texture->desc.sharesMemory && (texture->desc.sharedResourceFlags & SharedResourceFlags::Shared) == 0)
        {
            VmaAllocationCreateInfo createInfo = {};
            createInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            VmaAllocationInfo info = {};
            VmaAllocation allocation = nullptr;
            VkResult vr = vmaAllocateMemoryForImage(m_Context.vma, texture->image, &createInfo, &allocation, &info);
            if (vr == VK_SUCCESS)
            {
                vr = vmaBindImageMemory(m_Context.vma, allocation, texture->image);
                if (vr != VK_SUCCESS)
                {
                    vmaFreeMemory(m_Context.vma, allocation);
                    return vk::Result(vr);
                }
                texture->managed = true;
                texture->placed = allocation;
                texture->memory = vk::DeviceMemory(info.deviceMemory);
                return vk::Result::eSuccess;
            }
        }

        // grab the image memory requirements
        vk::MemoryRequirements memRequirements;
        m_Context.device.getImageMemoryRequirements(texture->image, &memRequirements);

        // allocate memory
        const vk::MemoryPropertyFlags memProperties = vk::MemoryPropertyFlagBits::eDeviceLocal;
        const bool enableDeviceAddress = false;
        const bool enableMemoryExport = (texture->desc.sharedResourceFlags & SharedResourceFlags::Shared) != 0;
        const vk::Result res = allocateMemory(texture, memRequirements, memProperties, enableDeviceAddress, enableMemoryExport, texture->image, VK_NULL_HANDLE);   // sacred-bild: x86
        CHECK_VK_RETURN(res)

        m_Context.device.bindImageMemory(texture->image, texture->memory, 0);

        return vk::Result::eSuccess;
    }

    void VulkanAllocator::freeTextureMemory(Texture *texture) const
    {
        if (texture->placed)    // sacred-bild: placed resources
        {
            vmaFreeMemory(m_Context.vma, texture->placed);
            texture->placed = nullptr;
            texture->memory = vk::DeviceMemory(nullptr);
            return;
        }

        freeMemory(texture);
    }

    vk::Result VulkanAllocator::allocateMemory(MemoryResource *res,
                                               vk::MemoryRequirements memRequirements,
                                               vk::MemoryPropertyFlags memPropertyFlags,
                                                bool enableDeviceAddress,
                                                bool enableExportMemory,
                                                VkImage dedicatedImage,
                                                VkBuffer dedicatedBuffer) const
    {
        res->managed = true;

        // find a memory space that satisfies the requirements
        vk::PhysicalDeviceMemoryProperties memProperties;
        m_Context.physicalDevice.getMemoryProperties(&memProperties);

        uint32_t memTypeIndex;
        for(memTypeIndex = 0; memTypeIndex < memProperties.memoryTypeCount; memTypeIndex++)
        {
            if ((memRequirements.memoryTypeBits & (1 << memTypeIndex)) &&
                ((memProperties.memoryTypes[memTypeIndex].propertyFlags & memPropertyFlags) == memPropertyFlags))
            {
                break;
            }
        }

        if (memTypeIndex == memProperties.memoryTypeCount)
        {
            // xxxnsubtil: this is incorrect; need better error reporting
            return vk::Result::eErrorOutOfDeviceMemory;
        }

        // allocate memory
        auto allocFlags = vk::MemoryAllocateFlagsInfo();
        if (enableDeviceAddress)
            allocFlags.flags |= vk::MemoryAllocateFlagBits::eDeviceAddress;
        const void* pNext = &allocFlags;

        auto dedicatedAllocation = vk::MemoryDedicatedAllocateInfo()
            .setImage(dedicatedImage)
            .setBuffer(dedicatedBuffer)
            .setPNext(pNext);

        if (dedicatedImage || dedicatedBuffer)
        {
            // Append the VkMemoryDedicatedAllocateInfo structure to the chain
            pNext = &dedicatedAllocation;
        }

#ifdef _WIN32
        const auto handleType = vk::ExternalMemoryHandleTypeFlagBits::eOpaqueWin32;
#else
        const auto handleType = vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd;
#endif
        auto exportInfo = vk::ExportMemoryAllocateInfo()
            .setHandleTypes(handleType)
            .setPNext(pNext);

        if(enableExportMemory)
        {
            // Append the VkExportMemoryAllocateInfo structure to the chain
            pNext = &exportInfo;
        }

        auto allocInfo = vk::MemoryAllocateInfo()
                            .setAllocationSize(memRequirements.size)
                            .setMemoryTypeIndex(memTypeIndex)
                            .setPNext(pNext);

        return m_Context.device.allocateMemory(&allocInfo, m_Context.allocationCallbacks, &res->memory);
    }

    // sacred-bild: placed resources
    void createPlacedAllocator(VulkanContext& context)
    {
        VmaVulkanFunctions functions = {};
        functions.vkGetInstanceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr;
        functions.vkGetDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;
        VmaAllocatorCreateInfo createInfo = {};
        createInfo.physicalDevice = context.physicalDevice;
        createInfo.device = context.device;
        createInfo.instance = context.instance;
        createInfo.pVulkanFunctions = &functions;
        createInfo.pAllocationCallbacks = reinterpret_cast<const VkAllocationCallbacks*>(context.allocationCallbacks);
        if (vmaCreateAllocator(&createInfo, &context.vma) != VK_SUCCESS)
            context.vma = nullptr;
    }

    void destroyPlacedAllocator(VulkanContext& context)
    {
        if (context.vma)
        {
            vmaDestroyAllocator(context.vma);
            context.vma = nullptr;
        }
    }

    void VulkanAllocator::freeMemory(MemoryResource *res) const
    {
        assert(res->managed);

        m_Context.device.freeMemory(res->memory, m_Context.allocationCallbacks);
        res->memory = vk::DeviceMemory(nullptr);
    }

} // namespace nvrhi::vulkan
