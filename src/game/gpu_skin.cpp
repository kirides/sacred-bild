#include "game/gpu_skin.h"
#include "game/d3d_stats.h"
#include "game/granny_mesh.h"
#include "ddraw9/skin.h"
#include "config.h"
#include "log.h"
#include "patch.h"

#include <intrin.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    using namespace GrannyMesh;
    namespace Skin = DDraw9::Skin;

    constexpr uint32_t kInfluences = 4;
    constexpr DWORD kFvfModel = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1;          // 0x112
    constexpr DWORD kFvfModelDiffuse = kFvfModel | D3DFVF_DIFFUSE;                 // 0x152
    constexpr DWORD kFvfShadow = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1;        // 0x142: one color, stride 0

    GrannyMesh::DeformFn g_origDeform = nullptr;
    uintptr_t g_renderReturn = 0;   // the rendering path's deform call returns here
    bool g_active = false;
    std::atomic<bool> g_gpuUsable{true};    // false once the backend can't skin: deforms are no longer left to it

    // A Granny mesh as analysed for the shader: per vertex up to four bones (binding indices) and weights.
    struct MeshInfo
    {
        // What the mesh looked like when analysed: a freed mesh's address may come back with another mesh.
        uint32_t vertexCount = 0, bindingCount = 0;
        const void* positions = nullptr;
        const void* normals = nullptr;
        const void* bindings = nullptr;
        const void* weighted = nullptr;
        const void* normalWeights = nullptr;

        bool gpu = false;
        std::vector<uint8_t> bones;     // kInfluences per vertex
        std::vector<float> weights;

        // Static vertex buffers, per device and texture coordinate stream (the draw supplies those).
        struct Buffer
        {
            IDirect3DDevice7* device;
            const void* uv;
            uint32_t uvStride;
            Skin::Mesh* mesh;
        };
        std::vector<Buffer> buffers;

        // Static index buffers of the pieces: their triangle lists are the mesh's own arrays.
        struct Piece
        {
            IDirect3DDevice7* device;
            const WORD* source;
            uint32_t count;
            uint32_t check;             // of a few of the indices, in case the array changed after all
            Skin::Indices* indices;
        };
        std::vector<Piece> pieces;

        ~MeshInfo()
        {
            for (Buffer& b : buffers)
            {
                Skin::releaseMesh(b.mesh);
            }
            for (Piece& p : pieces)
            {
                Skin::releaseIndices(p.indices);
            }
        }

        bool matches(const uint8_t* mesh) const
        {
            return field<uint32_t>(mesh, Mesh::vertexCount) == vertexCount &&
                field<uint32_t>(mesh, Mesh::bindingCount) == bindingCount &&
                field<const void*>(mesh, Mesh::positions) == positions && field<const void*>(mesh, Mesh::normals) == normals &&
                field<const void*>(mesh, Mesh::bindings) == bindings && field<const void*>(mesh, Mesh::weighted) == weighted &&
                field<const void*>(mesh, Mesh::normalWeights) == normalWeights;
        }
    };

    // A deform left to the GPU, until its vertices are drawn (or skinned on the CPU after all).
    struct Pending
    {
        std::shared_ptr<MeshInfo> info;
        float* positions = nullptr;     // Granny's output buffers, as the draw's streams point at them
        float* normals = nullptr;       // null: the deform wanted positions only (shadows)
        bool normalize = false;
        std::vector<float> palette;     // kPaletteFloats per binding, for this pose
        uint32_t paletteId = 0;
        uint64_t serial = 0;
        bool filled = false;            // skinned on the CPU after all
    };

    // [Debug] D3DStats: skeletons GrannyAdvanceTime poses per frame against those drawn (posing runs on the
    // animation worker before the world view draws them).
    using PoseFn = void(__fastcall*)(uint8_t* skeleton, void* edx, uint32_t a, uint32_t b);
    PoseFn g_origPose = nullptr;
    const uint32_t* g_poseFrame = nullptr;
    struct PoseStats
    {
        std::mutex mutex;
        std::unordered_set<const void*> posed, drawn;
        uint64_t posedBones = 0;
        uint64_t frames = 0, posedTotal = 0, posedBonesTotal = 0, drawnTotal = 0, undrawnTotal = 0, undrawnBonesTotal = 0;
        DWORD nextLog = 0;
    };
    PoseStats g_poses;

    void __fastcall hookPose(uint8_t* skeleton, void* edx, uint32_t a, uint32_t b)
    {
        if (field<uint8_t>(skeleton, Skeleton::active) && field<uint32_t>(skeleton, Skeleton::posedFrame) != *g_poseFrame)
        {
            std::scoped_lock lock(g_poses.mutex);
            if (g_poses.posed.insert(skeleton).second)
            {
                g_poses.posedBones += field<uint32_t>(skeleton, Skeleton::boneCount);
            }
        }
        g_origPose(skeleton, edx, a, b);
    }

    void noteDrawn(void* bones)
    {
        if (g_origPose && bones)
        {
            std::scoped_lock lock(g_poses.mutex);
            g_poses.drawn.insert(*static_cast<void* const*>(bones));
        }
    }

    std::mutex g_mutex;
    std::unordered_map<const uint8_t*, std::shared_ptr<MeshInfo>> g_meshes;
    std::unordered_map<const void*, std::shared_ptr<Pending>> g_pending;    // by positions buffer
    uint32_t g_nextPaletteId = 1;
    uint64_t g_serial = 0;
    uint32_t g_meshLogs = 0;

    std::shared_ptr<MeshInfo> analyse(const uint8_t* mesh)
    {
        auto info = std::make_shared<MeshInfo>();
        info->vertexCount = field<uint32_t>(mesh, Mesh::vertexCount);
        info->bindingCount = field<uint32_t>(mesh, Mesh::bindingCount);
        info->positions = field<const void*>(mesh, Mesh::positions);
        info->normals = field<const void*>(mesh, Mesh::normals);
        info->bindings = field<const void*>(mesh, Mesh::bindings);
        info->weighted = field<const void*>(mesh, Mesh::weighted);
        info->normalWeights = field<const void*>(mesh, Mesh::normalWeights);
        const char* reason = nullptr;
        if (!plausible(mesh) || !info->normals)
        {
            reason = "implausible";
        }
        else if (info->vertexCount > 0xFFFF)
        {
            reason = "too many vertices";
        }
        else if (info->bindingCount > Skin::kMaxBones)
        {
            reason = "too many bones";
        }
        else if (field<uint32_t>(mesh, Mesh::normalCount) != info->vertexCount)
        {
            reason = "normal count";
        }
        if (!reason)
        {
            Influences positions, normals;
            uint8_t paths = 0;
            Problems problems;
            positionInfluences(mesh, positions, paths, problems);
            normalInfluences(mesh, normals, paths, problems);
            info->bones.assign(size_t(info->vertexCount) * kInfluences, 0);
            info->weights.assign(size_t(info->vertexCount) * kInfluences, 0.0f);
            if (problems.outOfRange || problems.tooMany)
            {
                reason = "influence lists";
            }
            for (uint32_t v = 0; !reason && v < info->vertexCount; ++v)
            {
                const auto& list = positions[v];
                if (list.empty() || list.size() > kInfluences)
                {
                    reason = "influences per vertex";
                }
                else if (!sameInfluences(list, normals[v]))
                {
                    reason = "normals weighted unlike positions";
                }
                for (size_t i = 0; !reason && i < list.size(); ++i)
                {
                    info->bones[v * kInfluences + i] = static_cast<uint8_t>(list[i].binding);
                    info->weights[v * kInfluences + i] = list[i].weight;
                }
            }
        }
        info->gpu = !reason;
        if (reason && g_meshLogs < 32)
        {
            ++g_meshLogs;
            LOG("GPU skinning: mesh {} ({} vertices, {} bones) stays on the CPU: {}", static_cast<const void*>(mesh),
                info->vertexCount, info->bindingCount, reason);
        }
        return info;
    }

    std::shared_ptr<MeshInfo> meshInfo(const uint8_t* mesh)
    {
        auto it = g_meshes.find(mesh);
        if (it != g_meshes.end() && it->second->matches(mesh))
        {
            return it->second;
        }
        if (g_meshes.size() > 4096)
        {
            g_meshes.clear();   // pending draws keep their own references
        }
        auto info = analyse(mesh);
        g_meshes[mesh] = info;
        return info;
    }

    // Vertex v of a mesh in the pose of `palette`, with its normal if `normal` is set.
    void skinOne(const MeshInfo& info, const float* palette, uint32_t v, float* position, float* normal)
    {
        const float* p = static_cast<const float*>(info.positions) + size_t(v) * 3;
        const float* n = static_cast<const float*>(info.normals) + size_t(v) * 3;
        float pos[3] = {}, nrm[3] = {};
        for (uint32_t i = 0; i < kInfluences; ++i)
        {
            const float w = info.weights[size_t(v) * kInfluences + i];
            if (w == 0.0f)
            {
                continue;
            }
            const float* m = palette + size_t(info.bones[size_t(v) * kInfluences + i]) * kPaletteFloats;
            for (int r = 0; r < 3; ++r)
            {
                pos[r] += w * (m[r * 3] * p[0] + m[r * 3 + 1] * p[1] + m[r * 3 + 2] * p[2] + m[9 + r]);
                nrm[r] += w * (m[r * 3] * n[0] + m[r * 3 + 1] * n[1] + m[r * 3 + 2] * n[2]);
            }
        }
        std::memcpy(position, pos, sizeof(pos));
        if (normal)
        {
            std::memcpy(normal, nrm, sizeof(nrm));
        }
    }

    // The vertices cGranny_render reads for a piece's bounding box: every step-th of the mesh's vertices.
    void boundsSamples(const Pending& p)
    {
        const uint32_t count = p.info->vertexCount;
        const uint32_t step = count < 12 ? 1 : count < 23 ? 2 : count < 34 ? 3 : count / 11;
        for (uint32_t v = 0; v < count; v += step)
        {
            skinOne(*p.info, p.palette.data(), v, p.positions + size_t(v) * 3, nullptr);
        }
    }

    // All of the vertices on the CPU, as Granny would have skinned them.
    void fill(Pending& p)
    {
        if (p.filled)
        {
            return;
        }
        p.filled = true;
        for (uint32_t v = 0; v < p.info->vertexCount; ++v)
        {
            float* n = p.normals ? p.normals + size_t(v) * 3 : nullptr;
            skinOne(*p.info, p.palette.data(), v, p.positions + size_t(v) * 3, n);
            if (n && p.normalize)
            {
                const float inv = 1.0f / std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                n[0] *= inv;
                n[1] *= inv;
                n[2] *= inv;
            }
        }
    }

    void __fastcall hookDeform(uint8_t* mesh, void* edx, void* bones, uint32_t* positionsOut, uint32_t doPositions,
        uint32_t* normalsOut, uint32_t doNormals, uint32_t normalizeNormals)
    {
        // The model pass wants positions and normals, the shadow pass positions only.
        const bool render = reinterpret_cast<uintptr_t>(_ReturnAddress()) == g_renderReturn;
        if (render)
        {
            noteDrawn(bones);
        }
        const bool positions = (doPositions & 0xFF) && positionsOut && positionsOut[1];
        const bool wantNormals = (doNormals & 0xFF) != 0;
        const bool normals = wantNormals && normalsOut && normalsOut[1];
        std::shared_ptr<MeshInfo> info;
        if (render && positions && normals == wantNormals && g_gpuUsable.load(std::memory_order_relaxed))
        {
            std::scoped_lock lock(g_mutex);
            info = meshInfo(mesh);
            // Granny allocates its output buffers in a full deform; until then it has to do one.
            if (!info->gpu || positionsOut[0] < info->vertexCount || (normals && normalsOut[0] < info->vertexCount))
            {
                info.reset();
            }
        }
        if (!info)
        {
            g_origDeform(mesh, edx, bones, positionsOut, doPositions, normalsOut, doNormals, normalizeNormals);
            if (positions)
            {
                std::scoped_lock lock(g_mutex);
                g_pending.erase(reinterpret_cast<const void*>(positionsOut[1]));
            }
            return;
        }
        // Only the bones' matrices for this pose: no positions, no normals.
        g_origDeform(mesh, edx, bones, positionsOut, 0, normalsOut, 0, normalizeNormals);

        auto p = std::make_shared<Pending>();
        p->info = info;
        p->positions = reinterpret_cast<float*>(positionsOut[1]);
        p->normals = normals ? reinterpret_cast<float*>(normalsOut[1]) : nullptr;
        p->normalize = (normalizeNormals & 0xFF) != 0;
        p->palette.resize(size_t(info->bindingCount) * kPaletteFloats);
        const uint8_t* binding = field<const uint8_t*>(mesh, Mesh::bindings);
        for (uint32_t b = 0; b < info->bindingCount; ++b, binding += Binding::size)
        {
            std::memcpy(&p->palette[size_t(b) * kPaletteFloats], binding + Binding::matrix, kPaletteFloats * sizeof(float));
        }
        boundsSamples(*p);
        D3DStats::count(D3DStats::CSkinDeferred);

        std::scoped_lock lock(g_mutex);
        p->paletteId = g_nextPaletteId++;
        if (g_nextPaletteId == 0)
        {
            g_nextPaletteId = 1;
        }
        p->serial = ++g_serial;
        const void* key = p->positions;
        g_pending[key] = std::move(p);
        // Entries of characters that are gone: their draw always follows their deform at once.
        if (g_pending.size() > 2048)
        {
            std::erase_if(g_pending, [](const auto& e) { return e.second->serial + 1024 < g_serial; });
        }
    }

    Skin::Mesh* staticBuffer(MeshInfo& info, IDirect3DDevice7* device, const D3DDP_PTRSTRIDE& uv)
    {
        for (const MeshInfo::Buffer& b : info.buffers)
        {
            if (b.device == device && b.uv == uv.lpvData && b.uvStride == uv.dwStride)
            {
                return b.mesh;
            }
        }
        std::vector<Skin::Vertex> vertices(info.vertexCount);
        const auto* positions = static_cast<const float*>(info.positions);
        const auto* normals = static_cast<const float*>(info.normals);
        const auto* uvs = static_cast<const uint8_t*>(uv.lpvData);
        for (uint32_t v = 0; v < info.vertexCount; ++v)
        {
            Skin::Vertex& out = vertices[v];
            std::memcpy(out.position, positions + size_t(v) * 3, sizeof(out.position));
            std::memcpy(out.normal, normals + size_t(v) * 3, sizeof(out.normal));
            std::memcpy(out.uv, uvs + size_t(v) * uv.dwStride, sizeof(out.uv));
            std::memcpy(out.bones, &info.bones[size_t(v) * kInfluences], sizeof(out.bones));
            std::memcpy(out.weights, &info.weights[size_t(v) * kInfluences], sizeof(out.weights));
        }
        Skin::Mesh* mesh = Skin::createMesh(device, vertices.data(), info.vertexCount);
        if (mesh)
        {
            info.buffers.push_back({device, uv.lpvData, uv.dwStride, mesh});
        }
        return mesh;
    }

    uint32_t indexCheck(const WORD* indices, uint32_t count)
    {
        uint32_t check = count;
        const uint32_t step = count / 16 + 1;
        for (uint32_t i = 0; i < count; i += step)
        {
            check = check * 31 + indices[i];
        }
        return check * 31 + indices[count - 1];
    }

    Skin::Indices* staticIndices(MeshInfo& info, IDirect3DDevice7* device, const WORD* indices, uint32_t count)
    {
        const uint32_t check = indexCheck(indices, count);
        for (MeshInfo::Piece& piece : info.pieces)
        {
            if (piece.device == device && piece.source == indices && piece.count == count)
            {
                if (piece.check == check)
                {
                    return piece.indices;
                }
                Skin::releaseIndices(piece.indices);
                piece.indices = Skin::createIndices(device, indices, count);
                piece.check = check;
                return piece.indices;
            }
        }
        Skin::Indices* created = Skin::createIndices(device, indices, count);
        if (created)
        {
            info.pieces.push_back({device, indices, count, check, created});
        }
        return created;
    }

    bool drawGpu(IDirect3DDevice7* real, Pending& p, void (*prepare)(void*), void* context, D3DPRIMITIVETYPE type,
        DWORD fvf, const D3DDRAWPRIMITIVESTRIDEDDATA& data, DWORD vertCount, const WORD* indices, DWORD indexCount)
    {
        // The draws cGranny_render and cGranny_renderShadow make from the rendering state; anything else is drawn as it
        // comes.
        const bool diffuse = (fvf & D3DFVF_DIFFUSE) != 0;
        const bool normals = (fvf & D3DFVF_NORMAL) != 0;
        if (type != D3DPT_TRIANGLELIST || (fvf != kFvfModel && fvf != kFvfModelDiffuse && fvf != kFvfShadow) ||
            vertCount != p.info->vertexCount || (normals && (!p.normals || data.normal.lpvData != p.normals)) ||
            !data.textureCoords[0].lpvData || !indices || indexCount < 3 || (diffuse && !data.diffuse.lpvData))
        {
            return false;
        }
        if (!Skin::available(real))
        {
            if (g_gpuUsable.exchange(false))
            {
                LOG("GPU skinning: the device can't skin (not the Direct3D 9 backend, or no vs_2_0): Granny skins on the CPU");
            }
            return false;
        }
        Skin::Mesh* mesh = staticBuffer(*p.info, real, data.textureCoords[0]);
        Skin::Indices* pieceIndices = mesh ? staticIndices(*p.info, real, indices, indexCount) : nullptr;
        if (!pieceIndices)
        {
            return false;
        }
        prepare(context);
        Skin::Draw draw = {};
        draw.mesh = mesh;
        draw.indices = pieceIndices;
        draw.palette = p.palette.data();
        draw.bones = p.info->bindingCount;
        draw.paletteId = p.paletteId;
        draw.normals = normals;
        draw.normalizeSkinned = p.normalize;
        // The shadows' one color comes with stride 0: a constant instead of a stream copied per draw.
        draw.vertexColor = diffuse;
        draw.color = 0xFFFFFFFF;
        if (diffuse && data.diffuse.dwStride == 0)
        {
            draw.color = *static_cast<const DWORD*>(data.diffuse.lpvData);
        }
        else if (diffuse)
        {
            draw.diffuse = static_cast<const DWORD*>(data.diffuse.lpvData);
            draw.diffuseStride = data.diffuse.dwStride;
        }
        return Skin::draw(real, draw);
    }
}

