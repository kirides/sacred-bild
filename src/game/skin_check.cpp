#include "game/skin_check.h"
#include "config.h"
#include "log.h"
#include "patch.h"
#include "sig.h"

#include <windows.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{
    // granny.dll internals (image base 0x10000000; the English and German installs ship the same file), from the
    // deform routine FUN_1001e660 and its helpers: FUN_1001de00 (bone-major runs), FUN_1001df00 (vertex-major lists),
    // FUN_1001e490 (normal lists), FUN_1001e050 / FUN_1001e0e0 (one rigid bone).
    namespace Mesh
    {
        constexpr uintptr_t bindingCount = 0x0C;
        constexpr uintptr_t bindings = 0x14;        // Binding[bindingCount]
        constexpr uintptr_t vertexCount = 0x18;
        constexpr uintptr_t positions = 0x1C;       // float[3] per vertex, bind pose
        constexpr uintptr_t normalCount = 0x20;
        constexpr uintptr_t normals = 0x24;         // float[3] per normal
        // Vertices 0 .. weightedCount - 1 have a vertex-major list (if hasWeighted): an int stream of, per vertex, a
        // count and (binding, float weight) pairs. `duplicates` then holds, per such vertex, a count and offsets
        // (relative to the vertex) of copies that get its result.
        constexpr uintptr_t weightedCount = 0x4C;
        constexpr uintptr_t hasWeighted = 0x50;
        constexpr uintptr_t weighted = 0x54;
        constexpr uintptr_t duplicates = 0x5C;
        // Per normal (if hasNormalWeights) a pointer to a count and (binding, float weight) pairs.
        constexpr uintptr_t hasNormalWeights = 0x60;
        constexpr uintptr_t normalWeights = 0x64;
    }

    namespace Binding
    {
        constexpr uintptr_t size = 0x7C;
        // Bone-major influences, used for positions and normals: `listLength` ints of runs (first vertex, count, then
        // `count` float weights).
        constexpr uintptr_t listLength = 0x44;
        constexpr uintptr_t list = 0x48;
        // Set by the deform for the current pose: out = M * v + t, M row-major.
        constexpr uintptr_t matrix = 0x4C;
        constexpr uintptr_t translation = 0x70;
    }

    // Sanity limits: anything beyond them means the layout is not what this module assumes.
    constexpr uint32_t kMaxVertices = 0x40000;
    constexpr uint32_t kMaxBindings = 0x1000;
    constexpr uint32_t kMaxInfluences = 64;
    constexpr uint32_t kGpuInfluences = 4;

    // thiscall (bones, positions out, do positions, normals out, do normals, normalize normals). The outputs are
    // {capacity, float* data} with vertexCount entries; the flags are bytes in 4-byte stack slots.
    using DeformFn = void(__fastcall*)(uint8_t* mesh, void* edx, void* bones, uint32_t* positionsOut, uint32_t doPositions,
        uint32_t* normalsOut, uint32_t doNormals, uint32_t normalize);
    DeformFn g_origDeform = nullptr;

    template <class T>
    T field(const uint8_t* p, uintptr_t offset)
    {
        T v;
        std::memcpy(&v, p + offset, sizeof(T));
        return v;
    }

    float bitsToFloat(int32_t bits)
    {
        float f;
        std::memcpy(&f, &bits, sizeof(f));
        return f;
    }

    struct Influence
    {
        uint32_t binding;
        float weight;
    };
    using Influences = std::vector<std::vector<Influence>>;

    enum Path : uint8_t
    {
        BoneMajor = 1,
        Rigid = 2,
        VertexMajor = 4,
        NormalLists = 8,
    };

    struct Problems
    {
        uint32_t outOfRange = 0;    // vertex, binding or duplicate index outside the mesh
        uint32_t tooMany = 0;       // influence lists longer than kMaxInfluences
    };

    void reset(Influences& inf, uint32_t count)
    {
        if (inf.size() < count)
        {
            inf.resize(count);
        }
        for (uint32_t v = 0; v < count; ++v)
        {
            inf[v].clear();
        }
    }

    void push(std::vector<Influence>& list, uint32_t binding, float weight, uint32_t bindings, Problems& problems)
    {
        if (binding >= bindings)
        {
            ++problems.outOfRange;
        }
        else if (list.size() >= kMaxInfluences)
        {
            ++problems.tooMany;
        }
        else
        {
            list.push_back({binding, weight});
        }
    }

    // A count and (binding, float weight) pairs; returns the end of the list.
    const int32_t* readList(const int32_t* s, std::vector<Influence>& list, uint32_t bindings, Problems& problems)
    {
        list.clear();
        const int32_t count = *s++;
        if (count < 0 || count > 1024)
        {
            ++problems.outOfRange;
            return s;
        }
        for (int32_t i = 0; i < count; ++i, s += 2)
        {
            push(list, static_cast<uint32_t>(s[0]), bitsToFloat(s[1]), bindings, problems);
        }
        return s;
    }

    // FUN_1001de00 for every binding: weighted adds into the vertices of each run.
    void addBoneMajor(const uint8_t* mesh, uint32_t vertices, Influences& inf, uint8_t& paths, Problems& problems)
    {
        const uint32_t bindings = field<uint32_t>(mesh, Mesh::bindingCount);
        const uint8_t* binding = field<const uint8_t*>(mesh, Mesh::bindings);
        for (uint32_t b = 0; b < bindings; ++b, binding += Binding::size)
        {
            int32_t length = field<int32_t>(binding, Binding::listLength);
            const int32_t* run = field<const int32_t*>(binding, Binding::list);
            while (length > 0 && run)
            {
                const int32_t first = run[0], count = run[1];
                if (count < 0 || count > length)
                {
                    ++problems.outOfRange;
                    break;
                }
                for (int32_t i = 0; i < count; ++i)
                {
                    const uint32_t v = static_cast<uint32_t>(first + i);
                    if (v < vertices)
                    {
                        push(inf[v], b, bitsToFloat(run[2 + i]), bindings, problems);
                    }
                    else
                    {
                        ++problems.outOfRange;
                    }
                }
                run += 2 + count;
                length -= 2 + count;
                paths |= BoneMajor;
            }
        }
    }

    bool rigid(const uint8_t* mesh)
    {
        return field<uint32_t>(mesh, Mesh::weightedCount) == 0 && field<uint32_t>(mesh, Mesh::bindingCount) < 2;
    }

    // Per vertex, the influences Granny's deform gives its position: bone-major adds, then (overwriting) one rigid
    // bone or the vertex-major lists and their duplicates.
    void positionInfluences(const uint8_t* mesh, Influences& inf, uint8_t& paths, Problems& problems)
    {
        const uint32_t vertices = field<uint32_t>(mesh, Mesh::vertexCount);
        const uint32_t bindings = field<uint32_t>(mesh, Mesh::bindingCount);
        reset(inf, vertices);
        addBoneMajor(mesh, vertices, inf, paths, problems);
        if (rigid(mesh))
        {
            for (uint32_t v = 0; v < vertices; ++v)
            {
                inf[v].assign(1, {0, 1.0f});
            }
            paths |= Rigid;
            return;
        }
        if (!field<uint32_t>(mesh, Mesh::hasWeighted))
        {
            return;
        }
        const uint32_t weightedCount = std::min(field<uint32_t>(mesh, Mesh::weightedCount), vertices);
        const int32_t* s = field<const int32_t*>(mesh, Mesh::weighted);
        for (uint32_t v = 0; v < weightedCount; ++v)
        {
            s = readList(s, inf[v], bindings, problems);
        }
        const int32_t* d = field<const int32_t*>(mesh, Mesh::duplicates);
        for (uint32_t v = 0; d && v < weightedCount; ++v)
        {
            const int32_t count = *d++;
            for (int32_t i = 0; i < count; ++i)
            {
                const uint32_t target = v + static_cast<uint32_t>(*d++);
                if (target < vertices)
                {
                    inf[target] = inf[v];
                }
                else
                {
                    ++problems.outOfRange;
                }
            }
        }
        paths |= VertexMajor;
    }

    // The same for normals: bone-major adds (vertex index space), then one rigid bone or the per-normal lists.
    void normalInfluences(const uint8_t* mesh, Influences& inf, uint8_t& paths, Problems& problems)
    {
        const uint32_t vertices = field<uint32_t>(mesh, Mesh::vertexCount);
        const uint32_t normals = std::min(field<uint32_t>(mesh, Mesh::normalCount), vertices);
        const uint32_t bindings = field<uint32_t>(mesh, Mesh::bindingCount);
        reset(inf, vertices);
        addBoneMajor(mesh, vertices, inf, paths, problems);
        if (rigid(mesh))
        {
            for (uint32_t n = 0; n < normals; ++n)
            {
                inf[n].assign(1, {0, 1.0f});
            }
            return;
        }
        if (!field<uint32_t>(mesh, Mesh::hasNormalWeights))
        {
            return;
        }
        const int32_t* const* lists = field<const int32_t* const*>(mesh, Mesh::normalWeights);
        for (uint32_t n = 0; lists && n < normals; ++n)
        {
            if (lists[n])
            {
                readList(lists[n], inf[n], bindings, problems);
            }
        }
        paths |= NormalLists;
    }

    // out = sum of w * (M v + t) over at most `limit` influences per vertex (the largest, scaled to the full sum).
    void skin(const uint8_t* mesh, const Influences& inf, const float* src, uint32_t count, bool translate, uint32_t limit,
        std::vector<float>& out)
    {
        const uint8_t* bindings = field<const uint8_t*>(mesh, Mesh::bindings);
        out.assign(size_t(count) * 3, 0.0f);
        Influence kept[kMaxInfluences];
        for (uint32_t v = 0; v < count; ++v)
        {
            const std::vector<Influence>& list = inf[v];
            size_t n = list.size();
            std::copy(list.begin(), list.end(), kept);
            float scale = 1.0f;
            if (n > limit)
            {
                float all = 0.0f, part = 0.0f;
                for (size_t i = 0; i < n; ++i)
                {
                    all += kept[i].weight;
                }
                std::partial_sort(kept, kept + limit, kept + n,
                    [](const Influence& a, const Influence& b) { return a.weight > b.weight; });
                n = limit;
                for (size_t i = 0; i < n; ++i)
                {
                    part += kept[i].weight;
                }
                scale = part != 0.0f ? all / part : 1.0f;
            }
            const float* p = src + size_t(v) * 3;
            float* o = out.data() + size_t(v) * 3;
            for (size_t i = 0; i < n; ++i)
            {
                const uint8_t* b = bindings + size_t(kept[i].binding) * Binding::size;
                const float* m = reinterpret_cast<const float*>(b + Binding::matrix);
                const float* t = reinterpret_cast<const float*>(b + Binding::translation);
                const float w = kept[i].weight * scale;
                for (int r = 0; r < 3; ++r)
                {
                    o[r] += w * (m[r * 3] * p[0] + m[r * 3 + 1] * p[1] + m[r * 3 + 2] * p[2] + (translate ? t[r] : 0.0f));
                }
            }
        }
    }

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
            }
        }
    }

    bool sameInfluences(std::vector<Influence> a, std::vector<Influence> b)
    {
        if (a.size() != b.size())
        {
            return false;
        }
        auto byBinding = [](const Influence& x, const Influence& y) { return x.binding < y.binding; };
        std::sort(a.begin(), a.end(), byBinding);
        std::sort(b.begin(), b.end(), byBinding);
        for (size_t i = 0; i < a.size(); ++i)
        {
            if (a[i].binding != b[i].binding || std::fabs(a[i].weight - b[i].weight) > 1e-4f)
            {
                return false;
            }
        }
        return true;
    }

    struct Totals
    {
        uint32_t calls = 0, positionCalls = 0, normalCalls = 0, checks = 0, meshes = 0;
        uint32_t maxBindings = 0, maxVertices = 0, maxInfluences = 0, maxNormalInfluences = 0;
        uint64_t vertices = 0, over4 = 0, normalsOver4 = 0, unlikeNormals = 0, unweighted = 0;
        float positionError = 0.0f, positionError4 = 0.0f;  // relative to the mesh's extent
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
        const uint32_t vertices = field<uint32_t>(mesh, Mesh::vertexCount);
        const uint32_t normals = field<uint32_t>(mesh, Mesh::normalCount);
        const uint32_t bindings = field<uint32_t>(mesh, Mesh::bindingCount);
        const float* srcPositions = field<const float*>(mesh, Mesh::positions);
        const float* srcNormals = field<const float*>(mesh, Mesh::normals);
        if (vertices == 0 || vertices > kMaxVertices || bindings == 0 || bindings > kMaxBindings || !srcPositions ||
            !field<const uint8_t*>(mesh, Mesh::bindings))
        {
            ++g_totals.skipped;
            return;
        }
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

        // Positions: extent from Granny's result.
        float lo[3] = {FLT_MAX, FLT_MAX, FLT_MAX}, hi[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
        for (uint32_t v = 0; v < vertices; ++v)
        {
            for (int c = 0; c < 3; ++c)
            {
                const float x = positionsOut[v * 3 + c];
                if (std::isfinite(x))
                {
                    lo[c] = std::min(lo[c], x);
                    hi[c] = std::max(hi[c], x);
                }
            }
        }
        const float extent = std::max(
            std::sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) + (hi[2] - lo[2]) * (hi[2] - lo[2])),
            1e-6f);
        Error pos, pos4;
        skin(mesh, g_positionInf, srcPositions, vertices, true, kMaxInfluences, g_ours);
        compare(positionsOut, g_ours, vertices, pos);
        skin(mesh, g_positionInf, srcPositions, vertices, true, kGpuInfluences, g_ours);
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
            skin(mesh, g_normalInf, srcNormals, count, false, kMaxInfluences, g_ours);
            if (normalized)
            {
                normalize(g_ours);
            }
            compare(normalsOut, g_ours, count, nrm);
            skin(mesh, g_normalInf, srcNormals, count, false, kGpuInfluences, g_ours);
            if (normalized)
            {
                normalize(g_ours);
            }
            compare(normalsOut, g_ours, count, nrm4);
        }

        t.maxBindings = std::max(t.maxBindings, bindings);
        t.maxVertices = std::max(t.maxVertices, vertices);
        t.maxInfluences = std::max(t.maxInfluences, maxInf);
        t.maxNormalInfluences = std::max(t.maxNormalInfluences, maxNormalInf);
        t.vertices += vertices;
        t.over4 += over4;
        t.normalsOver4 += normalsOver4;
        t.unlikeNormals += unlike;
        t.unweighted += unweighted;
        t.positionError = std::max(t.positionError, pos.max / extent);
        t.positionError4 = std::max(t.positionError4, pos4.max / extent);
        t.normalError = std::max(t.normalError, nrm.max);
        t.normalError4 = std::max(t.normalError4, nrm4.max);
        t.nonFinite += pos.nonFinite + nrm.nonFinite;
        t.outOfRange += problems.outOfRange;
        t.tooMany += problems.tooMany;
        t.paths |= paths;

        const bool suspicious = pos.max > 1e-3f * extent || nrm.max > 1e-2f || pos.nonFinite || nrm.nonFinite ||
            problems.outOfRange || problems.tooMany;
        if ((firstSight || suspicious) && g_meshLogs < 64)
        {
            ++g_meshLogs;
            LOG("Skin check: {}mesh {} | {} vertices, {} normals, {} bones,{} | influences up to {} ({} vertices over 4, "
                "{} without), normals up to {} ({} over 4, {} weighted unlike their position) | position error {:.2g} "
                "(4 influences {:.2g}) of extent {:.3g}, normal error {:.2g} (4 influences {:.2g}){}{}",
                suspicious ? "MISMATCH " : "", static_cast<const void*>(mesh), vertices, normals, bindings,
                pathNames(paths), maxInf, over4, unweighted, maxNormalInf, normalsOver4, unlike, pos.max, pos4.max,
                extent, nrm.max, nrm4.max, normalsOut ? "" : " (positions only)",
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
            "unlike normals {}, without influences {} | worst position error {:.2g} of the extent (4 influences {:.2g}), "
            "normal error {:.2g} ({:.2g}), weight sum off by {:.2g} | paths{} | out of range {}, too many {}, not finite {}",
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

    // granny.dll's code section as mapped.
    std::span<const uint8_t> codeSection(HMODULE module)
    {
        const auto* base = reinterpret_cast<const uint8_t*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
        {
            if (std::memcmp(section->Name, ".text", 6) == 0)
            {
                return {base + section->VirtualAddress, section->Misc.VirtualSize};
            }
        }
        return {};
    }
}

void SkinCheck::install()
{
    if (!g_config.skinCheck)
    {
        return;
    }
    HMODULE granny = GetModuleHandleW(L"granny.dll");
    if (!granny)
    {
        LOG("Skin check: granny.dll is not loaded");
        return;
    }
    const std::span<const uint8_t> code = codeSection(granny);
    uintptr_t deform = 0;
    const Sig::Entry entries[] = {
        {"granny deform", "83 EC 2C 8B 44 24 30 53 8B D9 56 8B 08 57 8B 7B 18 89 4C 24 20 83 FF 01", 0, Sig::Take::Match,
            &deform},
    };
    if (code.empty() || Sig::resolve(entries, code, reinterpret_cast<uintptr_t>(code.data())) != 0)
    {
        LOG("Skin check: Granny's deform routine not found");
        return;
    }
    if (Patch::hook(g_origDeform, deform, &hookDeform, "granny deform"))
    {
        LOG("Skin check: comparing Granny's deformation with per-vertex weights (granny.dll + {:x})",
            deform - reinterpret_cast<uintptr_t>(granny));
    }
}
