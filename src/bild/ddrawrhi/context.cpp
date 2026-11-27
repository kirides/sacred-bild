#include "ddrawrhi/context.h"
#include "ddraw9/format.h"
#include "rhi/rhi.h"
#include "settings/config/ddraw.h"
#include "settings/config/debug.h"
#include "settings/config/display.h"
#include "settings/config/render.h"
#include "log.h"

#include "ddrawrhi/shaders/blit_shaders.inc"
#include "ddrawrhi/shaders/ui_shaders.inc"

#include <nvrhi/utils.h>

#include <algorithm>
#include <emmintrin.h>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
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

    // D24S8 where the device has it as a depth buffer, else D32S8 (AMD's Vulkan drivers have no D24S8).
    static nvrhi::Format depthStencil()
    {
        static const nvrhi::Format format = [] {
            const nvrhi::FormatSupport support = Rhi::device()->queryFormatSupport(nvrhi::Format::D24S8);
            if ((support & nvrhi::FormatSupport::DepthStencil) == nvrhi::FormatSupport::DepthStencil)
            {
                return nvrhi::Format::D24S8;
            }
            LOG("NVRHI backend: the device has no D24S8 depth buffers, using D32S8");
            return nvrhi::Format::D32S8;
        }();
        return format;
    }

    nvrhi::Format gpuFormat(d9::D3DFORMAT format)
    {
        switch (format)
        {
        case d9::D3DFMT_DXT1: return nvrhi::Format::BC1_UNORM;
        case d9::D3DFMT_DXT2: case d9::D3DFMT_DXT3: return nvrhi::Format::BC2_UNORM;
        case d9::D3DFMT_DXT4: case d9::D3DFMT_DXT5: return nvrhi::Format::BC3_UNORM;
        case d9::D3DFMT_D16: return nvrhi::Format::D16;
        case d9::D3DFMT_D24X8: case d9::D3DFMT_D24S8: return depthStencil();
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

    namespace
    {
        // 16-bit pixels to BGRA8, eight at a time (SSE2). Channels widen as the tables in toBgra8 do: 4 bits times 17,
        // 5 and 6 bits rounded (x * 255 / 31 is (x * 527 + 23) >> 6, x * 255 / 63 is (x * 259 + 33) >> 6, exactly).
        // Returns the pixels done; toBgra8's loops do the rest.
        uint32_t expand16(d9::D3DFORMAT format, const uint16_t* in, uint32_t* out, uint32_t width)
        {
            const __m128i m4 = _mm_set1_epi16(15), m5 = _mm_set1_epi16(31), m6 = _mm_set1_epi16(63);
            const auto widen5 = [](__m128i v) { return _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(v, _mm_set1_epi16(527)), _mm_set1_epi16(23)), 6); };
            const auto widen6 = [](__m128i v) { return _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(v, _mm_set1_epi16(259)), _mm_set1_epi16(33)), 6); };
            const auto widen4 = [](__m128i v) { return _mm_mullo_epi16(v, _mm_set1_epi16(17)); };
            uint32_t x = 0;
            for (; x + 8 <= width; x += 8)
            {
                const __m128i p = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + x));
                __m128i a, r, g, b;
                switch (format)
                {
                case d9::D3DFMT_R5G6B5:
                    a = _mm_set1_epi16(255);
                    r = widen5(_mm_srli_epi16(p, 11));
                    g = widen6(_mm_and_si128(_mm_srli_epi16(p, 5), m6));
                    b = widen5(_mm_and_si128(p, m5));
                    break;
                case d9::D3DFMT_X1R5G5B5:
                case d9::D3DFMT_A1R5G5B5:
                    a = format == d9::D3DFMT_A1R5G5B5 ? _mm_srli_epi16(_mm_srai_epi16(p, 15), 8) : _mm_set1_epi16(255);
                    r = widen5(_mm_and_si128(_mm_srli_epi16(p, 10), m5));
                    g = widen5(_mm_and_si128(_mm_srli_epi16(p, 5), m5));
                    b = widen5(_mm_and_si128(p, m5));
                    break;
                case d9::D3DFMT_A4R4G4B4:
                    a = widen4(_mm_srli_epi16(p, 12));
                    r = widen4(_mm_and_si128(_mm_srli_epi16(p, 8), m4));
                    g = widen4(_mm_and_si128(_mm_srli_epi16(p, 4), m4));
                    b = widen4(_mm_and_si128(p, m4));
                    break;
                default:
                    return x;
                }
                const __m128i bg = _mm_or_si128(b, _mm_slli_epi16(g, 8));
                const __m128i ra = _mm_or_si128(r, _mm_slli_epi16(a, 8));
                _mm_storeu_si128(reinterpret_cast<__m128i*>(out + x), _mm_unpacklo_epi16(bg, ra));
                _mm_storeu_si128(reinterpret_cast<__m128i*>(out + x + 4), _mm_unpackhi_epi16(bg, ra));
            }
            return x;
        }
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
            uint32_t x0 = 0;        // the 16-bit formats: most of the row in expand16, the rest below
            if (format == d9::D3DFMT_A4R4G4B4 || format == d9::D3DFMT_R5G6B5 || format == d9::D3DFMT_X1R5G5B5 ||
                format == d9::D3DFMT_A1R5G5B5)
            {
                x0 = expand16(format, reinterpret_cast<const uint16_t*>(in), out, width);
            }
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
                for (uint32_t x = x0; x < width; ++x)
                {
                    const uint16_t p = reinterpret_cast<const uint16_t*>(in)[x];
                    out[x] = uint32_t(expand4[p >> 12]) << 24 | uint32_t(expand4[(p >> 8) & 15]) << 16 |
                        uint32_t(expand4[(p >> 4) & 15]) << 8 | expand4[p & 15];
                }
                break;
            case d9::D3DFMT_R5G6B5:
                for (uint32_t x = x0; x < width; ++x)
                {
                    const uint16_t p = reinterpret_cast<const uint16_t*>(in)[x];
                    out[x] = 0xFF000000u | uint32_t(expand5[p >> 11]) << 16 | uint32_t(expand6[(p >> 5) & 63]) << 8 | expand5[p & 31];
                }
                break;
            case d9::D3DFMT_X1R5G5B5:
            case d9::D3DFMT_A1R5G5B5:
            {
                const bool alpha = format == d9::D3DFMT_A1R5G5B5;
                for (uint32_t x = x0; x < width; ++x)
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

            // The command list being recorded (Context lock held) and the idle ones; recorded lists go to the render thread.
            nvrhi::CommandListHandle g_commands;
            std::vector<nvrhi::CommandListHandle> g_idleLists;      // guarded by g_jobMutex
            bool g_open = false;
            uint32_t g_epoch = 0;

            // The render thread: the only thread that executes command lists and presents. The game's threads record
            // (under the Context lock) and hand each closed list over with a serial; a frame's list comes with the
            // texture it is presented from.
            struct Job
            {
                nvrhi::CommandListHandle list;  // null: nothing recorded (a present or a wait only)
                uint64_t serial = 0;
                nvrhi::TextureHandle present;   // shown after the list (a present)
                bool idle = false;              // wait until the GPU is idle afterwards
            };
            std::mutex g_jobMutex;
            std::condition_variable g_jobCv;    // a job was queued
            std::condition_variable g_doneCv;   // a job was done
            std::deque<Job> g_jobs;
            uint64_t g_nextSerial = 1;          // Context lock
            uint64_t g_doneSerial = 0;          // jobs up to here executed (g_jobMutex)
            int g_pendingPresents = 0;          // presents queued and not shown yet (g_jobMutex)
            struct Executed
            {
                uint64_t serial;
                nvrhi::EventQueryHandle query;
            };
            std::deque<Executed> g_executed;    // the GPU's progress through the executed lists (g_jobMutex)
            uint64_t g_gpuDoneSerial = 0;       // lists up to here are done on the GPU (g_jobMutex)
            std::vector<nvrhi::EventQueryHandle> g_freeQueries;    // g_jobMutex
            std::thread g_renderThread;

            // The textures frames are presented from (a copy of the frame and the overlay), rotating; the render thread
            // copies them into the swap chain. Context lock.
            constexpr int kPresentTextures = 5;
            nvrhi::TextureHandle g_presentTextures[kPresentTextures];
            int g_presentNext = 0;

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

            // scratchTexture: by width << 40 | height << 16 | format. Context lock.
            std::unordered_map<uint64_t, nvrhi::TextureHandle> g_scratch;
            uint64_t g_scratchBytes = 0;
            constexpr uint64_t kScratchBytes = 96ull << 20;     // more and the pool starts again
            // copy(): binding sets by source texture (| 1: linear), until the texture is forgotten. g_lock.
            std::unordered_map<const void*, nvrhi::BindingSetHandle> g_copySets;

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
                s.pipelineCache = Config::render.pipelineCache;
                s.placedResources = Config::render.placedResources;
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

            // The ring's frames are the serials of the lists that last read their data.
            void ringFrameEnded(uint64_t serial)
            {
                g_ringFrames.push_back({serial, g_frameStart});
                g_frameStart = g_head;
            }

            // Whether the GPU is done with the list of `serial`; with `wait`, until it is (the list must be queued).
            bool gpuDone(uint64_t serial, bool wait)
            {
                std::unique_lock lock(g_jobMutex);
                for (;;)
                {
                    while (!g_executed.empty() && Rhi::device()->pollEventQuery(g_executed.front().query))
                    {
                        g_gpuDoneSerial = g_executed.front().serial;
                        g_freeQueries.push_back(std::move(g_executed.front().query));
                        g_executed.pop_front();
                    }
                    if (serial <= g_gpuDoneSerial || !wait)
                    {
                        return serial <= g_gpuDoneSerial;
                    }
                    if (g_doneSerial < serial)
                    {
                        g_doneCv.wait(lock, [&] { return g_doneSerial >= serial; });
                        continue;
                    }
                    // Executed: wait for its query (or a later one) without holding the job lock.
                    nvrhi::EventQueryHandle query;
                    uint64_t querySerial = 0;
                    for (const Executed& e : g_executed)
                    {
                        if (e.serial >= serial)
                        {
                            query = e.query;
                            querySerial = e.serial;
                            break;
                        }
                    }
                    if (!query)
                    {
                        return true;    // nothing left in flight
                    }
                    lock.unlock();
                    ++counters().waits;
                    Rhi::device()->waitEventQuery(query);
                    lock.lock();
                    g_gpuDoneSerial = std::max(g_gpuDoneSerial, querySerial);
                    while (!g_executed.empty() && g_executed.front().serial <= g_gpuDoneSerial)
                    {
                        g_freeQueries.push_back(std::move(g_executed.front().query));
                        g_executed.pop_front();
                    }
                }
            }

            // [Debug] D3DStats: GPU time of the recorded command lists, one timer query per list (Context lock).
            struct GpuTimer
            {
                nvrhi::TimerQueryHandle current;            // in the open list
                // Ended, with the serial of their job: read once the GPU is done with it (asking Vulkan earlier reads
                // a query the GPU has not reset yet).
                std::vector<std::pair<nvrhi::TimerQueryHandle, uint64_t>> pending;
                std::vector<nvrhi::TimerQueryHandle> free;
                double seconds = 0.0;
                uint32_t lists = 0, frames = 0;
                DWORD nextLog = 0;
            };
            GpuTimer g_gpuTimer;

            void gpuTimerBegin(nvrhi::ICommandList* cmd)
            {
                if (!Config::debug.d3dStats)
                {
                    return;
                }
                GpuTimer& t = g_gpuTimer;
                if (!t.free.empty())
                {
                    t.current = std::move(t.free.back());
                    t.free.pop_back();
                }
                else
                {
                    t.current = Rhi::device()->createTimerQuery();
                }
                if (t.current)
                {
                    cmd->beginTimerQuery(t.current);
                }
            }

            void gpuTimerEnd(nvrhi::ICommandList* cmd, uint64_t serial)
            {
                GpuTimer& t = g_gpuTimer;
                if (t.current)
                {
                    cmd->endTimerQuery(t.current);
                    t.pending.emplace_back(std::move(t.current), serial);
                    t.current = nullptr;
                }
            }

            // Once a frame: what the GPU finished, logged every second with the frame count.
            void gpuTimerFrame()
            {
                GpuTimer& t = g_gpuTimer;
                if (!Config::debug.d3dStats)
                {
                    return;
                }
                ++t.frames;
                nvrhi::IDevice* dev = Rhi::device();
                while (!t.pending.empty() && gpuDone(t.pending.front().second, false) && dev->pollTimerQuery(t.pending.front().first))
                {
                    t.seconds += dev->getTimerQueryTime(t.pending.front().first);
                    dev->resetTimerQuery(t.pending.front().first);
                    t.free.push_back(std::move(t.pending.front().first));
                    t.pending.erase(t.pending.begin());
                    ++t.lists;
                }
                const DWORD now = GetTickCount();
                if (!t.nextLog)
                {
                    t.nextLog = now + 1000;
                }
                if (static_cast<int32_t>(now - t.nextLog) >= 0 && t.frames)
                {
                    Counters& k = counters();
                    LOG("NVRHI backend: GPU {:.3f} ms a frame ({} frames, {:.1f} command lists a frame); per frame: {:.1f} temporary "
                        "textures, {:.1f} new textures ({:.3f} ms), {:.2f} read-backs, {:.2f} GPU waits", t.seconds * 1000.0 / t.frames,
                        t.frames, static_cast<double>(t.lists) / t.frames, k.tempTextures.exchange(0) / double(t.frames),
                        k.textures.exchange(0) / double(t.frames), k.textureMicros.exchange(0) / 1000.0 / t.frames,
                        k.readBacks.exchange(0) / double(t.frames),
                        k.waits.exchange(0) / double(t.frames));
                    t.seconds = 0.0;
                    t.lists = t.frames = 0;
                    t.nextLog = now + 1000;
                }
            }

            void runPendingFwd();
            // Hands what is recorded to the render thread; returns the job's serial. Context lock held.
            uint64_t submit(nvrhi::ITexture* present, bool idle)
            {
                runPendingFwd();
                Job job;
                if (g_open)
                {
                    gpuTimerEnd(g_commands, g_nextSerial);     // this job's serial (below)
                    g_commands->close();
                    g_open = false;
                    job.list = g_commands;
                    g_commands = nullptr;
                }
                job.serial = g_nextSerial++;
                job.present = present;
                job.idle = idle;
                {
                    std::scoped_lock lock(g_jobMutex);
                    if (present)
                    {
                        ++g_pendingPresents;
                    }
                    g_jobs.push_back(std::move(job));
                }
                g_jobCv.notify_one();
                return g_nextSerial - 1;
            }

            void waitDone(uint64_t serial)
            {
                std::unique_lock lock(g_jobMutex);
                g_doneCv.wait(lock, [&] { return g_doneSerial >= serial; });
            }

            // The render thread's own copy into the swap chain: its pipelines, bindings and constants are its alone.
            struct Presenter
            {
                nvrhi::CommandListHandle commands;
                nvrhi::BufferHandle constants;
                nvrhi::SamplerHandle linear, point;
                std::vector<std::pair<nvrhi::Format, nvrhi::GraphicsPipelineHandle>> pipelines;
                std::vector<std::pair<std::pair<nvrhi::ITexture*, bool>, nvrhi::BindingSetHandle>> sets;
                std::vector<std::pair<nvrhi::ITexture*, nvrhi::FramebufferHandle>> framebuffers;
            };

            void show(Presenter& p, nvrhi::ITexture* source)
            {
                nvrhi::IDevice* dev = Rhi::device();
                HWND window = g_window;
                RECT client = {};
                if (!window || !GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0)
                {
                    return;     // no window yet, or minimized
                }
                const uint32_t w = static_cast<uint32_t>(client.right), h = static_cast<uint32_t>(client.bottom);
                if (!Rhi::window())
                {
                    if (!Rhi::attachWindow(window, w, h))
                    {
                        return;
                    }
                }
                else if (w != Rhi::width() || h != Rhi::height())
                {
                    p.framebuffers.clear();
                    Rhi::resize(w, h);
                }
                if (!Rhi::beginFrame())
                {
                    return;
                }
                nvrhi::ITexture* back = Rhi::backBuffer();
                nvrhi::IFramebuffer* fb = nullptr;
                for (auto& [t, f] : p.framebuffers)
                {
                    if (t == back)
                    {
                        fb = f;
                    }
                }
                if (!fb)
                {
                    p.framebuffers.emplace_back(back, dev->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(back)));
                    fb = p.framebuffers.back().second;
                }
                const nvrhi::FramebufferInfoEx& info = fb->getFramebufferInfo();
                nvrhi::IGraphicsPipeline* pipeline = nullptr;
                for (auto& [f, pso] : p.pipelines)
                {
                    if (f == info.colorFormats[0])
                    {
                        pipeline = pso;
                    }
                }
                if (!pipeline)
                {
                    nvrhi::GraphicsPipelineDesc desc;
                    desc.VS = g_blitVS;
                    desc.PS = g_copyPS;
                    desc.primType = nvrhi::PrimitiveType::TriangleList;
                    desc.bindingLayouts = {g_blitLayout};
                    desc.renderState.rasterState.setCullNone();
                    desc.renderState.depthStencilState.setDepthTestEnable(false).setDepthWriteEnable(false);
                    p.pipelines.emplace_back(info.colorFormats[0], dev->createGraphicsPipeline(desc, info));
                    pipeline = p.pipelines.back().second;
                }
                const nvrhi::TextureDesc& sd = source->getDesc();
                const bool scaled = sd.width != w || sd.height != h;
                nvrhi::IBindingSet* set = nullptr;
                for (auto& [k, bs] : p.sets)
                {
                    if (k.first == source && k.second == scaled)
                    {
                        set = bs;
                    }
                }
                if (!set)
                {
                    nvrhi::BindingSetDesc desc;
                    desc.bindings = {
                        nvrhi::BindingSetItem::ConstantBuffer(0, p.constants),
                        nvrhi::BindingSetItem::Texture_SRV(0, source),
                        nvrhi::BindingSetItem::Sampler(0, scaled ? p.linear : p.point),
                    };
                    if (p.sets.size() > 32)
                    {
                        p.sets.clear();
                    }
                    p.sets.emplace_back(std::make_pair(source, scaled), dev->createBindingSet(desc, g_blitLayout));
                    set = p.sets.back().second;
                }
                BlitConstants c = {};
                c.dst[0] = -1.0f;
                c.dst[1] = 1.0f;
                c.dst[2] = 1.0f;
                c.dst[3] = -1.0f;
                c.src[2] = 1.0f;
                c.src[3] = 1.0f;
                p.commands->open();
                p.commands->writeBuffer(p.constants, &c, sizeof(c));
                nvrhi::GraphicsState state;
                state.pipeline = pipeline;
                state.framebuffer = fb;
                state.bindings = {set};
                state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(w), static_cast<float>(h)));
                p.commands->setGraphicsState(state);
                p.commands->draw(nvrhi::DrawArguments().setVertexCount(6));
                p.commands->close();
                dev->executeCommandList(p.commands);
                Rhi::present();
            }

            void renderThread()
            {
                Presenter p;
                nvrhi::IDevice* dev = Rhi::device();
                p.commands = dev->createCommandList();
                p.constants = dev->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(BlitConstants), "DDrawRhi present", 16));
                p.linear = dev->createSampler(nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
                p.point = dev->createSampler(nvrhi::SamplerDesc().setAllFilters(false).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
                for (;;)
                {
                    Job job;
                    {
                        std::unique_lock lock(g_jobMutex);
                        g_jobCv.wait(lock, [] { return !g_jobs.empty(); });
                        job = std::move(g_jobs.front());
                        g_jobs.pop_front();
                    }
                    if (job.list)
                    {
                        dev->executeCommandList(job.list);
                        nvrhi::EventQueryHandle query;
                        {
                            std::scoped_lock lock(g_jobMutex);
                            if (!g_freeQueries.empty())
                            {
                                query = std::move(g_freeQueries.back());
                                g_freeQueries.pop_back();
                            }
                        }
                        if (query)
                        {
                            dev->resetEventQuery(query);
                        }
                        else
                        {
                            query = dev->createEventQuery();
                        }
                        dev->setEventQuery(query, nvrhi::CommandQueue::Graphics);
                        std::scoped_lock lock(g_jobMutex);
                        g_executed.push_back({job.serial, std::move(query)});
                        g_idleLists.push_back(std::move(job.list));
                    }
                    if (job.present)
                    {
                        show(p, job.present);
                    }
                    if (job.idle)
                    {
                        dev->waitForIdle();
                    }
                    {
                        std::scoped_lock lock(g_jobMutex);
                        g_doneSerial = job.serial;
                        if (job.present)
                        {
                            --g_pendingPresents;
                        }
                        if (job.idle)
                        {
                            g_gpuDoneSerial = std::max(g_gpuDoneSerial, job.serial);
                            for (Executed& e : g_executed)
                            {
                                g_freeQueries.push_back(std::move(e.query));
                            }
                            g_executed.clear();
                        }
                    }
                    g_doneCv.notify_all();
                }
            }
        }

        Counters& counters()
        {
            static Counters c;
            return c;
        }

        bool ready()
        {
            std::scoped_lock lock(g_lock);
            return g_tried && g_available;
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
            if (g_available)
            {
                g_renderThread = std::thread(&renderThread);
                SetThreadDescription(g_renderThread.native_handle(), L"SacredBild render");
                g_renderThread.detach();    // lives as long as the process
                LOG("NVRHI backend: command lists are executed and frames presented by one render thread");
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

        PendingFn g_pendingFn = nullptr;
        bool g_inPending = false;

        void runPending()
        {
            if (g_pendingFn && !g_inPending)
            {
                g_inPending = true;
                g_pendingFn();
                g_inPending = false;
            }
        }

        void setPendingFlush(PendingFn fn)
        {
            g_pendingFn = fn;
        }

        namespace
        {
            void runPendingFwd()
            {
                runPending();
            }
        }

        nvrhi::ICommandList* commands()
        {
            runPending();
            return commandsKeepPending();
        }

        nvrhi::ICommandList* commandsKeepPending()
        {
            if (!g_open)
            {
                if (!g_commands)
                {
                    std::scoped_lock lock(g_jobMutex);
                    if (!g_idleLists.empty())
                    {
                        g_commands = std::move(g_idleLists.back());
                        g_idleLists.pop_back();
                    }
                }
                if (!g_commands)
                {
                    // Uploads (texture writes) come from 8 MB chunks: with NVRHI's 64 KB every texture streamed in larger than
                    // that got an upload buffer of its own, made on the spot (a sector brings ~6 MB).
                    g_commands = Rhi::device()->createCommandList(nvrhi::CommandListParameters().setUploadChunkSize(8u << 20));
                }
                g_commands->open();
                g_open = true;
                ++g_epoch;
                gpuTimerBegin(g_commands);
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
            if (!wait)
            {
                if (g_open)
                {
                    submit(nullptr, false);
                }
                return;
            }
            ++counters().waits;
            waitDone(submit(nullptr, true));
            // Everything recorded so far is done: the ring is free.
            g_ringFrames.clear();
            g_frameStart = g_head;
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
                // The GPU's progress is asked for only when the ring looks full: asking costs a call into the
                // driver (a kernel call on some), and uploads come with nearly every draw.
                uint64_t oldest = g_ringFrames.empty() ? g_frameStart : g_ringFrames.front().start;
                if (pos + bytes - oldest > kRingSize)
                {
                    while (!g_ringFrames.empty() && gpuDone(g_ringFrames.front().frame, false))
                    {
                        g_ringFrames.pop_front();
                    }
                    oldest = g_ringFrames.empty() ? g_frameStart : g_ringFrames.front().start;
                }
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
                    gpuDone(g_ringFrames.front().frame, true);
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
            g_copySets.erase(texture);
            g_copySets.erase(reinterpret_cast<const uint8_t*>(texture) + 1);
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

        nvrhi::ITexture* scratchTexture(uint32_t width, uint32_t height, nvrhi::Format format)
        {
            const uint64_t key = static_cast<uint64_t>(width) << 40 | static_cast<uint64_t>(height) << 16 | static_cast<uint32_t>(format);
            if (const auto it = g_scratch.find(key); it != g_scratch.end())
            {
                return it->second;
            }
            const uint64_t bytes = static_cast<uint64_t>(width) * height * 4;
            if (g_scratchBytes + bytes > kScratchBytes)
            {
                // Sizes the game no longer uses pile up: start again (what the open command list uses stays alive in it).
                for (auto& [k, texture] : g_scratch)
                {
                    forget(texture);
                }
                g_scratch.clear();
                g_scratchBytes = 0;
            }
            ++counters().tempTextures;
            nvrhi::TextureHandle texture = Rhi::device()->createTexture(nvrhi::TextureDesc().setWidth(width).setHeight(height)
                .setFormat(format).setDebugName("copy source").setInitialState(nvrhi::ResourceStates::ShaderResource)
                .setKeepInitialState(true).setSharesMemory(true));     // written whole each time before it is read
            if (texture)
            {
                g_scratch.emplace(key, texture);
                g_scratchBytes += bytes;
            }
            return texture;
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
            const void* setKey = reinterpret_cast<const uint8_t*>(src) + (linear ? 1 : 0);
            nvrhi::BindingSetHandle bindings;
            {
                std::scoped_lock lock(g_lock);
                if (const auto it = g_copySets.find(setKey); it != g_copySets.end())
                {
                    bindings = it->second;
                }
            }
            if (!bindings)
            {
                nvrhi::BindingSetDesc set;
                set.bindings = {
                    nvrhi::BindingSetItem::ConstantBuffer(0, g_blitConstants),
                    nvrhi::BindingSetItem::Texture_SRV(0, src),
                    nvrhi::BindingSetItem::Sampler(0, sampler(D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, linear, linear, D3DTFP_NONE, 0)),
                };
                bindings = Rhi::device()->createBindingSet(set, g_blitLayout);
                std::scoped_lock lock(g_lock);
                g_copySets[setKey] = bindings;
            }
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
                desc.inputLayout = g_uiInputs[format == UiVertex::Rhw ? 0 : 1];
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
            c.pixelOffset[0] = c.pixelOffset[1] = format == UiVertex::ImGui ? 0.0f : 0.5f;
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
            const nvrhi::TextureDesc& fd = frame->getDesc();
            RECT src = {0, 0, static_cast<LONG>(fd.width), static_cast<LONG>(fd.height)};
            if (srcRect)
            {
                IntersectRect(&src, srcRect, &src);
            }
            const uint32_t w = static_cast<uint32_t>(src.right - src.left), h = static_cast<uint32_t>(src.bottom - src.top);
            if (!w || !h)
            {
                flush(false);
                return DD_OK;
            }
            // The frame so far, copied into the next present texture (the game draws its next frame into the same target
            // right away), the overlay over it.
            nvrhi::TextureHandle& target = g_presentTextures[g_presentNext];
            g_presentNext = (g_presentNext + 1) % kPresentTextures;
            if (!target || target->getDesc().width != w || target->getDesc().height != h)
            {
                if (target)
                {
                    flush(true);    // the render thread may still show it
                    forget(target);
                }
                target = Rhi::device()->createTexture(nvrhi::TextureDesc().setWidth(w).setHeight(h)
                    .setFormat(nvrhi::Format::BGRA8_UNORM).setIsRenderTarget(true).setDebugName("presented frame")
                    .setInitialState(nvrhi::ResourceStates::ShaderResource).setKeepInitialState(true));
                if (!target)
                {
                    flush(false);
                    return DDERR_GENERIC;
                }
            }
            nvrhi::TextureSlice from;
            from.x = static_cast<uint32_t>(src.left);
            from.y = static_cast<uint32_t>(src.top);
            from.width = w;
            from.height = h;
            nvrhi::TextureSlice to;
            to.width = w;
            to.height = h;
            commands()->copyTexture(target, to, frame, from);
            if (g_overlayActive && g_overlayDraw && g_overlayActive())
            {
                g_overlayDraw(target, g_window);
            }
            const uint64_t serial = submit(target, false);
            ringFrameEnded(serial);
            gpuTimerFrame();
            // No more frames queued than the frame latency allows: the game waits here as it waited in Present.
            const int limit = std::clamp(Config::display.maxFrameLatency, 1, 3) + 1;
            std::unique_lock jobs(g_jobMutex);
            g_doneCv.wait(jobs, [&] { return g_pendingPresents <= limit - 1; });
            return DD_OK;
        }
    }
}
