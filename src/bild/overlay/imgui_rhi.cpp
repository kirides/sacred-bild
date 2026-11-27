#include "overlay/imgui_rhi.h"
#include "ddrawrhi/context.h"
#include "log.h"

#include <imgui.h>

#include <cstring>
#include <unordered_map>
#include <vector>

namespace ImGuiRhi
{
    namespace
    {
        // ImGui's textures by the id handed to ImGui (the NVRHI texture's address).
        std::unordered_map<ImTextureID, nvrhi::TextureHandle> g_textures;

        void upload(ImTextureData* tex, nvrhi::ITexture* texture)
        {
            const int w = tex->Width, h = tex->Height;
            std::vector<uint32_t> rgba;
            const void* pixels = tex->GetPixels();
            size_t pitch = static_cast<size_t>(tex->GetPitch());
            if (tex->Format == ImTextureFormat_Alpha8)
            {
                rgba.resize(size_t(w) * h);
                for (int y = 0; y < h; ++y)
                {
                    const auto* row = static_cast<const uint8_t*>(pixels) + size_t(y) * pitch;
                    for (int x = 0; x < w; ++x)
                    {
                        rgba[size_t(y) * w + x] = 0x00FFFFFFu | uint32_t(row[x]) << 24;
                    }
                }
                pixels = rgba.data();
                pitch = size_t(w) * 4;
            }
            DDrawRhi::Context::commands()->writeTexture(texture, 0, 0, pixels, pitch);
        }

        void update(ImTextureData* tex)
        {
            if (tex->Status == ImTextureStatus_WantCreate)
            {
                nvrhi::TextureHandle texture = DDrawRhi::Context::device()->createTexture(nvrhi::TextureDesc()
                    .setWidth(static_cast<uint32_t>(tex->Width)).setHeight(static_cast<uint32_t>(tex->Height))
                    .setFormat(nvrhi::Format::RGBA8_UNORM).setDebugName("ImGui texture")
                    .setInitialState(nvrhi::ResourceStates::ShaderResource).setKeepInitialState(true));
                if (!texture)
                {
                    LOG("Overlay: ImGui texture {}x{} could not be created", tex->Width, tex->Height);
                    return;
                }
                upload(tex, texture);
                const ImTextureID id = static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(texture.Get()));
                g_textures[id] = texture;
                tex->SetTexID(id);
                tex->SetStatus(ImTextureStatus_OK);
            }
            else if (tex->Status == ImTextureStatus_WantUpdates)
            {
                // The whole texture again: ImGui changes its atlas rarely (new glyphs).
                auto it = g_textures.find(tex->GetTexID());
                if (it != g_textures.end())
                {
                    upload(tex, it->second);
                }
                tex->SetStatus(ImTextureStatus_OK);
            }
            else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0)
            {
                auto it = g_textures.find(tex->GetTexID());
                if (it != g_textures.end())
                {
                    DDrawRhi::Context::forget(it->second);
                    g_textures.erase(it);
                }
                tex->SetTexID(ImTextureID_Invalid);
                tex->SetStatus(ImTextureStatus_Destroyed);
            }
        }
    }

    void init()
    {
        ImGuiIO& io = ImGui::GetIO();
        io.BackendRendererName = "SacredBild NVRHI";
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
    }

    void shutdown()
    {
        for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
        {
            if (tex->RefCount == 1)
            {
                tex->SetTexID(ImTextureID_Invalid);
                tex->SetStatus(ImTextureStatus_Destroyed);
            }
        }
        for (auto& [id, texture] : g_textures)
        {
            DDrawRhi::Context::forget(texture);
        }
        g_textures.clear();
    }

    void render(ImDrawData* data, nvrhi::ITexture* target)
    {
        if (!data || !target)
        {
            return;
        }
        if (data->Textures)
        {
            for (ImTextureData* tex : *data->Textures)
            {
                if (tex->Status != ImTextureStatus_OK)
                {
                    update(tex);
                }
            }
        }
        if (data->TotalVtxCount <= 0)
        {
            return;
        }
        static_assert(sizeof(ImDrawVert) == 20 && sizeof(ImDrawIdx) == 2);
        const ImVec2 origin = data->DisplayPos;
        const ImVec2 scale = data->FramebufferScale;
        for (const ImDrawList* list : data->CmdLists)
        {
            DDrawRhi::Context::Upload vertices, indices;
            if (!DDrawRhi::Context::upload(static_cast<uint32_t>(list->VtxBuffer.Size * sizeof(ImDrawVert)), vertices) ||
                !DDrawRhi::Context::upload(static_cast<uint32_t>(list->IdxBuffer.Size * sizeof(ImDrawIdx)), indices))
            {
                return;
            }
            std::memcpy(vertices.data, list->VtxBuffer.Data, list->VtxBuffer.Size * sizeof(ImDrawVert));
            std::memcpy(indices.data, list->IdxBuffer.Data, list->IdxBuffer.Size * sizeof(ImDrawIdx));
            for (const ImDrawCmd& cmd : list->CmdBuffer)
            {
                if (cmd.UserCallback)
                {
                    if (cmd.UserCallback != ImDrawCallback_ResetRenderState)
                    {
                        cmd.UserCallback(list, &cmd);
                    }
                    continue;
                }
                const RECT clip = {static_cast<LONG>((cmd.ClipRect.x - origin.x) * scale.x),
                    static_cast<LONG>((cmd.ClipRect.y - origin.y) * scale.y), static_cast<LONG>((cmd.ClipRect.z - origin.x) * scale.x),
                    static_cast<LONG>((cmd.ClipRect.w - origin.y) * scale.y)};
                auto* texture = reinterpret_cast<nvrhi::ITexture*>(static_cast<uintptr_t>(cmd.GetTexID()));
                DDrawRhi::Context::drawUi(target, DDrawRhi::Context::UiVertex::ImGui, false, texture, true, vertices, indices,
                    cmd.IdxOffset, cmd.ElemCount, cmd.VtxOffset, clip);
            }
        }
    }
}
