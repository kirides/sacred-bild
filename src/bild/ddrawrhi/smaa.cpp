// SMAA 1x on the NVRHI backend (smaa.h): three full-screen passes with the shaders of shaders/smaa.hlsl and the lookup
// textures of src/bild/ddraw9/smaa (AreaTex.h, SearchTex.h).
#include "ddrawrhi/smaa.h"
#include "ddrawrhi/context.h"
#include "ddrawrhi/device.h"
#include "rhi/rhi.h"
#include "log.h"

#include "ddrawrhi/shaders/smaa_shaders.inc"
#include "ddraw9/smaa/AreaTex.h"
#include "ddraw9/smaa/SearchTex.h"

#include <nvrhi/utils.h>

#include <algorithm>
#include <mutex>

namespace DDrawRhi::Smaa
{
    namespace
    {
        constexpr int kPresets = 3;

        struct State
        {
            int ready = 0;      // 0 not tried, 1 ready, -1 unavailable
            nvrhi::ShaderHandle edgeVS, blendVS, blendPS;
            nvrhi::ShaderHandle edgePS[kPresets], weightVS[kPresets], weightPS[kPresets];
            nvrhi::BindingLayoutHandle layout;
            nvrhi::BufferHandle constants;
            nvrhi::SamplerHandle linear, point;
            nvrhi::TextureHandle area, search;
            bool lookupsUploaded = false;

            // Per render target size and format.
            uint32_t width = 0, height = 0;
            nvrhi::Format format = nvrhi::Format::UNKNOWN;
            nvrhi::TextureHandle color, edges, weights;
            nvrhi::FramebufferHandle edgesFb, weightsFb;
            nvrhi::GraphicsPipelineHandle edgePipeline[kPresets], weightPipeline[kPresets], blendPipeline;
            nvrhi::BindingSetHandle edgeSet, weightSet, blendSet;
            nvrhi::ITexture* blendTarget = nullptr;
        };
        State g;

