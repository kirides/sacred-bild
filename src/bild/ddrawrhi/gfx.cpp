// SacredBild_GfxGet (core/bild_gfx.h) on the NVRHI backend: textures and meshes for other modules' 2D drawing, and a
// device context that records their triangles straight into the frame with shaders/gfx.hlsl, mapped into the UI canvas
// as the device proxy maps the game's UI draws.
//
// Every draw is a range of one batch: the batch's vertices and indices go into the upload ring once, its texture,
// blending, filtering and scissor are one pipeline state, and each draw's placement (a matrix to the target's pixels,
// the image's rectangle, the coordinate flags) is a push constant. Meshes are copied into the batch as they are; the
// GPU places them. Immediate draws are mapped on the CPU (rectangles get the proxy's UI quad fixes) and drawn with an
// identity placement.
#include "bild_gfx.h"
#include "ddrawrhi/context.h"
#include "ddrawrhi/device.h"
#include "ddrawrhi/surface.h"
#include "render/device_proxy.h"
#include "render/ui_quads.h"
#include "settings/config/ddraw.h"
#include "settings/config/render.h"
#include "settings/config/ui.h"
#include "ui/ui_canvas.h"
#include "ui/ui_windows.h"
#include "rhi/rhi.h"
#include "log.h"

#include "ddrawrhi/shaders/gfx_shaders.inc"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
    using Vertex = SacredBild_GfxVertex;
    static_assert(sizeof(Vertex) == 20);

    // ---- Resources ----

    struct Texture
    {
        nvrhi::TextureHandle texture;
        uint32_t format = 0;
    };

    struct Mesh
    {
        std::vector<Vertex> vertices;
        std::vector<int> indices;
        float minX = 0, minY = 0, maxX = 0, maxY = 0;   // the vertices' bounds (culling)
    };

    std::recursive_mutex g_resources;   // drawMesh holds it while its batch may flush (and look a texture up)
    std::unordered_map<uint32_t, Texture> g_textures;
    std::unordered_map<uint32_t, Mesh> g_meshes;
    std::atomic<uint32_t> g_nextTexture{1}, g_nextMesh{1};
    std::atomic<uint32_t> g_draws{0};

    nvrhi::Format gpuFormat(uint32_t format)
    {
        switch (format)
        {
        case SACREDBILD_GFX_ARGB8: return nvrhi::Format::BGRA8_UNORM;
        case SACREDBILD_GFX_BC1: return nvrhi::Format::BC1_UNORM;
        case SACREDBILD_GFX_BC2: return nvrhi::Format::BC2_UNORM;
        case SACREDBILD_GFX_BC3: return nvrhi::Format::BC3_UNORM;
        default: return nvrhi::Format::UNKNOWN;
        }
    }

    nvrhi::TextureHandle lookupTexture(uint32_t id)
    {
        std::scoped_lock lock(g_resources);
        const auto it = g_textures.find(id);
        return it == g_textures.end() ? nullptr : it->second.texture;
    }

    uint32_t __cdecl textureCreate(int width, int height, uint32_t format, const void* pixels, int pitch)
    {
        const nvrhi::Format f = gpuFormat(format);
        if (width <= 0 || height <= 0 || f == nvrhi::Format::UNKNOWN || !pixels || pitch <= 0 || !DDrawRhi::Context::ready())
        {
            return 0;
        }
        if (format != SACREDBILD_GFX_ARGB8 && (width % 4 || height % 4))
        {
            return 0;
        }
        nvrhi::TextureHandle texture = Rhi::device()->createTexture(nvrhi::TextureDesc()
            .setWidth(static_cast<uint32_t>(width)).setHeight(static_cast<uint32_t>(height)).setFormat(f)
            .setDebugName("gfx texture").setInitialState(nvrhi::ResourceStates::ShaderResource).setKeepInitialState(true));
        if (!texture)
        {
            return 0;
        }
        {
            std::scoped_lock lock(DDrawRhi::Context::lock());
            DDrawRhi::Context::commands()->writeTexture(texture, 0, 0, pixels, static_cast<size_t>(pitch));
        }
        const uint32_t id = g_nextTexture++;
        std::scoped_lock lock(g_resources);
        g_textures[id] = {texture, format};
        return id;
    }

    int __cdecl textureUpdate(uint32_t id, int x, int y, int width, int height, const void* pixels, int pitch)
    {
        uint32_t format = 0;
        nvrhi::TextureHandle texture;
        {
            std::scoped_lock lock(g_resources);
            const auto it = g_textures.find(id);
            if (it == g_textures.end())
            {
                return 0;
            }
            texture = it->second.texture;
            format = it->second.format;
        }
        const nvrhi::TextureDesc& d = texture->getDesc();
        if (format != SACREDBILD_GFX_ARGB8 || !pixels || width <= 0 || height <= 0 || x < 0 || y < 0 ||
            static_cast<uint32_t>(x + width) > d.width || static_cast<uint32_t>(y + height) > d.height)
        {
            return 0;
        }
        // NVRHI writes whole subresources: the rectangle goes to a texture of its size and is copied over.
        std::scoped_lock lock(DDrawRhi::Context::lock());
        nvrhi::ITexture* part = DDrawRhi::Context::scratchTexture(static_cast<uint32_t>(width), static_cast<uint32_t>(height), d.format);
        if (!part)
        {
            return 0;
        }
        nvrhi::ICommandList* cmd = DDrawRhi::Context::commands();
        cmd->writeTexture(part, 0, 0, pixels, static_cast<size_t>(pitch));
        nvrhi::TextureSlice to;
        to.x = static_cast<uint32_t>(x);
        to.y = static_cast<uint32_t>(y);
        to.width = static_cast<uint32_t>(width);
        to.height = static_cast<uint32_t>(height);
        cmd->copyTexture(texture, to, part, nvrhi::TextureSlice());
        return 1;
    }

    void __cdecl textureRelease(uint32_t id)
    {
        nvrhi::TextureHandle texture;
        {
            std::scoped_lock lock(g_resources);
            const auto it = g_textures.find(id);
            if (it == g_textures.end())
            {
                return;
            }
            texture = it->second.texture;
            g_textures.erase(it);
        }
        // What was recorded holds its own reference; the binding sets made for it go.
        DDrawRhi::Context::forget(texture);
    }

    uint32_t __cdecl meshCreate(const Vertex* vertices, int vertexCount, const int* indices, int indexCount)
    {
        if (!vertices || vertexCount <= 0 || vertexCount > 0xFFFF || indexCount < 0 || (indexCount && !indices))
        {
            return 0;
        }
        Mesh m;
        m.vertices.assign(vertices, vertices + vertexCount);
        if (indices)
        {
            m.indices.assign(indices, indices + indexCount);
        }
        else
        {
            m.indices.resize(static_cast<size_t>(vertexCount / 3 * 3));
            for (size_t i = 0; i < m.indices.size(); ++i)
            {
                m.indices[i] = static_cast<int>(i);
            }
        }
        for (int i : m.indices)
        {
            if (i < 0 || i >= vertexCount)
            {
                return 0;
            }
        }
        m.minX = m.maxX = vertices[0].x;
        m.minY = m.maxY = vertices[0].y;
        for (const Vertex& v : m.vertices)
        {
            m.minX = std::min(m.minX, v.x);
            m.maxX = std::max(m.maxX, v.x);
            m.minY = std::min(m.minY, v.y);
            m.maxY = std::max(m.maxY, v.y);
        }
        const uint32_t id = g_nextMesh++;
        std::scoped_lock lock(g_resources);
        g_meshes[id] = std::move(m);
        return id;
    }

    void __cdecl meshRelease(uint32_t id)
    {
        std::scoped_lock lock(g_resources);
        g_meshes.erase(id);
    }

    // ---- The GPU side ----

    struct DrawConstants
    {
        float toPixels[16];     // column-major
        float uvRect[4];
        float targetScale[2];
        uint32_t flags;
        float pad;
    };
    static_assert(sizeof(DrawConstants) == 96);

    struct Gpu
    {
        int ready = 0;          // 0 not tried, 1 ready, -1 unavailable
        nvrhi::ShaderHandle vs, ps;
        nvrhi::InputLayoutHandle inputs;
        nvrhi::BindingLayoutHandle layout;
        nvrhi::SamplerHandle samplers[2];   // linear, point
        std::vector<std::pair<std::pair<nvrhi::Format, bool>, nvrhi::GraphicsPipelineHandle>> pipelines;
        std::unordered_map<const void*, nvrhi::BindingSetHandle> sets;  // by texture (+1 for point sampling)
    };
    Gpu g_gpu;     // Context lock

    void forgetTexture(nvrhi::ITexture* texture)
    {
        g_gpu.sets.erase(texture);
        g_gpu.sets.erase(reinterpret_cast<const uint8_t*>(texture) + 1);
    }

    bool gpuReady()
    {
        if (g_gpu.ready)
        {
            return g_gpu.ready > 0;
        }
        g_gpu.ready = -1;
        nvrhi::IDevice* dev = Rhi::device();
        g_gpu.vs = Rhi::createShader(nvrhi::ShaderType::Vertex, "vsMain", kGfxVSDxil, kGfxVSSpirv);
        g_gpu.ps = Rhi::createShader(nvrhi::ShaderType::Pixel, "psMain", kGfxPSDxil, kGfxPSSpirv);
        const nvrhi::VertexAttributeDesc attributes[] = {
            nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(0).setElementStride(20),
            nvrhi::VertexAttributeDesc().setName("TEXA").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(8).setElementStride(20),
            nvrhi::VertexAttributeDesc().setName("DIFFUSE").setFormat(nvrhi::Format::R32_UINT).setOffset(16).setElementStride(20),
        };
        g_gpu.inputs = g_gpu.vs ? dev->createInputLayout(attributes, 3) : nullptr;
        nvrhi::BindingLayoutDesc layout;
        layout.visibility = nvrhi::ShaderType::All;
        layout.bindings = {
            nvrhi::BindingLayoutItem::PushConstants(0, sizeof(DrawConstants)),
            nvrhi::BindingLayoutItem::Texture_SRV(0),
            nvrhi::BindingLayoutItem::Sampler(0),
        };
        g_gpu.layout = dev->createBindingLayout(layout);
        g_gpu.samplers[0] = dev->createSampler(nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
        g_gpu.samplers[1] = dev->createSampler(nvrhi::SamplerDesc().setAllFilters(false).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
        if (!g_gpu.vs || !g_gpu.ps || !g_gpu.inputs || !g_gpu.layout || !g_gpu.samplers[0] || !g_gpu.samplers[1])
        {
            LOG("NVRHI backend: bild_gfx's shaders or layouts could not be created");
            return false;
        }
        DDrawRhi::Context::onForget(&forgetTexture);
        g_gpu.ready = 1;
        return true;
    }

    nvrhi::IGraphicsPipeline* pipeline(nvrhi::IFramebuffer* fb, bool premultiplied)
    {
        const nvrhi::FramebufferInfoEx& info = fb->getFramebufferInfo();
        const auto key = std::make_pair(info.colorFormats[0], premultiplied);
        for (auto& [k, p] : g_gpu.pipelines)
        {
            if (k == key)
            {
                return p;
            }
        }
        nvrhi::GraphicsPipelineDesc desc;
        desc.VS = g_gpu.vs;
        desc.PS = g_gpu.ps;
        desc.inputLayout = g_gpu.inputs;
        desc.primType = nvrhi::PrimitiveType::TriangleList;
        desc.bindingLayouts = {g_gpu.layout};
        desc.renderState.rasterState.setCullNone().setScissorEnable(true);
        desc.renderState.depthStencilState.setDepthTestEnable(false).setDepthWriteEnable(false);
        desc.renderState.blendState.targets[0].setBlendEnable(true)
            .setSrcBlend(premultiplied ? nvrhi::BlendFactor::One : nvrhi::BlendFactor::SrcAlpha)
            .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha).setSrcBlendAlpha(nvrhi::BlendFactor::One)
            .setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha);
        g_gpu.pipelines.emplace_back(key, Rhi::device()->createGraphicsPipeline(desc, info));
        return g_gpu.pipelines.back().second;
    }

    nvrhi::IBindingSet* bindings(nvrhi::ITexture* texture, bool linear)
    {
        nvrhi::BindingSetHandle& set = g_gpu.sets[reinterpret_cast<const uint8_t*>(texture) + (linear ? 0 : 1)];
        if (!set)
        {
            nvrhi::BindingSetDesc desc;
            desc.bindings = {
                nvrhi::BindingSetItem::PushConstants(0, sizeof(DrawConstants)),
                nvrhi::BindingSetItem::Texture_SRV(0, texture),
                nvrhi::BindingSetItem::Sampler(0, g_gpu.samplers[linear ? 0 : 1]),
            };
            set = Rhi::device()->createBindingSet(desc, g_gpu.layout);
        }
        return set;
    }

    // ---- The device context (per drawing thread) ----

    struct Draw
    {
        uint32_t firstIndex, indexCount;
        DrawConstants constants;
    };

    struct Context
    {
        bool active = false;
        nvrhi::ITexture* target = nullptr;
        float targetW = 0, targetH = 0;
        // The canvas mapping and the frame's clip (physical pixels).
        bool canvas = false;
        UiCanvas::Placement place{};
        bool confine = false;
        // State.
        bool scissor = false;
        float scissorRect[4] = {};      // canvas units: x, y, w, h
        bool transform = false;
        float matrix[16] = {};
        uint32_t blend = SACREDBILD_GFX_ALPHA, filter = SACREDBILD_GFX_LINEAR;
        // The pending batch: one texture, blending, filtering and scissor.
        std::vector<Vertex> vertices;
        std::vector<uint16_t> indices;
        std::vector<Draw> draws;
        uint32_t texture = 0;
        uint32_t batchBlend = 0, batchFilter = 0;
        RECT batchScissor{};
        // The device's surfaces drawn with since begin (surfaceTexture ids 0x80000000 | index + 1), referenced until end.
        std::vector<IDirectDrawSurface7*> surfaces;
    };
    thread_local Context t;

    constexpr uint32_t kSurfaceBit = 0x80000000u;

    DDrawRhi::Surface* surfaceOf(uint32_t id)
    {
        const uint32_t index = (id & ~kSurfaceBit) - 1;
        return index < t.surfaces.size() ? DDrawRhi::Surface::from(t.surfaces[index]) : nullptr;
    }

    // The texture's size in texels.
    bool textureSize(uint32_t id, float& w, float& h)
    {
        if (id & kSurfaceBit)
        {
            DDrawRhi::Surface* s = surfaceOf(id);
            if (!s)
            {
                return false;
            }
            w = static_cast<float>(s->width());
            h = static_cast<float>(s->height());
            return w > 0 && h > 0;
        }
        nvrhi::TextureHandle texture = lookupTexture(id);
        if (!texture)
        {
            return false;
        }
        w = static_cast<float>(texture->getDesc().width);
        h = static_cast<float>(texture->getDesc().height);
        return true;
    }

    RECT scissorPixels()
    {
        RECT r = {0, 0, static_cast<LONG>(t.targetW), static_cast<LONG>(t.targetH)};
        if (t.scissor)
        {
            float x0 = t.scissorRect[0], y0 = t.scissorRect[1];
            float x1 = x0 + t.scissorRect[2], y1 = y0 + t.scissorRect[3];
            if (t.canvas)
            {
                x0 = x0 * t.place.scale + t.place.originX;
                x1 = x1 * t.place.scale + t.place.originX;
                y0 = y0 * t.place.scale + t.place.originY;
                y1 = y1 * t.place.scale + t.place.originY;
            }
            RECT s = {static_cast<LONG>(std::floor(x0 + 0.5f)), static_cast<LONG>(std::floor(y0 + 0.5f)),
                static_cast<LONG>(std::floor(x1 + 0.5f)), static_cast<LONG>(std::floor(y1 + 0.5f))};
            IntersectRect(&r, &r, &s);
        }
        return r;
    }

    void flushBatch()
    {
        if (t.draws.empty())
        {
            t.vertices.clear();
            t.indices.clear();
            return;
        }
        nvrhi::TextureHandle texture = t.texture && !(t.texture & kSurfaceBit) ? lookupTexture(t.texture) : nullptr;
        if (DeviceProxy* proxy = DeviceProxy::instance())
        {
            proxy->syncBatch();     // the game's batched draws before these
        }
        {
            std::scoped_lock lock(DDrawRhi::Context::lock());
            if (t.texture & kSurfaceBit)
            {
                // The surface's texture, with what was written into it through the CPU uploaded first.
                DDrawRhi::Surface* s = surfaceOf(t.texture);
                texture = s ? s->texture() : nullptr;
            }
            const uint32_t vb = static_cast<uint32_t>(t.vertices.size() * sizeof(Vertex));
            const uint32_t ib = static_cast<uint32_t>((t.indices.size() * sizeof(uint16_t) + 3) & ~3u);
            DDrawRhi::Context::Upload v{}, i{};
            nvrhi::IFramebuffer* fb = gpuReady() ? DDrawRhi::Context::framebuffer(t.target, nullptr) : nullptr;
            nvrhi::IGraphicsPipeline* pso = fb ? pipeline(fb, t.batchBlend == SACREDBILD_GFX_PREMULTIPLIED) : nullptr;
            if (pso && DDrawRhi::Context::upload(vb, v) && DDrawRhi::Context::upload(ib, i))
            {
                std::memcpy(v.data, t.vertices.data(), vb);
                std::memcpy(i.data, t.indices.data(), t.indices.size() * sizeof(uint16_t));
                nvrhi::GraphicsState state;
                state.pipeline = pso;
                state.framebuffer = fb;
                state.bindings = {bindings(texture ? texture.Get() : DDrawRhi::Context::whiteTexture(), t.batchFilter == SACREDBILD_GFX_LINEAR)};
                state.vertexBuffers = {nvrhi::VertexBufferBinding().setBuffer(v.buffer).setSlot(0).setOffset(v.offset)};
                state.indexBuffer = nvrhi::IndexBufferBinding().setBuffer(i.buffer).setFormat(nvrhi::Format::R16_UINT).setOffset(i.offset);
                state.viewport.addViewport(nvrhi::Viewport(t.targetW, t.targetH));
                const RECT& s = t.batchScissor;
                state.viewport.addScissorRect(nvrhi::Rect(s.left, s.right, s.top, s.bottom));
                nvrhi::ICommandList* cmd = DDrawRhi::Context::commands();
                cmd->setGraphicsState(state);
                for (const Draw& d : t.draws)
                {
                    cmd->setPushConstants(&d.constants, sizeof(d.constants));
                    cmd->drawIndexed(nvrhi::DrawArguments().setVertexCount(d.indexCount).setStartIndexLocation(d.firstIndex));
                }
                if (g_draws++ == 0)
                {
                    LOG("NVRHI backend: first 2D draw through bild_gfx ({} draws, {} triangles)", t.draws.size(), t.indices.size() / 3);
                }
            }
        }
        t.vertices.clear();
        t.indices.clear();
        t.draws.clear();
    }

    // The pending batch, ended if this draw needs another state; false if nothing of it can be drawn.
    bool prepare(uint32_t texture, size_t vertexCount, RECT& scissor)
    {
        if (!t.active || !vertexCount || vertexCount > 0xFFFF)
        {
            return false;
        }
        scissor = scissorPixels();
        if (scissor.right <= scissor.left || scissor.bottom <= scissor.top)
        {
            return false;
        }
        if (!t.draws.empty() && (texture != t.texture || t.blend != t.batchBlend || t.filter != t.batchFilter ||
            !EqualRect(&scissor, &t.batchScissor) || t.vertices.size() + vertexCount > 0xFFFF))
        {
            flushBatch();
        }
        return true;
    }

    void addDraw(uint32_t texture, const RECT& scissor, size_t base, const int* indices, size_t indexCount, const DrawConstants& constants)
    {
        const size_t first = t.indices.size();
        t.indices.resize(first + indexCount);
        for (size_t k = 0; k < indexCount; ++k)
        {
            t.indices[first + k] = static_cast<uint16_t>(base + static_cast<size_t>(indices[k]));
        }
        t.draws.push_back({static_cast<uint32_t>(first), static_cast<uint32_t>(indexCount), constants});
        t.texture = texture;
        t.batchBlend = t.blend;
        t.batchFilter = t.filter;
        t.batchScissor = scissor;
    }

    void identityConstants(DrawConstants& c)
    {
        std::memset(&c, 0, sizeof(c));
        c.toPixels[0] = c.toPixels[5] = c.toPixels[10] = c.toPixels[15] = 1.0f;
        c.uvRect[2] = c.uvRect[3] = 1.0f;
        c.targetScale[0] = 2.0f / t.targetW;
        c.targetScale[1] = -2.0f / t.targetH;
    }

    // a * b (column-major 4x4).
    void multiply(const float* a, const float* b, float* out)
    {
        float r[16];
        for (int col = 0; col < 4; ++col)
        {
            for (int row = 0; row < 4; ++row)
            {
                r[col * 4 + row] = a[row] * b[col * 4] + a[4 + row] * b[col * 4 + 1] + a[8 + row] * b[col * 4 + 2] +
                    a[12 + row] * b[col * 4 + 3];
            }
        }
        std::memcpy(out, r, sizeof(r));
    }

    // Immediate triangles: mapped to pixels here (rectangles get the proxy's UI quad fixes), drawn with an identity
    // placement.
    void append(const Vertex* vertices, size_t vertexCount, const int* indices, size_t indexCount, uint32_t texture)
    {
        RECT scissor;
        if (!indexCount || !prepare(texture, vertexCount, scissor))
        {
            return;
        }
        const size_t base = t.vertices.size();
        t.vertices.resize(base + vertexCount);
        Vertex* out = &t.vertices[base];
        float vMinX = 1e30f, vMinY = 1e30f, vMaxX = -1e30f, vMaxY = -1e30f;
        float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
        for (size_t k = 0; k < vertexCount; ++k)
        {
            float x = vertices[k].x, y = vertices[k].y;
            if (t.transform)
            {
                const float* m = t.matrix;
                const float px = m[0] * x + m[4] * y + m[12];
                const float py = m[1] * x + m[5] * y + m[13];
                const float pw = m[3] * x + m[7] * y + m[15];
                const float w = pw != 0.0f ? pw : 1.0f;
                x = px / w;
                y = py / w;
            }
            vMinX = std::min(vMinX, x);
            vMaxX = std::max(vMaxX, x);
            vMinY = std::min(vMinY, y);
            vMaxY = std::max(vMaxY, y);
            if (t.canvas)
            {
                x = x * t.place.scale + t.place.originX;
                y = y * t.place.scale + t.place.originY;
            }
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);
            out[k] = {x, y, vertices[k].u, vertices[k].v, vertices[k].argb};
        }
        // A rectangle of 4 vertices gets the device proxy's fixes for UI quads.
        if (t.canvas && vertexCount == 4)
        {
            float tw = 0, th = 0;
            if (texture && textureSize(texture, tw, th))
            {
                UiQuads::fitTexels(reinterpret_cast<uint8_t*>(out), sizeof(Vertex), 8, tw, th, t.filter == SACREDBILD_GFX_LINEAR);
            }
            if (t.confine && !UiQuads::clip(reinterpret_cast<uint8_t*>(out), sizeof(Vertex), 8, 1, t.place))
            {
                t.vertices.resize(base);
                return;
            }
        }
        if (t.canvas)
        {
            UiWindows::noteDraw(vMinX, vMinY, vMaxX, vMaxY);
            // Windows parked outside the 1024x768 screen stay invisible (as DeviceProxy::mapToCanvas).
            if (t.confine && !(maxX > t.place.clipLeft && minX < t.place.clipRight && maxY > t.place.clipTop &&
                minY < t.place.clipBottom))
            {
                t.vertices.resize(base);
                return;
            }
        }
        DrawConstants c;
        identityConstants(c);
        addDraw(texture, scissor, base, indices, indexCount, c);
    }

    int __cdecl begin(IDirect3DDevice7* device)
    {
        if (t.active || !device)
        {
            return 0;
        }
        DeviceProxy* proxy = DeviceProxy::instance();
        IDirect3DDevice7* real = proxy && device == proxy ? proxy->real() : device;
        DDrawRhi::Device* d = DDrawRhi::Device::from(real);
        if (!d)
        {
            return 0;
        }
        nvrhi::ITexture* target = d->renderTarget();
        if (!target)
        {
            return 0;
        }
        t.active = true;
        t.target = target;
        t.targetW = static_cast<float>(target->getDesc().width);
        t.targetH = static_cast<float>(target->getDesc().height);
        t.canvas = proxy && device == proxy && proxy->uiMode();
        if (t.canvas)
        {
            t.place = UiCanvas::placement();
            t.confine = UiCanvas::frame().confine;
        }
        t.scissor = false;
        t.transform = false;
        t.blend = SACREDBILD_GFX_ALPHA;
        t.filter = SACREDBILD_GFX_LINEAR;
        t.vertices.clear();
        t.indices.clear();
        t.draws.clear();
        return 1;
    }

    void __cdecl flush()
    {
        if (t.active)
        {
            flushBatch();
        }
    }

    void __cdecl end()
    {
        if (!t.active)
        {
            return;
        }
        flushBatch();
        for (IDirectDrawSurface7* s : t.surfaces)
        {
            s->Release();
        }
        t.surfaces.clear();
        t.active = false;
        t.target = nullptr;
    }

    uint32_t __cdecl surfaceTexture(IDirectDrawSurface7* surface)
    {
        if (!t.active || !surface || !DDrawRhi::Surface::from(surface))
        {
            return 0;
        }
        for (size_t i = 0; i < t.surfaces.size(); ++i)
        {
            if (t.surfaces[i] == surface)
            {
                return kSurfaceBit | static_cast<uint32_t>(i + 1);
            }
        }
        surface->AddRef();
        t.surfaces.push_back(surface);
        return kSurfaceBit | static_cast<uint32_t>(t.surfaces.size());
    }

    void __cdecl setScissor(int enable, float x, float y, float w, float h)
    {
        t.scissor = enable != 0;
        t.scissorRect[0] = x;
        t.scissorRect[1] = y;
        t.scissorRect[2] = w;
        t.scissorRect[3] = h;
    }

    void __cdecl setTransform(const float* matrix)
    {
        t.transform = matrix != nullptr;
        if (matrix)
        {
            std::memcpy(t.matrix, matrix, sizeof(t.matrix));
        }
    }

    void __cdecl setState(uint32_t blend, uint32_t filter)
    {
        t.blend = blend == SACREDBILD_GFX_PREMULTIPLIED ? SACREDBILD_GFX_PREMULTIPLIED : SACREDBILD_GFX_ALPHA;
        // [UI] LinearFilter turns the UI's point sampling into bilinear, as the proxy does for the game's UI.
        t.filter = filter == SACREDBILD_GFX_POINT && !Config::ui.linearFilter ? SACREDBILD_GFX_POINT : SACREDBILD_GFX_LINEAR;
    }

    void __cdecl drawMeshEx(uint32_t id, const SacredBild_GfxMeshDraw* draw)
    {
        if (!t.active || !draw)
        {
            return;
        }
        std::scoped_lock lock(g_resources);
        const auto it = g_meshes.find(id);
        if (it == g_meshes.end() || it->second.indices.empty())
        {
            return;
        }
        const Mesh& m = it->second;
        // Mesh units -> canvas units: translate, the transform, the scale.
        float place[16] = {draw->scale, 0, 0, 0, 0, draw->scale, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        float translate[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, draw->x, draw->y, 0, 1};
        float toCanvas[16];
        if (t.transform)
        {
            float mt[16];
            multiply(t.matrix, translate, mt);
            multiply(place, mt, toCanvas);
        }
        else
        {
            multiply(place, translate, toCanvas);
        }
        // Culled as the proxy culls the game's UI draws: by the corners of the mesh's bounds.
        float vMinX = 1e30f, vMinY = 1e30f, vMaxX = -1e30f, vMaxY = -1e30f;
        for (const auto& [cx, cy] : {std::pair{m.minX, m.minY}, {m.maxX, m.minY}, {m.minX, m.maxY}, {m.maxX, m.maxY}})
        {
            const float px = toCanvas[0] * cx + toCanvas[4] * cy + toCanvas[12];
            const float py = toCanvas[1] * cx + toCanvas[5] * cy + toCanvas[13];
            const float pw = toCanvas[3] * cx + toCanvas[7] * cy + toCanvas[15];
            const float w = pw != 0.0f ? pw : 1.0f;
            vMinX = std::min(vMinX, px / w);
            vMaxX = std::max(vMaxX, px / w);
            vMinY = std::min(vMinY, py / w);
            vMaxY = std::max(vMaxY, py / w);
        }
        DrawConstants c;
        identityConstants(c);
        if (t.canvas)
        {
            UiWindows::noteDraw(vMinX, vMinY, vMaxX, vMaxY);
            const float s = t.place.scale, ox = t.place.originX, oy = t.place.originY;
            if (t.confine && !(vMaxX * s + ox > t.place.clipLeft && vMinX * s + ox < t.place.clipRight &&
                vMaxY * s + oy > t.place.clipTop && vMinY * s + oy < t.place.clipBottom))
            {
                return;
            }
            // x_pixels = (s * X + ox * W) / W: the canvas mapping folded into the matrix.
            const float canvas[16] = {s, 0, 0, 0, 0, s, 0, 0, 0, 0, 1, 0, ox, oy, 0, 1};
            multiply(canvas, toCanvas, c.toPixels);
        }
        else
        {
            std::memcpy(c.toPixels, toCanvas, sizeof(toCanvas));
        }
        c.uvRect[0] = draw->u0;
        c.uvRect[1] = draw->v0;
        c.uvRect[2] = draw->u1;
        c.uvRect[3] = draw->v1;
        c.flags = draw->flags & 63;
        RECT scissor;
        if (!prepare(draw->texture, m.vertices.size(), scissor))
        {
            return;
        }
        const size_t base = t.vertices.size();
        t.vertices.insert(t.vertices.end(), m.vertices.begin(), m.vertices.end());
        addDraw(draw->texture, scissor, base, m.indices.data(), m.indices.size(), c);
    }

    void __cdecl drawMesh(uint32_t id, float x, float y, uint32_t texture)
    {
        const SacredBild_GfxMeshDraw d = {x, y, 1.0f, texture, 0.0f, 0.0f, 1.0f, 1.0f, 0};
        drawMeshEx(id, &d);
    }

    void __cdecl draw(const Vertex* vertices, int vertexCount, const int* indices, int indexCount, uint32_t texture)
    {
        if (!t.active || !vertices || vertexCount <= 0)
        {
            return;
        }
        if (indices)
        {
            for (int k = 0; k < indexCount; ++k)
            {
                if (indices[k] < 0 || indices[k] >= vertexCount)
                {
                    return;
                }
            }
            append(vertices, static_cast<size_t>(vertexCount), indices, static_cast<size_t>(indexCount), texture);
            return;
        }
        thread_local std::vector<int> sequential;
        const size_t count = static_cast<size_t>(vertexCount / 3 * 3);
        if (sequential.size() < count)
        {
            const size_t from = sequential.size();
            sequential.resize(count);
            for (size_t k = from; k < count; ++k)
            {
                sequential[k] = static_cast<int>(k);
            }
        }
        append(vertices, static_cast<size_t>(vertexCount), sequential.data(), count, texture);
    }

    const SacredBild_Gfx g_table = {
        SACREDBILD_GFX_VERSION, sizeof(SacredBild_Gfx),
        &textureCreate, &textureUpdate, &textureRelease,
        &meshCreate, &meshRelease,
        &begin, &end, &flush,
        &setScissor, &setTransform, &setState,
        &drawMesh, &draw,
        &surfaceTexture,
        &drawMeshEx,
    };
}

extern "C" const SacredBild_Gfx* __cdecl SacredBild_GfxGet(uint32_t version)
{
    // Never makes the device: Gear asks early (possibly under the loader lock, where DXGI refuses to start; making
    // it there sent Direct3D 12 to its Vulkan fallback). begin() returns 0 until the game's device draws.
    if (version == 0 || version > SACREDBILD_GFX_VERSION || !Config::ddraw.nvrhi || !Config::render.gearGfx)
    {
        return nullptr;
    }
    return &g_table;
}
