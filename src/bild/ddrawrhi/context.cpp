#include "ddrawrhi/context.h"
#include "ddraw9/format.h"
#include "rhi/rhi.h"
#include "settings/config/ddraw.h"
#include "settings/config/debug.h"
#include "settings/config/display.h"
#include "log.h"

#include "ddrawrhi/shaders/blit_shaders.inc"
#include "ddrawrhi/shaders/ui_shaders.inc"

#include <nvrhi/utils.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace DDrawRhi
{
    namespace Format = DDraw9::Format;

    void unsupported(const char* what)
    {
        static std::mutex mutex;
        static std::set<std::string> seen;
        std::scoped_lock lock(mutex);
        if (seen.size() < 256 && seen.insert(what).second)
        {
            LOG("NVRHI backend: not supported: {}", what);
        }
    }

    nvrhi::Format gpuFormat(d9::D3DFORMAT format)
    {
        switch (format)
        {
        case d9::D3DFMT_DXT1: return nvrhi::Format::BC1_UNORM;
        case d9::D3DFMT_DXT2: case d9::D3DFMT_DXT3: return nvrhi::Format::BC2_UNORM;
        case d9::D3DFMT_DXT4: case d9::D3DFMT_DXT5: return nvrhi::Format::BC3_UNORM;
        case d9::D3DFMT_D16: return nvrhi::Format::D16;
        case d9::D3DFMT_D24X8: case d9::D3DFMT_D24S8: return nvrhi::Format::D24S8;
        case d9::D3DFMT_D32: return nvrhi::Format::D32;
        case d9::D3DFMT_UNKNOWN: return nvrhi::Format::UNKNOWN;
        default:
            return Format::Rgb(Format::toPixelFormat(format)).valid() ? nvrhi::Format::BGRA8_UNORM : nvrhi::Format::UNKNOWN;
        }
    }

    bool isDepth(d9::D3DFORMAT format)
    {
        return format == d9::D3DFMT_D16 || format == d9::D3DFMT_D24X8 || format == d9::D3DFMT_D24S8 || format == d9::D3DFMT_D32;
    }

    bool toBgra8(d9::D3DFORMAT format, const DDPIXELFORMAT& pf, const uint8_t* src, int srcPitch, uint32_t width,
        uint32_t height, uint8_t* dst, uint32_t dstPitch)
    {
        // The formats the game's textures use, without going through Format::Rgb per channel.
        static const auto expand4 = [] { std::array<uint8_t, 16> t{}; for (int i = 0; i < 16; ++i) t[i] = uint8_t(i * 17); return t; }();
        static const auto expand5 = [] { std::array<uint8_t, 32> t{}; for (int i = 0; i < 32; ++i) t[i] = uint8_t((i * 255 + 15) / 31); return t; }();
        static const auto expand6 = [] { std::array<uint8_t, 64> t{}; for (int i = 0; i < 64; ++i) t[i] = uint8_t((i * 255 + 31) / 63); return t; }();
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t* in = src + ptrdiff_t(srcPitch) * y;
            uint32_t* out = reinterpret_cast<uint32_t*>(dst + size_t(dstPitch) * y);
            switch (format)
            {
            case d9::D3DFMT_A8R8G8B8:
                std::memcpy(out, in, size_t(width) * 4);
                break;
            case d9::D3DFMT_X8R8G8B8:
                for (uint32_t x = 0; x < width; ++x)
                    out[x] = reinterpret_cast<const uint32_t*>(in)[x] | 0xFF000000u;
                break;
            case d9::D3DFMT_A4R4G4B4:
                for (uint32_t x = 0; x < width; ++x)
                {
                    const uint16_t p = reinterpret_cast<const uint16_t*>(in)[x];
                    out[x] = uint32_t(expand4[p >> 12]) << 24 | uint32_t(expand4[(p >> 8) & 15]) << 16 |
                        uint32_t(expand4[(p >> 4) & 15]) << 8 | expand4[p & 15];
                }
                break;
            case d9::D3DFMT_R5G6B5:
                for (uint32_t x = 0; x < width; ++x)
                {
                    const uint16_t p = reinterpret_cast<const uint16_t*>(in)[x];
                    out[x] = 0xFF000000u | uint32_t(expand5[p >> 11]) << 16 | uint32_t(expand6[(p >> 5) & 63]) << 8 | expand5[p & 31];
                }
                break;
            case d9::D3DFMT_X1R5G5B5:
            case d9::D3DFMT_A1R5G5B5:
            {
                const bool alpha = format == d9::D3DFMT_A1R5G5B5;
                for (uint32_t x = 0; x < width; ++x)
                {
                    const uint16_t p = reinterpret_cast<const uint16_t*>(in)[x];
                    const uint32_t a = !alpha || (p & 0x8000) ? 0xFF000000u : 0u;
                    out[x] = a | uint32_t(expand5[(p >> 10) & 31]) << 16 | uint32_t(expand5[(p >> 5) & 31]) << 8 | expand5[p & 31];
                }
                break;
            }
            default:
            {
                // Any other plain RGB format, the slow way; the rest of the image in one call.
                Format::Image from = {const_cast<uint8_t*>(src), srcPitch, width, height, format, pf};
                Format::Image to = {dst, static_cast<int>(dstPitch), width, height, d9::D3DFMT_A8R8G8B8,
                    Format::toPixelFormat(d9::D3DFMT_A8R8G8B8)};
                const RECT all = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
                if (!Format::copy(from, all, to, all, nullptr))
                {
                    return false;
                }
                // Formats without alpha come out with alpha 255 (Format::Rgb::toArgb).
                return true;
            }
            }
        }
        return true;
    }

    namespace Context
    {
        namespace
        {
            RecursiveSpinLock g_lock;
            bool g_tried = false;
            bool g_available = false;
            HWND g_window = nullptr;

            nvrhi::CommandListHandle g_commands;
            bool g_open = false;
            uint32_t g_epoch = 0;

            nvrhi::BufferHandle g_defaults;
            nvrhi::TextureHandle g_white;
            bool g_whiteUploaded = false;

            // The upload ring. Positions are absolute (they only grow); the buffer offset is the position modulo the size.
            // Each presented frame remembers where its data started: the ring waits for the oldest frame before writing
            // over it.
            constexpr uint32_t kRingSize = 32u << 20;
            nvrhi::BufferHandle g_ring;
            uint8_t* g_ringData = nullptr;
            uint64_t g_head = 0;
            uint64_t g_frameStart = 0;
            struct RingFrame
            {
                uint64_t frame;
                uint64_t start;
            };
            std::deque<RingFrame> g_ringFrames;

            std::unordered_map<uint32_t, nvrhi::SamplerHandle> g_samplers;

            struct FramebufferKey
            {
                nvrhi::ITexture* color;
                nvrhi::ITexture* depth;
                bool operator==(const FramebufferKey&) const = default;
            };
            struct FramebufferKeyHash
            {
                size_t operator()(const FramebufferKey& k) const
                {
                    return std::hash<const void*>()(k.color) ^ (std::hash<const void*>()(k.depth) << 1);
                }
            };
            std::unordered_map<FramebufferKey, nvrhi::FramebufferHandle, FramebufferKeyHash> g_framebuffers;
            std::vector<ForgetFn> g_forgetFns;

            // Copies and fills.
            nvrhi::ShaderHandle g_blitVS, g_copyPS, g_fillPS;
            nvrhi::BindingLayoutHandle g_blitLayout;
            nvrhi::BufferHandle g_blitConstants;
            struct BlitPipelineKey
            {
                nvrhi::Format color;
                nvrhi::Format depth;
                int mode;               // 0 copy, 1 fill
                bool writeColor;
                bool writeDepth;
                bool operator==(const BlitPipelineKey&) const = default;
            };
            std::vector<std::pair<BlitPipelineKey, nvrhi::GraphicsPipelineHandle>> g_blitPipelines;

            // SacredBild's 2D drawing (drawUi) and the overlay.
            nvrhi::ShaderHandle g_uiVS, g_uiPS;
            nvrhi::BindingLayoutHandle g_uiLayout;
            nvrhi::BufferHandle g_uiConstants;
            nvrhi::InputLayoutHandle g_uiInputs[2];
            struct UiPipelineKey
            {
                nvrhi::Format color;
                int format;
                bool premultiplied;
                bool operator==(const UiPipelineKey&) const = default;
            };
            std::vector<std::pair<UiPipelineKey, nvrhi::GraphicsPipelineHandle>> g_uiPipelines;
            std::unordered_map<const void*, nvrhi::BindingSetHandle> g_uiSets;     // by texture (and sampler bit)
            OverlayActiveFn g_overlayActive = nullptr;
            OverlayDrawFn g_overlayDraw = nullptr;
            nvrhi::TextureHandle g_overlayTarget;   // the frame with the overlay, at the frame's size

            struct UiConstants
            {
                float scale[2];
                float pixelOffset[2];
                uint32_t colorOrder;
                float pad[3];
            };

            struct BlitConstants
            {
                float dst[4];
                float src[4];
                float color[4];
                float depth;
                float pad[3];
            };

            Rhi::Settings settingsFor(bool vulkan)
            {
                Rhi::Settings s;
                s.api = vulkan ? Rhi::Api::Vulkan : Rhi::Api::D3D12;
                s.validation = Config::debug.rhiValidation;
                s.vsync = Config::display.vsync;
                s.maxFrameLatency = Config::display.maxFrameLatency;
                return s;
            }

            bool createShared()
            {
                nvrhi::IDevice* dev = Rhi::device();
                g_commands = dev->createCommandList();

                nvrhi::BufferDesc ring;
                ring.byteSize = kRingSize;
                ring.debugName = "DDrawRhi upload ring";
                ring.isVertexBuffer = true;
                ring.isIndexBuffer = true;
                ring.cpuAccess = nvrhi::CpuAccessMode::Write;
                ring.initialState = nvrhi::ResourceStates::VertexBuffer | nvrhi::ResourceStates::IndexBuffer;
                ring.keepInitialState = true;
                g_ring = dev->createBuffer(ring);
                g_ringData = g_ring ? static_cast<uint8_t*>(dev->mapBuffer(g_ring, nvrhi::CpuAccessMode::Write)) : nullptr;

                nvrhi::BufferDesc defaults = ring;
                defaults.byteSize = 64;
                defaults.debugName = "DDrawRhi vertex defaults";
                defaults.isIndexBuffer = false;
                defaults.initialState = nvrhi::ResourceStates::VertexBuffer;
                g_defaults = dev->createBuffer(defaults);
                if (auto* p = g_defaults ? static_cast<uint8_t*>(dev->mapBuffer(g_defaults, nvrhi::CpuAccessMode::Write)) : nullptr)
                {
                    std::memset(p, 0, 64);
                    const uint32_t white = 0xFFFFFFFFu;
                    std::memcpy(p + 12, &white, 4);
                    dev->unmapBuffer(g_defaults);
                }

                g_white = dev->createTexture(nvrhi::TextureDesc().setWidth(1).setHeight(1).setFormat(nvrhi::Format::BGRA8_UNORM)
                    .setDebugName("DDrawRhi white").setInitialState(nvrhi::ResourceStates::ShaderResource).setKeepInitialState(true));

                g_blitVS = Rhi::createShader(nvrhi::ShaderType::Vertex, "vsMain", kBlitVSDxil, kBlitVSSpirv);
                g_copyPS = Rhi::createShader(nvrhi::ShaderType::Pixel, "psCopy", kBlitCopyPSDxil, kBlitCopyPSSpirv);
                g_fillPS = Rhi::createShader(nvrhi::ShaderType::Pixel, "psFill", kBlitFillPSDxil, kBlitFillPSSpirv);
                nvrhi::BindingLayoutDesc layout;
                layout.visibility = nvrhi::ShaderType::All;
                layout.bindings = {
                    nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
                    nvrhi::BindingLayoutItem::Texture_SRV(0),
                    nvrhi::BindingLayoutItem::Sampler(0),
                };
                g_blitLayout = dev->createBindingLayout(layout);
                g_blitConstants = dev->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(BlitConstants), "DDrawRhi blit", 1024));

                g_uiVS = Rhi::createShader(nvrhi::ShaderType::Vertex, "vsMain", kUiVSDxil, kUiVSSpirv);
                g_uiPS = Rhi::createShader(nvrhi::ShaderType::Pixel, "psMain", kUiPSDxil, kUiPSSpirv);
                g_uiLayout = g_blitLayout;      // the same three bindings
                g_uiConstants = dev->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(UiConstants), "DDrawRhi ui", 1024));
                const nvrhi::VertexAttributeDesc rhw[] = {
                    nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(0).setElementStride(28),
                    nvrhi::VertexAttributeDesc().setName("TEXA").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(20).setElementStride(28),
                    nvrhi::VertexAttributeDesc().setName("DIFFUSE").setFormat(nvrhi::Format::R32_UINT).setOffset(16).setElementStride(28),
                };
                const nvrhi::VertexAttributeDesc imgui[] = {
                    nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(0).setElementStride(20),
                    nvrhi::VertexAttributeDesc().setName("TEXA").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(8).setElementStride(20),
                    nvrhi::VertexAttributeDesc().setName("DIFFUSE").setFormat(nvrhi::Format::R32_UINT).setOffset(16).setElementStride(20),
                };
                g_uiInputs[0] = dev->createInputLayout(rhw, 3);
                g_uiInputs[1] = dev->createInputLayout(imgui, 3);

                const bool ok = g_commands && g_ringData && g_defaults && g_white && g_blitVS && g_copyPS && g_fillPS &&
                    g_blitLayout && g_blitConstants && g_uiVS && g_uiPS && g_uiConstants && g_uiInputs[0] && g_uiInputs[1];
                if (!ok)
                {
                    LOG("NVRHI backend: creating the shared resources failed");
                }
                return ok;
            }

            nvrhi::GraphicsPipelineHandle blitPipeline(nvrhi::IFramebuffer* fb, int mode, bool writeColor, bool writeDepth)
            {
                const nvrhi::FramebufferInfoEx& info = fb->getFramebufferInfo();
                const BlitPipelineKey key = {info.colorFormats.empty() ? nvrhi::Format::UNKNOWN : info.colorFormats[0],
                    info.depthFormat, mode, writeColor, writeDepth};
                for (auto& [k, p] : g_blitPipelines)
                {
                    if (k == key)
                    {
                        return p;
                    }
                }
                nvrhi::GraphicsPipelineDesc desc;
                desc.VS = g_blitVS;
                desc.PS = mode == 0 ? g_copyPS : g_fillPS;
                desc.primType = nvrhi::PrimitiveType::TriangleList;
                desc.bindingLayouts = {g_blitLayout};
                desc.renderState.rasterState.setCullNone();
                desc.renderState.depthStencilState.setDepthTestEnable(writeDepth).setDepthWriteEnable(writeDepth)
                    .setDepthFunc(nvrhi::ComparisonFunc::Always).setStencilEnable(false);
                desc.renderState.blendState.targets[0].setColorWriteMask(writeColor ? nvrhi::ColorMask::All : nvrhi::ColorMask(0));
                nvrhi::GraphicsPipelineHandle pipeline = Rhi::device()->createGraphicsPipeline(desc, info);
                g_blitPipelines.emplace_back(key, pipeline);
                return pipeline;
            }

            void ringFrameEnded(uint64_t frame)
            {
                g_ringFrames.push_back({frame, g_frameStart});
                g_frameStart = g_head;
            }
        }

        bool available()
        {
            std::scoped_lock lock(g_lock);
            if (g_tried)
            {
                return g_available;
            }
            g_tried = true;
            const bool vulkan = Config::ddraw.vulkan;
            g_available = Rhi::initDevice(settingsFor(vulkan));
            if (!g_available)
            {
                LOG("NVRHI backend: {} failed, trying {}", vulkan ? "Vulkan" : "Direct3D 12", vulkan ? "Direct3D 12" : "Vulkan");
                g_available = Rhi::initDevice(settingsFor(!vulkan));
            }
            if (g_available && !createShared())
            {
                Rhi::shutdown();
                g_available = false;
            }
            return g_available;
        }

        const std::string& adapterName()
        {
            return Rhi::adapterName();
        }

        RecursiveSpinLock& lock()
        {
            return g_lock;
        }

        nvrhi::IDevice* device()
        {
            return Rhi::device();
        }

        nvrhi::ICommandList* commands()
        {
            if (!g_open)
            {
                g_commands->open();
                g_open = true;
                ++g_epoch;
                if (!g_whiteUploaded)
                {
                    const uint32_t white = 0xFFFFFFFFu;
                    g_commands->writeTexture(g_white, 0, 0, &white, 4);
                    g_whiteUploaded = true;
                }
            }
            return g_commands;
        }

        uint32_t epoch()
        {
            return g_epoch;
        }

        void flush(bool wait)
        {
            if (g_open)
            {
                g_commands->close();
                g_open = false;
                Rhi::device()->executeCommandList(g_commands);
            }
            if (wait)
            {
                Rhi::device()->waitForIdle();
                // Everything recorded so far is done: the ring is free.
                g_ringFrames.clear();
                g_frameStart = g_head;
            }
        }

        bool upload(uint32_t bytes, Upload& out)
        {
            bytes = (bytes + 15) & ~15u;
            if (!g_ringData || bytes > kRingSize)
            {
                return false;
            }
            for (;;)
            {
                uint64_t pos = g_head;
                if (pos % kRingSize + bytes > kRingSize)
                {
                    pos += kRingSize - pos % kRingSize;     // never split: start the next lap
                }
                while (!g_ringFrames.empty() && Rhi::frameFinished(g_ringFrames.front().frame))
                {
                    g_ringFrames.pop_front();
                }
                const uint64_t oldest = g_ringFrames.empty() ? g_frameStart : g_ringFrames.front().start;
                if (pos + bytes - oldest <= kRingSize)
                {
                    g_head = pos + bytes;
                    out.buffer = g_ring;
                    out.offset = static_cast<uint32_t>(pos % kRingSize);
                    out.data = g_ringData + out.offset;
                    return true;
                }
                if (!g_ringFrames.empty())
                {
                    Rhi::waitForFrame(g_ringFrames.front().frame);
                    g_ringFrames.pop_front();
                }
                else
                {
                    // This frame alone filled the ring: what it recorded so far is executed and waited for.
                    static bool logged = false;
                    if (!logged)
                    {
                        logged = true;
                        LOG("NVRHI backend: a frame's vertex data exceeds {} MB; flushing mid-frame", kRingSize >> 20);
                    }
                    flush(true);
                }
            }
        }

        nvrhi::IBuffer* vertexDefaults()
        {
            return g_defaults;
        }

        nvrhi::ITexture* whiteTexture()
        {
            return g_white;
        }

        nvrhi::ISampler* sampler(DWORD addressU, DWORD addressV, bool magLinear, bool minLinear, DWORD mipFilter, DWORD border)
        {
            const uint32_t key = (addressU & 7) | (addressV & 7) << 3 | (magLinear ? 1u : 0u) << 6 | (minLinear ? 1u : 0u) << 7 |
                (mipFilter & 3) << 8 | (border ? 1u : 0u) << 10;
            auto it = g_samplers.find(key);
            if (it != g_samplers.end())
            {
                return it->second;
            }
            auto mode = [](DWORD a) {
                switch (a)
                {
                case D3DTADDRESS_MIRROR: return nvrhi::SamplerAddressMode::Mirror;
                case D3DTADDRESS_CLAMP: return nvrhi::SamplerAddressMode::Clamp;
                case D3DTADDRESS_BORDER: return nvrhi::SamplerAddressMode::Border;
                default: return nvrhi::SamplerAddressMode::Wrap;
                }
            };
            nvrhi::SamplerDesc desc;
            desc.addressU = mode(addressU);
            desc.addressV = mode(addressV);
            desc.addressW = nvrhi::SamplerAddressMode::Wrap;
            desc.magFilter = magLinear;
            desc.minFilter = minLinear;
            desc.mipFilter = mipFilter == D3DTFP_LINEAR;
            if (border)
            {
                desc.borderColor = nvrhi::Color(((border >> 16) & 255) / 255.0f, ((border >> 8) & 255) / 255.0f,
                    (border & 255) / 255.0f, (border >> 24) / 255.0f);
            }
            nvrhi::SamplerHandle sampler = Rhi::device()->createSampler(desc);
            g_samplers.emplace(key, sampler);
            return sampler;
        }

        nvrhi::IFramebuffer* framebuffer(nvrhi::ITexture* color, nvrhi::ITexture* depth)
        {
            const FramebufferKey key = {color, depth};
            auto it = g_framebuffers.find(key);
            if (it != g_framebuffers.end())
            {
                return it->second;
            }
            nvrhi::FramebufferDesc desc;
            if (color)
            {
                desc.addColorAttachment(color);
            }
            if (depth)
            {
                desc.setDepthAttachment(depth);
            }
            nvrhi::FramebufferHandle fb = Rhi::device()->createFramebuffer(desc);
            g_framebuffers.emplace(key, fb);
            return fb;
        }

        void forget(nvrhi::ITexture* texture)
        {
            if (!texture)
            {
                return;
            }
            std::scoped_lock lock(g_lock);
            for (auto it = g_framebuffers.begin(); it != g_framebuffers.end();)
            {
                it = it->first.color == texture || it->first.depth == texture ? g_framebuffers.erase(it) : std::next(it);
            }
            g_uiSets.erase(texture);
            g_uiSets.erase(reinterpret_cast<const uint8_t*>(texture) + 1);
            for (ForgetFn fn : g_forgetFns)
            {
                fn(texture);
            }
        }

        void onForget(ForgetFn fn)
        {
            std::scoped_lock lock(g_lock);
            g_forgetFns.push_back(fn);
        }

        void copy(nvrhi::ITexture* src, const RECT& srcRect, nvrhi::ITexture* dst, const RECT& dstRect, bool linear)
        {
            const nvrhi::TextureDesc& s = src->getDesc();
            const nvrhi::TextureDesc& d = dst->getDesc();
            nvrhi::IFramebuffer* fb = framebuffer(dst, nullptr);
            nvrhi::GraphicsPipelineHandle pipeline = blitPipeline(fb, 0, true, false);
            BlitConstants c = {};
            c.dst[0] = dstRect.left * 2.0f / d.width - 1.0f;
            c.dst[1] = 1.0f - dstRect.top * 2.0f / d.height;
            c.dst[2] = dstRect.right * 2.0f / d.width - 1.0f;
            c.dst[3] = 1.0f - dstRect.bottom * 2.0f / d.height;
            c.src[0] = static_cast<float>(srcRect.left) / s.width;
            c.src[1] = static_cast<float>(srcRect.top) / s.height;
            c.src[2] = static_cast<float>(srcRect.right) / s.width;
            c.src[3] = static_cast<float>(srcRect.bottom) / s.height;
            nvrhi::BindingSetDesc set;
            set.bindings = {
                nvrhi::BindingSetItem::ConstantBuffer(0, g_blitConstants),
                nvrhi::BindingSetItem::Texture_SRV(0, src),
                nvrhi::BindingSetItem::Sampler(0, sampler(D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, linear, linear, D3DTFP_NONE, 0)),
            };
            nvrhi::BindingSetHandle bindings = Rhi::device()->createBindingSet(set, g_blitLayout);
            nvrhi::ICommandList* cmd = commands();
            cmd->writeBuffer(g_blitConstants, &c, sizeof(c));
            nvrhi::GraphicsState state;
            state.pipeline = pipeline;
            state.framebuffer = fb;
            state.bindings = {bindings};
            state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(d.width), static_cast<float>(d.height)));
            cmd->setGraphicsState(state);
            cmd->draw(nvrhi::DrawArguments().setVertexCount(6));
        }

        void fill(nvrhi::ITexture* color, nvrhi::ITexture* depth, const RECT& rect, const float rgba[4], bool writeColor,
            bool writeDepth, float z)
        {
            nvrhi::ITexture* any = color ? color : depth;
            if (!any || (!writeColor && !writeDepth))
            {
                return;
            }
            const nvrhi::TextureDesc& d = any->getDesc();
            nvrhi::IFramebuffer* fb = framebuffer(color, depth);
            nvrhi::GraphicsPipelineHandle pipeline = blitPipeline(fb, 1, writeColor && color, writeDepth && depth);
            BlitConstants c = {};
            c.dst[0] = -1.0f;
            c.dst[1] = 1.0f;
            c.dst[2] = 1.0f;
            c.dst[3] = -1.0f;
            std::memcpy(c.color, rgba, sizeof(c.color));
            c.depth = z;
            static nvrhi::BindingSetHandle s_bindings;
            if (!s_bindings)
            {
                nvrhi::BindingSetDesc set;
                set.bindings = {
                    nvrhi::BindingSetItem::ConstantBuffer(0, g_blitConstants),
                    nvrhi::BindingSetItem::Texture_SRV(0, g_white),
                    nvrhi::BindingSetItem::Sampler(0, sampler(D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, false, false, D3DTFP_NONE, 0)),
                };
                s_bindings = Rhi::device()->createBindingSet(set, g_blitLayout);
            }
            nvrhi::ICommandList* cmd = commands();
            cmd->writeBuffer(g_blitConstants, &c, sizeof(c));
            nvrhi::GraphicsState state;
            state.pipeline = pipeline;
            state.framebuffer = fb;
            state.bindings = {s_bindings};
            nvrhi::Viewport vp(static_cast<float>(d.width), static_cast<float>(d.height));
            state.viewport.addViewport(vp);
            state.viewport.addScissorRect(nvrhi::Rect(rect.left, rect.right, rect.top, rect.bottom));
            cmd->setGraphicsState(state);
            cmd->draw(nvrhi::DrawArguments().setVertexCount(6));
        }

        void drawUi(nvrhi::ITexture* target, UiVertex format, bool premultiplied, nvrhi::ITexture* texture, bool linear,
            const Upload& vertices, const Upload& indices, uint32_t firstIndex, uint32_t indexCount, uint32_t baseVertex,
            const RECT& scissor)
        {
            if (!target || !indexCount)
            {
                return;
            }
            nvrhi::IFramebuffer* fb = framebuffer(target, nullptr);
            const nvrhi::FramebufferInfoEx& info = fb->getFramebufferInfo();
            const UiPipelineKey key = {info.colorFormats[0], static_cast<int>(format), premultiplied};
            nvrhi::GraphicsPipelineHandle pipeline;
            for (auto& [k, p] : g_uiPipelines)
            {
                if (k == key)
                {
                    pipeline = p;
                }
            }
            if (!pipeline)
            {
                nvrhi::GraphicsPipelineDesc desc;
                desc.VS = g_uiVS;
                desc.PS = g_uiPS;
                desc.inputLayout = g_uiInputs[format == UiVertex::ImGui ? 1 : 0];
                desc.primType = nvrhi::PrimitiveType::TriangleList;
                desc.bindingLayouts = {g_uiLayout};
                desc.renderState.rasterState.setCullNone().setScissorEnable(true);
                desc.renderState.depthStencilState.setDepthTestEnable(false).setDepthWriteEnable(false);
                desc.renderState.blendState.targets[0].setBlendEnable(true)
                    .setSrcBlend(premultiplied ? nvrhi::BlendFactor::One : nvrhi::BlendFactor::SrcAlpha)
                    .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha).setSrcBlendAlpha(nvrhi::BlendFactor::One)
                    .setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha);
                pipeline = Rhi::device()->createGraphicsPipeline(desc, info);
                g_uiPipelines.emplace_back(key, pipeline);
            }
            if (!pipeline)
            {
                return;
            }
            nvrhi::ITexture* tex = texture ? texture : g_white.Get();
            const void* setKey = reinterpret_cast<const uint8_t*>(tex) + (linear ? 1 : 0);
            nvrhi::BindingSetHandle& set = g_uiSets[setKey];
            if (!set)
            {
                nvrhi::BindingSetDesc desc;
                desc.bindings = {
                    nvrhi::BindingSetItem::ConstantBuffer(0, g_uiConstants),
                    nvrhi::BindingSetItem::Texture_SRV(0, tex),
                    nvrhi::BindingSetItem::Sampler(0, sampler(D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, linear, linear, D3DTFP_NONE, 0)),
                };
                set = Rhi::device()->createBindingSet(desc, g_uiLayout);
            }
            const nvrhi::TextureDesc& td = target->getDesc();
            UiConstants c = {};
            c.scale[0] = 2.0f / static_cast<float>(td.width);
            c.scale[1] = -2.0f / static_cast<float>(td.height);
            c.pixelOffset[0] = c.pixelOffset[1] = format == UiVertex::Rhw ? 0.5f : 0.0f;
            c.colorOrder = format == UiVertex::ImGui ? 1u : 0u;
            nvrhi::ICommandList* cmd = commands();
            cmd->writeBuffer(g_uiConstants, &c, sizeof(c));
            nvrhi::GraphicsState state;
            state.pipeline = pipeline;
            state.framebuffer = fb;
            state.bindings = {set};
            state.vertexBuffers = {nvrhi::VertexBufferBinding().setBuffer(vertices.buffer).setSlot(0).setOffset(vertices.offset)};
            state.indexBuffer = nvrhi::IndexBufferBinding().setBuffer(indices.buffer).setFormat(nvrhi::Format::R16_UINT)
                .setOffset(indices.offset);
            state.viewport.addViewport(nvrhi::Viewport(static_cast<float>(td.width), static_cast<float>(td.height)));
            const LONG x0 = std::clamp<LONG>(scissor.left, 0, static_cast<LONG>(td.width));
            const LONG y0 = std::clamp<LONG>(scissor.top, 0, static_cast<LONG>(td.height));
            const LONG x1 = std::clamp<LONG>(scissor.right, x0, static_cast<LONG>(td.width));
            const LONG y1 = std::clamp<LONG>(scissor.bottom, y0, static_cast<LONG>(td.height));
            if (x1 <= x0 || y1 <= y0)
            {
                return;
            }
            state.viewport.addScissorRect(nvrhi::Rect(x0, x1, y0, y1));
            cmd->setGraphicsState(state);
            cmd->drawIndexed(nvrhi::DrawArguments().setVertexCount(indexCount).setStartIndexLocation(firstIndex)
                .setStartVertexLocation(baseVertex));
        }

        void setOverlay(OverlayActiveFn active, OverlayDrawFn draw)
        {
            std::scoped_lock lock(g_lock);
            g_overlayActive = active;
            g_overlayDraw = draw;
        }

        void setWindow(HWND window)
        {
            std::scoped_lock lock(g_lock);
            if (window)
            {
                g_window = window;
            }
        }

        HWND window()
        {
            return g_window;
        }

        HRESULT present(nvrhi::ITexture* frame, const RECT* srcRect)
        {
            std::scoped_lock lock(g_lock);
            if (!g_available || !frame)
            {
                return DDERR_GENERIC;
            }
            RECT client = {};
            if (!g_window || !GetClientRect(g_window, &client) || client.right <= 0 || client.bottom <= 0)
            {
                // No window yet, or minimized: the frame is executed, not shown.
                flush(false);
                return DD_OK;
            }
            const uint32_t w = static_cast<uint32_t>(client.right), h = static_cast<uint32_t>(client.bottom);
            if (!Rhi::window())
            {
                if (!Rhi::attachWindow(g_window, w, h))
                {
                    flush(false);
                    return DDERR_GENERIC;
                }
            }
            else if (w != Rhi::width() || h != Rhi::height())
            {
                flush(false);
                Rhi::resize(w, h);
            }
            if (!Rhi::beginFrame())
            {
                flush(false);
                return DD_OK;
            }
            const nvrhi::TextureDesc& fd = frame->getDesc();
            RECT src = {0, 0, static_cast<LONG>(fd.width), static_cast<LONG>(fd.height)};
            if (srcRect)
            {
                src = *srcRect;
            }
            const bool overlayOn = g_overlayActive && g_overlayDraw && g_overlayActive();
            if (overlayOn)
            {
                // The overlay goes over a copy of the frame at the frame's size, which is then shown in its place.
                const uint32_t ow = static_cast<uint32_t>(src.right - src.left), oh = static_cast<uint32_t>(src.bottom - src.top);
                if (!g_overlayTarget || g_overlayTarget->getDesc().width != ow || g_overlayTarget->getDesc().height != oh)
                {
                    if (g_overlayTarget)
                    {
                        forget(g_overlayTarget);
                    }
                    g_overlayTarget = Rhi::device()->createTexture(nvrhi::TextureDesc().setWidth(ow).setHeight(oh)
                        .setFormat(nvrhi::Format::BGRA8_UNORM).setIsRenderTarget(true).setDebugName("overlay frame")
                        .setInitialState(nvrhi::ResourceStates::ShaderResource).setKeepInitialState(true));
                }
                if (g_overlayTarget)
                {
                    nvrhi::TextureSlice from;
                    from.x = static_cast<uint32_t>(src.left);
                    from.y = static_cast<uint32_t>(src.top);
                    from.width = ow;
                    from.height = oh;
                    nvrhi::TextureSlice to;
                    to.width = ow;
                    to.height = oh;
                    commands()->copyTexture(g_overlayTarget, to, frame, from);
                    g_overlayDraw(g_overlayTarget, g_window);
                    frame = g_overlayTarget;
                    src = {0, 0, static_cast<LONG>(ow), static_cast<LONG>(oh)};
                }
            }
            const RECT dst = {0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
            const bool scaled = src.right - src.left != dst.right || src.bottom - src.top != dst.bottom;
            copy(frame, src, Rhi::backBuffer(), dst, scaled);
            flush(false);
            const uint64_t presented = Rhi::frameIndex();
            Rhi::present();
            ringFrameEnded(presented);
            return DD_OK;
        }
    }
}