        bool init()
        {
            if (g.ready)
            {
                return g.ready > 0;
            }
            g.ready = -1;
            nvrhi::IDevice* dev = Context::device();
            using T = nvrhi::ShaderType;
            g.edgeVS = Rhi::createShader(T::Vertex, "EdgeVS", kSmaaEdgeVSDxil, kSmaaEdgeVSSpirv);
            g.blendVS = Rhi::createShader(T::Vertex, "BlendVS", kSmaaBlendVSDxil, kSmaaBlendVSSpirv);
            g.blendPS = Rhi::createShader(T::Pixel, "BlendPS", kSmaaBlendPSDxil, kSmaaBlendPSSpirv);
            g.edgePS[0] = Rhi::createShader(T::Pixel, "EdgePS", kSmaaEdgePSLowDxil, kSmaaEdgePSLowSpirv);
            g.edgePS[1] = Rhi::createShader(T::Pixel, "EdgePS", kSmaaEdgePSMediumDxil, kSmaaEdgePSMediumSpirv);
            g.edgePS[2] = Rhi::createShader(T::Pixel, "EdgePS", kSmaaEdgePSHighDxil, kSmaaEdgePSHighSpirv);
            g.weightVS[0] = Rhi::createShader(T::Vertex, "WeightVS", kSmaaWeightVSLowDxil, kSmaaWeightVSLowSpirv);
            g.weightVS[1] = Rhi::createShader(T::Vertex, "WeightVS", kSmaaWeightVSMediumDxil, kSmaaWeightVSMediumSpirv);
            g.weightVS[2] = Rhi::createShader(T::Vertex, "WeightVS", kSmaaWeightVSHighDxil, kSmaaWeightVSHighSpirv);
            g.weightPS[0] = Rhi::createShader(T::Pixel, "WeightPS", kSmaaWeightPSLowDxil, kSmaaWeightPSLowSpirv);
            g.weightPS[1] = Rhi::createShader(T::Pixel, "WeightPS", kSmaaWeightPSMediumDxil, kSmaaWeightPSMediumSpirv);
            g.weightPS[2] = Rhi::createShader(T::Pixel, "WeightPS", kSmaaWeightPSHighDxil, kSmaaWeightPSHighSpirv);
            nvrhi::BindingLayoutDesc layout;
            layout.visibility = nvrhi::ShaderType::All;
            layout.bindings = {
                nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
                nvrhi::BindingLayoutItem::Texture_SRV(0),
                nvrhi::BindingLayoutItem::Texture_SRV(1),
                nvrhi::BindingLayoutItem::Texture_SRV(2),
                nvrhi::BindingLayoutItem::Sampler(0),
                nvrhi::BindingLayoutItem::Sampler(1),
            };
            g.layout = dev->createBindingLayout(layout);
            g.constants = dev->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(16, "smaa", 64));
            g.linear = dev->createSampler(nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
            g.point = dev->createSampler(nvrhi::SamplerDesc().setAllFilters(false).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
            g.area = dev->createTexture(nvrhi::TextureDesc().setWidth(AREATEX_WIDTH).setHeight(AREATEX_HEIGHT)
                .setFormat(nvrhi::Format::RG8_UNORM).setDebugName("smaa area").setInitialState(nvrhi::ResourceStates::ShaderResource)
                .setKeepInitialState(true));
            g.search = dev->createTexture(nvrhi::TextureDesc().setWidth(SEARCHTEX_WIDTH).setHeight(SEARCHTEX_HEIGHT)
                .setFormat(nvrhi::Format::R8_UNORM).setDebugName("smaa search").setInitialState(nvrhi::ResourceStates::ShaderResource)
                .setKeepInitialState(true));
            bool ok = g.edgeVS && g.blendVS && g.blendPS && g.layout && g.constants && g.linear && g.point && g.area && g.search;
            for (int i = 0; i < kPresets; ++i)
            {
                ok = ok && g.edgePS[i] && g.weightVS[i] && g.weightPS[i];
            }
            if (!ok)
            {
                LOG("NVRHI backend: SMAA: creating its shaders or textures failed");
                return false;
            }
            g.ready = 1;
            LOG("NVRHI backend: SMAA ready");
            return true;
        }

        nvrhi::GraphicsPipelineHandle pipeline(nvrhi::IShader* vs, nvrhi::IShader* ps, nvrhi::IFramebuffer* fb)
        {
            nvrhi::GraphicsPipelineDesc desc;
            desc.VS = vs;
            desc.PS = ps;
            desc.primType = nvrhi::PrimitiveType::TriangleList;
            desc.bindingLayouts = {g.layout};
            desc.renderState.rasterState.setCullNone();
            desc.renderState.depthStencilState.setDepthTestEnable(false).setDepthWriteEnable(false);
            return Context::device()->createGraphicsPipeline(desc, fb->getFramebufferInfo());
        }

        nvrhi::BindingSetHandle bindings(nvrhi::ITexture* t0, nvrhi::ITexture* t1, nvrhi::ITexture* t2)
        {
            nvrhi::BindingSetDesc desc;
            desc.bindings = {
                nvrhi::BindingSetItem::ConstantBuffer(0, g.constants),
                nvrhi::BindingSetItem::Texture_SRV(0, t0),
                nvrhi::BindingSetItem::Texture_SRV(1, t1),
                nvrhi::BindingSetItem::Texture_SRV(2, t2),
                nvrhi::BindingSetItem::Sampler(0, g.linear),
                nvrhi::BindingSetItem::Sampler(1, g.point),
            };
            return Context::device()->createBindingSet(desc, g.layout);
        }

        // The per-size textures, pipelines and bindings for `target`.
        bool prepare(nvrhi::ITexture* target)
        {
            const nvrhi::TextureDesc& t = target->getDesc();
            if (g.width == t.width && g.height == t.height && g.format == t.format && g.blendTarget == target)
            {
                return true;
            }
            nvrhi::IDevice* dev = Context::device();
            auto rt = [&](nvrhi::Format format, const char* name, bool renderTarget) {
                return dev->createTexture(nvrhi::TextureDesc().setWidth(t.width).setHeight(t.height).setFormat(format)
                    .setIsRenderTarget(renderTarget).setDebugName(name)
                    .setInitialState(renderTarget ? nvrhi::ResourceStates::RenderTarget : nvrhi::ResourceStates::ShaderResource)
                    .setKeepInitialState(true));
            };
            for (nvrhi::ITexture* old : {g.color.Get(), g.edges.Get(), g.weights.Get()})
            {
                if (old)
                {
                    Context::forget(old);
                }
            }
            g.color = rt(t.format, "smaa color", false);
            g.edges = rt(nvrhi::Format::RG8_UNORM, "smaa edges", true);
            g.weights = rt(nvrhi::Format::RGBA8_UNORM, "smaa weights", true);
            if (!g.color || !g.edges || !g.weights)
            {
                LOG("NVRHI backend: SMAA: its {}x{} textures could not be created", t.width, t.height);
                g.width = g.height = 0;
                return false;
            }
            g.edgesFb = dev->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(g.edges));
            g.weightsFb = dev->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(g.weights));
            for (int i = 0; i < kPresets; ++i)
            {
                g.edgePipeline[i] = pipeline(g.edgeVS, g.edgePS[i], g.edgesFb);
                g.weightPipeline[i] = pipeline(g.weightVS[i], g.weightPS[i], g.weightsFb);
            }
            g.blendPipeline = pipeline(g.blendVS, g.blendPS, Context::framebuffer(target, nullptr));
            g.edgeSet = bindings(g.color, Context::whiteTexture(), Context::whiteTexture());
            g.weightSet = bindings(g.edges, g.area, g.search);
            g.blendSet = bindings(g.color, g.weights, Context::whiteTexture());
            g.width = t.width;
            g.height = t.height;
            g.format = t.format;
            g.blendTarget = target;
            return true;
        }