bool GpuSkin::active()
{
    return g_active;
}

void GpuSkin::onFrame()
{
    if (!g_origPose)
    {
        return;
    }
    PoseStats& s = g_poses;
    std::scoped_lock lock(s.mutex);
    if (s.posed.empty() && s.drawn.empty())
    {
        return;
    }
    ++s.frames;
    s.posedTotal += s.posed.size();
    s.posedBonesTotal += s.posedBones;
    s.drawnTotal += s.drawn.size();
    for (const void* skeleton : s.posed)
    {
        if (!s.drawn.contains(skeleton))
        {
            ++s.undrawnTotal;
            s.undrawnBonesTotal += field<uint32_t>(static_cast<const uint8_t*>(skeleton), Skeleton::boneCount);
        }
    }
    s.posed.clear();
    s.drawn.clear();
    s.posedBones = 0;
    const DWORD now = GetTickCount();
    if (!s.nextLog)
    {
        s.nextLog = now + 5000;
    }
    else if (static_cast<int>(now - s.nextLog) >= 0)
    {
        s.nextLog = now + 5000;
        const double f = static_cast<double>(s.frames);
        LOG("Animation poses per frame: {:.0f} skeletons ({:.0f} bones) posed, {:.0f} drawn, {:.0f} posed but not drawn "
            "({:.0f} bones)", s.posedTotal / f, s.posedBonesTotal / f, s.drawnTotal / f, s.undrawnTotal / f,
            s.undrawnBonesTotal / f);
        s.frames = s.posedTotal = s.posedBonesTotal = s.drawnTotal = s.undrawnTotal = s.undrawnBonesTotal = 0;
    }
}

