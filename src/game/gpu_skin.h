#pragma once
#include <windows.h>
#include <objbase.h>
#include <ddraw.h>
#include <d3d.h>

// [Render] GpuSkinning: characters and their shadows skinned in a vertex shader instead of by Granny on the CPU
// (~20 % of the render thread zoomed out). For Granny's rendering path (the model pass wants positions and normals, the
// shadow pass positions only) the deform only computes the bone matrices of the pose; this module keeps them, skins
// on the CPU just the vertices cGranny_render samples for its bounding box, and the following
// DrawIndexedPrimitiveStrided of those vertices is drawn by the Direct3D 9 backend from a static per-mesh vertex
// buffer (DDraw9::Skin). Wherever that can't be done (another backend, state the shader doesn't reproduce, UI mode)
// the vertices are skinned on the CPU after all and drawn as before. Picking keeps Granny's own deform.
namespace GpuSkin
{
    // Queues the hook in the caller's Patch transaction.
    void install();
    bool active();
    // Once per presented frame: with D3DStats, skeletons posed against skeletons drawn.
    void onFrame();

    // From DeviceProxy::DrawIndexedPrimitiveStrided. If the positions are vertices whose skinning this module left to
    // the GPU: with `gpu`, calls `prepare(context)` (apply what the batcher only recorded) and draws them through the
    // backend, returning true; otherwise, or if the backend declines, skins them on the CPU and returns false for the
    // caller to draw as usual. False right away for any other draw.
    bool draw(IDirect3DDevice7* real, bool gpu, void (*prepare)(void*), void* context, D3DPRIMITIVETYPE type, DWORD fvf,
        const D3DDRAWPRIMITIVESTRIDEDDATA& data, DWORD vertCount, const WORD* indices, DWORD indexCount);
}