        void pass(nvrhi::ICommandList* cmd, nvrhi::IGraphicsPipeline* pso, nvrhi::IFramebuffer* fb, nvrhi::IBindingSet* set)
        {
            nvrhi::GraphicsState state;
            state.pipeline = pso;
            state.framebuffer = fb;
            state.bindings = {set};
            state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(g.width), static_cast<float>(g.height)));
            cmd->setGraphicsState(state);
            cmd->draw(nvrhi::DrawArguments().setVertexCount(3));
        }
    }

    bool apply(IDirect3DDevice7* device, int preset)
    {
        Device* d = Device::from(device);
        if (!d)
        {
            return false;
        }
        std::scoped_lock lock(Context::lock());
        nvrhi::ITexture* target = d->renderTarget();
        if (!target || !init() || !prepare(target))
        {
            return false;
        }
        preset = std::clamp(preset, 0, kPresets - 1);
        nvrhi::ICommandList* cmd = Context::commands();
        if (!g.lookupsUploaded)
        {
            cmd->writeTexture(g.area, 0, 0, areaTexBytes, AREATEX_PITCH);
            cmd->writeTexture(g.search, 0, 0, searchTexBytes, SEARCHTEX_PITCH);
            g.lookupsUploaded = true;
        }
        const float metrics[4] = {1.0f / g.width, 1.0f / g.height, static_cast<float>(g.width), static_cast<float>(g.height)};
        cmd->writeBuffer(g.constants, metrics, sizeof(metrics));
        cmd->copyTexture(g.color, nvrhi::TextureSlice(), target, nvrhi::TextureSlice());
        cmd->clearTextureFloat(g.edges, nvrhi::AllSubresources, nvrhi::Color(0.0f));
        cmd->clearTextureFloat(g.weights, nvrhi::AllSubresources, nvrhi::Color(0.0f));
        pass(cmd, g.edgePipeline[preset], g.edgesFb, g.edgeSet);
        pass(cmd, g.weightPipeline[preset], g.weightsFb, g.weightSet);
        pass(cmd, g.blendPipeline, Context::framebuffer(target, nullptr), g.blendSet);
        return true;
    }
}