bool GpuSkin::draw(IDirect3DDevice7* real, bool gpu, void (*prepare)(void*), void* context, D3DPRIMITIVETYPE type,
    DWORD fvf, const D3DDRAWPRIMITIVESTRIDEDDATA& data, DWORD vertCount, const WORD* indices, DWORD indexCount)
{
    std::shared_ptr<Pending> p;
    {
        std::scoped_lock lock(g_mutex);
        auto it = g_pending.find(data.position.lpvData);
        if (it == g_pending.end())
        {
            return false;
        }
        p = it->second;
    }
    if (gpu && !p->filled && drawGpu(real, *p, prepare, context, type, fvf, data, vertCount, indices, indexCount))
    {
        D3DStats::count(D3DStats::CSkinGpu);
        return true;
    }
    if (!p->filled)
    {
        D3DStats::count(D3DStats::CSkinCpu);
    }
    fill(*p);
    return false;
}

void GpuSkin::install()
{
    if (!g_config.gpuSkinning)
    {
        return;
    }
    if (!g_config.ddrawD3D9)
    {
        LOG("GPU skinning: off, it needs [DDraw] Backend=d3d9");
        return;
    }
    const uintptr_t deform = findDeform("GPU skinning");
    g_renderReturn = findRenderDeformReturn("GPU skinning");
    if (!deform || !g_renderReturn)
    {
        return;
    }
    if (g_config.d3dStats)
    {
        const uintptr_t pose = findPose("GPU skinning", g_poseFrame);
        if (pose && g_poseFrame && !Patch::hook(g_origPose, pose, &hookPose, "granny pose"))
        {
            g_origPose = nullptr;
        }
    }
    if (Patch::hook(g_origDeform, deform, &hookDeform, "granny deform"))
    {
        g_active = true;
        LOG("GPU skinning: on, characters are skinned in a vertex shader (Granny's deform at granny.dll + {:x})",
            deform - reinterpret_cast<uintptr_t>(GetModuleHandleW(L"granny.dll")));
    }
}
