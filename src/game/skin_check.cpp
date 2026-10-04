#include "game/skin_check.h"
#include "game/granny_mesh.h"
#include "config.h"
#include "log.h"
#include "patch.h"

#include <windows.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{
    using namespace GrannyMesh;

    constexpr uint32_t kGpuInfluences = 4;

    GrannyMesh::DeformFn g_origDeform = nullptr;

    void normalize(std::vector<float>& v)
    {
        for (size_t i = 0; i + 2 < v.size(); i += 3)
        {
            const float inv = 1.0f / std::sqrt(v[i] * v[i] + v[i + 1] * v[i + 1] + v[i + 2] * v[i + 2]);
            v[i] *= inv;
            v[i + 1] *= inv;
            v[i + 2] *= inv;
        }
    }

    struct Error
    {
        float max = 0.0f;
        float magnitude = 0.0f;     // largest |coordinate| of Granny's result
        uint32_t nonFinite = 0;     // a component that is not finite in exactly one of both
    };

    void compare(const float* granny, const std::vector<float>& ours, uint32_t count, Error& e)
    {
        for (size_t i = 0; i < size_t(count) * 3; ++i)
        {
            const bool a = std::isfinite(granny[i]), b = std::isfinite(ours[i]);
            if (a != b)
            {
                ++e.nonFinite;
            }
            else if (a)
            {
                e.max = std::max(e.max, std::fabs(granny[i] - ours[i]));
                e.magnitude = std::max(e.magnitude, std::fabs(granny[i]));
            }
        }
    }

    struct Totals
    {
        uint32_t calls = 0, positionCalls = 0, normalCalls = 0, checks = 0, meshes = 0;
        uint32_t maxBindings = 0, maxVertices = 0, maxInfluences = 0, maxNormalInfluences = 0;
        uint64_t vertices = 0, over4 = 0, normalsOver4 = 0, unlikeNormals = 0, unweighted = 0;
        float positionError = 0.0f, positionError4 = 0.0f;  // relative to the largest coordinate
        float normalError = 0.0f, normalError4 = 0.0f;
        float weightSum = 0.0f;     // largest |sum of a vertex's weights - 1|
        uint32_t nonFinite = 0, outOfRange = 0, tooMany = 0, skipped = 0;
        uint8_t paths = 0;
    };

    Totals g_totals;
    std::unordered_set<const void*> g_seen;
    uint32_t g_meshLogs = 0;
    DWORD g_nextReport = 0;
    Influences g_positionInf, g_normalInf;
    std::vector<float> g_ours;

    std::string pathNames(uint8_t paths)
    {
        std::string s;
        if (paths & BoneMajor) s += " bone-major";
        if (paths & Rigid) s += " rigid";
        if (paths & VertexMajor) s += " vertex-major";
        if (paths & NormalLists) s += " normal-lists";
        return s.empty() ? " none" : s;
    }

    void check(const uint8_t* mesh, const float* positionsOut, const float* normalsOut, bool normalized, bool firstSight)
    {
        if (!plausible(mesh))
        {
            ++g_totals.skipped;
            return;
        }
        const uint32_t vertices = field<uint32_t>(mesh, Mesh::vertexCount);
        const uint32_t normals = field<uint32_t>(mesh, Mesh::normalCount);
        const uint32_t bindings = field<uint32_t>(mesh, Mesh::bindingCount);
        const float* srcPositions = field<const float*>(mesh, Mesh::positions);
        const float* srcNormals = field<const float*>(mesh, Mesh::normals);
        const float* palette = bindingPalette(mesh);
        Totals& t = g_totals;
        ++t.checks;
        uint8_t paths = 0;
        Problems problems;
        positionInfluences(mesh, g_positionInf, paths, problems);

        uint32_t maxInf = 0, over4 = 0, unweighted = 0;
        for (uint32_t v = 0; v < vertices; ++v)
        {
            const auto& list = g_positionInf[v];
            maxInf = std::max<uint32_t>(maxInf, static_cast<uint32_t>(list.size()));
            over4 += list.size() > kGpuInfluences;
            unweighted += list.empty();
            float sum = 0.0f;
            for (const Influence& i : list)
            {
                sum += i.weight;
            }
            if (!list.empty())
            {
                t.weightSum = std::max(t.weightSum, std::fabs(sum - 1.0f));
            }
        }

        Error pos, pos4;
        g_ours.resize(size_t(vertices) * 3);
        skin(palette, kBindingPaletteStride, g_positionInf, srcPositions, vertices, true, kMaxInfluences, g_ours.data());
        compare(positionsOut, g_ours, vertices, pos);
        skin(palette, kBindingPaletteStride, g_positionInf, srcPositions, vertices, true, kGpuInfluences, g_ours.data());
        compare(positionsOut, g_ours, vertices, pos4);

        Error nrm, nrm4;
        uint32_t maxNormalInf = 0, normalsOver4 = 0, unlike = 0;
        if (normalsOut && srcNormals)
        {
            normalInfluences(mesh, g_normalInf, paths, problems);
            const uint32_t count = std::min(normals, vertices);
            for (uint32_t n = 0; n < vertices; ++n)
            {
                maxNormalInf = std::max<uint32_t>(maxNormalInf, static_cast<uint32_t>(g_normalInf[n].size()));
                normalsOver4 += g_normalInf[n].size() > kGpuInfluences;
                if (n < count && !sameInfluences(g_positionInf[n], g_normalInf[n]))
                {
                    ++unlike;
                }
            }
            // Granny's normal source has normalCount entries; only those are compared.
            g_ours.resize(size_t(count) * 3);
            skin(palette, kBindingPaletteStride, g_normalInf, srcNormals, count, false, kMaxInfluences, g_ours.data());
            if (normalized)
            {
                normalize(g_ours);
            }
            compare(normalsOut, g_ours, count, nrm);
            skin(palette, kBindingPaletteStride, g_normalInf, srcNormals, count, false, kGpuInfluences, g_ours.data());
            if (normalized)
            {
                normalize(g_ours);
            }
            compare(normalsOut, g_ours, count, nrm4);
        }

        // Granny adds in the x87's extended precision and stores floats, this module adds floats: up to a few float
        // steps (2^-23 relative) at the largest coordinate are the same result.
        const float magnitude = std::max(pos.magnitude, 1e-6f);
        t.maxBindings = std::max(t.maxBindings, bindings);
        t.maxVertices = std::max(t.maxVertices, vertices);
        t.maxInfluences = std::max(t.maxInfluences, maxInf);
        t.maxNormalInfluences = std::max(t.maxNormalInfluences, maxNormalInf);
        t.vertices += vertices;
        t.over4 += over4;
        t.normalsOver4 += normalsOver4;
        t.unlikeNormals += unlike;
        t.unweighted += unweighted;
        t.positionError = std::max(t.positionError, pos.max / magnitude);
        t.positionError4 = std::max(t.positionError4, pos4.max / magnitude);
        t.normalError = std::max(t.normalError, nrm.max);
        t.normalError4 = std::max(t.normalError4, nrm4.max);
        t.nonFinite += pos.nonFinite + nrm.nonFinite;
        t.outOfRange += problems.outOfRange;
        t.tooMany += problems.tooMany;
        t.paths |= paths;

        const bool suspicious = pos.max > 1e-5f * magnitude || nrm.max > 1e-3f || pos.nonFinite || nrm.nonFinite ||
            problems.outOfRange || problems.tooMany;
        if ((firstSight || suspicious) && g_meshLogs < 64)
        {
            ++g_meshLogs;
            LOG("Skin check: {}mesh {} | {} vertices, {} normals, {} bones,{} | influences up to {} ({} vertices over 4, "
                "{} without), normals up to {} ({} over 4, {} weighted unlike their position) | position error {:.2g} "
                "(4 influences {:.2g}) at coordinates up to {:.3g}, normal error {:.2g} (4 influences {:.2g}){}{}",
                suspicious ? "MISMATCH " : "", static_cast<const void*>(mesh), vertices, normals, bindings,
                pathNames(paths), maxInf, over4, unweighted, maxNormalInf, normalsOver4, unlike, pos.max, pos4.max,
                pos.magnitude, nrm.max, nrm4.max, normalsOut ? "" : " (positions only)",
                problems.outOfRange || problems.tooMany || pos.nonFinite || nrm.nonFinite
                    ? std::format(" | out of range {}, too many {}, not finite {}", problems.outOfRange, problems.tooMany,
                          pos.nonFinite + nrm.nonFinite)
                    : std::string());
        }
    }

    void report()
    {
        const Totals& t = g_totals;
        LOG("Skin check: {} deforms ({} positions, {} normals), {} checked, {} meshes, {} skipped | bones per mesh up to {}, "
            "vertices up to {}, influences up to {} (normals {}) | over 4 influences: {} of {} vertices ({} normals), "
            "unlike normals {}, without influences {} | worst position error {:.2g} of the largest coordinate "
            "(4 influences {:.2g}), normal error {:.2g} ({:.2g}), weight sum off by {:.2g} | paths{} | out of range {}, "
            "too many {}, not finite {}",
            t.calls, t.positionCalls, t.normalCalls, t.checks, t.meshes, t.skipped, t.maxBindings, t.maxVertices,
            t.maxInfluences, t.maxNormalInfluences, t.over4, t.vertices, t.normalsOver4, t.unlikeNormals, t.unweighted,
            t.positionError, t.positionError4, t.normalError, t.normalError4, t.weightSum, pathNames(t.paths),
            t.outOfRange, t.tooMany, t.nonFinite);
    }

    void __fastcall hookDeform(uint8_t* mesh, void* edx, void* bones, uint32_t* positionsOut, uint32_t doPositions,
        uint32_t* normalsOut, uint32_t doNormals, uint32_t normalizeNormals)
    {
        g_origDeform(mesh, edx, bones, positionsOut, doPositions, normalsOut, doNormals, normalizeNormals);
        Totals& t = g_totals;
        ++t.calls;
        const bool positions = (doPositions & 0xFF) && field<uint32_t>(mesh, Mesh::vertexCount) != 0;
        const bool normals = (doNormals & 0xFF) && field<uint32_t>(mesh, Mesh::vertexCount) != 0;
        t.positionCalls += positions;
        t.normalCalls += normals;
        const bool firstSight = g_seen.insert(mesh).second;
        t.meshes += firstSight;
        // Every new mesh, and every 16th deform: a check costs about as much as the deform itself.
        if (positions && positionsOut && (firstSight || t.calls % 16 == 0))
        {
            check(mesh, reinterpret_cast<const float*>(positionsOut[1]),
                normals && normalsOut ? reinterpret_cast<const float*>(normalsOut[1]) : nullptr, (normalizeNormals & 0xFF) != 0,
                firstSight);
        }
        const DWORD now = GetTickCount();
        if (!g_nextReport)
        {
            g_nextReport = now + 10000;
        }
        else if (static_cast<int>(now - g_nextReport) >= 0)
        {
            g_nextReport = now + 10000;
            report();
        }
    }
}

void SkinCheck::install()
{
    if (!g_config.skinCheck)
    {
        return;
    }
    if (g_config.gpuSkinning)
    {
        LOG("Skin check: off, [Render] GpuSkinning replaces Granny's deformation");
        return;
    }
    const uintptr_t deform = findDeform("Skin check");
    if (deform && Patch::hook(g_origDeform, deform, &hookDeform, "granny deform"))
    {
        LOG("Skin check: comparing Granny's deformation with per-vertex weights (granny.dll + {:x})",
            deform - reinterpret_cast<uintptr_t>(GetModuleHandleW(L"granny.dll")));
    }
}
