#pragma once

#include <nvrhi/nvrhi.h>

struct ImDrawData;

// Dear ImGui's renderer on the NVRHI backend ([DDraw] Backend=nvrhi): draw lists through DDrawRhi::Context::drawUi, and
// ImGui's textures (the font atlas) as NVRHI textures created and updated as ImGui asks (ImGuiBackendFlags_RendererHasTextures).
// Everything runs on the presenting thread with the Context lock held (the overlay is drawn from Context::present).
namespace ImGuiRhi
{
    // After ImGui::CreateContext: the backend flags and name.
    void init();
    void shutdown();
    // Into `target` (its pixels are ImGui's display coordinates).
    void render(ImDrawData* data, nvrhi::ITexture* target);
}
