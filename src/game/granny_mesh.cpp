#include "game/granny_mesh.h"
#include "log.h"
#include "sig.h"

#include <algorithm>
#include <cmath>
#include <span>

namespace GrannyMesh
{
    namespace
    {
        float bitsToFloat(int32_t bits)
        {
            float f;
            std::memcpy(&f, &bits, sizeof(f));
            return f;
        }

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

    namespace
    {
        uintptr_t findInGranny(const char* who, const char* what, const char* pattern, uint16_t offset)
        {
            HMODULE granny = GetModuleHandleW(L"granny.dll");
            if (!granny)
            {
                LOG("{}: granny.dll is not loaded", who);
                return 0;
            }
            const std::span<const uint8_t> code = codeSection(granny);
            uintptr_t address = 0;
            const Sig::Entry entries[] = {{what, pattern, offset, Sig::Take::Match, &address}};
            if (code.empty() || Sig::resolve(entries, code, reinterpret_cast<uintptr_t>(code.data())) != 0)
            {
                LOG("{}: {} not found in granny.dll", who, what);
                return 0;
            }
            return address;
        }
    }

    uintptr_t find(const char* who, const char* what, const char* pattern, uint16_t offset)
    {
        return findInGranny(who, what, pattern, offset);
    }

    uintptr_t callTarget(uintptr_t site)
    {
        if (!site || *reinterpret_cast<const uint8_t*>(site) != 0xE8)
        {
            return 0;
        }
        int32_t rel;
        std::memcpy(&rel, reinterpret_cast<const void*>(site + 1), sizeof(rel));
        return site + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(rel));
    }

    uintptr_t findDeform(const char* who)
    {
        return findInGranny(who, "Granny's deform routine",
            "83 EC 2C 8B 44 24 30 53 8B D9 56 8B 08 57 8B 7B 18 89 4C 24 20 83 FF 01", 0);
    }

    uintptr_t findPose(const char* who, const uint32_t*& frameCounter)
    {
        // push esi; mov esi, ecx; push edi; mov al, [esi + 0x6C]; test al, al; je; mov eax, [counter]; mov ecx, [esi + 0x78]
        const uintptr_t pose = findInGranny(who, "Granny's skeleton pose update",
            "56 8B F1 57 8A 46 6C 84 C0 74 ?? A1 ?? ?? ?? ?? 8B 4E 78 3B C8", 0);
        frameCounter = pose ? *reinterpret_cast<const uint32_t* const*>(pose + 12) : nullptr;
        return pose;
    }

    uintptr_t findRenderDeformReturn(const char* who)
    {
        // lea eax, [edi + 0x1A8] (normals out) ... lea edx, [edi + 0x1A0] (positions out) ... call deform
        return findInGranny(who, "the rendering path's deform call",
            "8D 87 A8 01 00 00 0F 95 C1 51 8B 4C 24 24 52 50 8B 87 88 01 00 00 8D 97 A0 01 00 00 51 8B 4C 24 60 83 C0 14 "
            "52 50 E8 ?? ?? ?? ??", 43);
    }

    bool plausible(const uint8_t* mesh)
    {
        const uint32_t vertices = field<uint32_t>(mesh, Mesh::vertexCount);
        const uint32_t bindings = field<uint32_t>(mesh, Mesh::bindingCount);
        return vertices != 0 && vertices <= kMaxVertices && bindings != 0 && bindings <= kMaxBindings &&
            field<const float*>(mesh, Mesh::positions) && field<const uint8_t*>(mesh, Mesh::bindings);
    }

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
        for (uint32_t v = 0; s && v < weightedCount; ++v)
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

    void skinVertex(const float* palette, uint32_t paletteStride, const std::vector<Influence>& list, const float* v,
        bool translate, float* out)
    {
        out[0] = out[1] = out[2] = 0.0f;
        for (const Influence& i : list)
        {
            const float* m = palette + size_t(i.binding) * paletteStride;
            for (int r = 0; r < 3; ++r)
            {
                out[r] += i.weight * (m[r * 3] * v[0] + m[r * 3 + 1] * v[1] + m[r * 3 + 2] * v[2] + (translate ? m[9 + r] : 0.0f));
            }
        }
    }

    void skin(const float* palette, uint32_t paletteStride, const Influences& inf, const float* src, uint32_t count,
        bool translate, uint32_t limit, float* out)
    {
        std::vector<Influence> kept;
        for (uint32_t v = 0; v < count; ++v)
        {
            const std::vector<Influence>& list = inf[v];
            if (list.size() <= limit)
            {
                skinVertex(palette, paletteStride, list, src + size_t(v) * 3, translate, out + size_t(v) * 3);
                continue;
            }
            // The largest `limit` influences, scaled to the weight of all of them.
            kept = list;
            float all = 0.0f, part = 0.0f;
            for (const Influence& i : kept)
            {
                all += i.weight;
            }
            std::partial_sort(kept.begin(), kept.begin() + limit, kept.end(),
                [](const Influence& a, const Influence& b) { return a.weight > b.weight; });
            kept.resize(limit);
            for (const Influence& i : kept)
            {
                part += i.weight;
            }
            const float scale = part != 0.0f ? all / part : 1.0f;
            for (Influence& i : kept)
            {
                i.weight *= scale;
            }
            skinVertex(palette, paletteStride, kept, src + size_t(v) * 3, translate, out + size_t(v) * 3);
        }
    }
}
