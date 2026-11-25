#pragma once
#include "mem.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <vector>

// Granny 1.x meshes as granny.dll's deform routine (FUN_1001e660) sees them: bind-pose vertices, bone bindings with
// the matrices of the current pose, and the influence lists in Granny's three layouts (see docs/RE_NOTES.md,
// Character skinning). Shared by SkinCheck (validation) and GpuSkin (skinning in a vertex shader).
namespace GrannyMesh
{
    // granny.dll internals (image base 0x10000000; the English and German installs ship the same file).
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
        // Set by the deform for the current pose: out = M * v + t, M row-major; the translation follows the matrix,
        // so a binding's 12 floats from `matrix` on are its bone in palette form.
        constexpr uintptr_t matrix = 0x4C;
        constexpr uintptr_t translation = 0x70;
    }

    // Floats per bone in a palette: Granny's row-major 3x3 matrix, then the translation.
    constexpr uint32_t kPaletteFloats = 12;
    // Sanity limits: anything beyond them means the layout is not what this module assumes.
    constexpr uint32_t kMaxVertices = 0x40000;
    constexpr uint32_t kMaxBindings = 0x1000;
    constexpr uint32_t kMaxInfluences = 64;

    // thiscall on the mesh (bones, positions out, do positions, normals out, do normals, normalize normals), ret 0x18.
    // The outputs are {capacity, float* data}; the flags are bytes in 4-byte stack slots.
    using DeformFn = void(__fastcall*)(uint8_t* mesh, void* edx, void* bones, uint32_t* positionsOut, uint32_t doPositions,
        uint32_t* normalsOut, uint32_t doNormals, uint32_t normalize);

    // Code in the loaded granny.dll by signature (`offset` bytes into the match), 0 if not found (logged with `who`).
    uintptr_t find(const char* who, const char* what, const char* pattern, uint16_t offset);
    // The target of the `call rel32` at `site`, 0 if there is none.
    uintptr_t callTarget(uintptr_t site);

    // The deform routine in the loaded granny.dll (by signature), 0 if not found (logged with `who`).
    uintptr_t findDeform(const char* who);
    // Where the deform returns to in the rendering path (LockNextRenderingState's engine function 0x10034200, the
    // first state of a sequence lock), 0 if not found. GrannyLockSequenceForRayIntersection (0x100359b0) also deforms:
    // picking reads those positions on the CPU.
    uintptr_t findRenderDeformReturn(const char* who);

    // The skeleton pose update GrannyAdvanceTime runs for every active skeleton (thiscall on the skeleton (2 args),
    // ret 8): once per frame (frame counter at +0x78 against a global counter), parent first, then every bone
    // (count at +0x10, 300-byte bone states at +0x18; the deform's first argument points at the same skeleton).
    // `frameCounter` receives the global counter's address.
    uintptr_t findPose(const char* who, const uint32_t*& frameCounter);
    namespace Skeleton
    {
        constexpr uintptr_t boneCount = 0x10;
        constexpr uintptr_t active = 0x6C;          // byte
        constexpr uintptr_t parent = 0x74;          // skeleton this one is attached to, posed first
        constexpr uintptr_t posedFrame = 0x78;
    }

    using Mem::field;

    // Counts and pointers within the sanity limits.
    bool plausible(const uint8_t* mesh);

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

    // Per vertex, the influences Granny's deform gives its position: bone-major adds, then (overwriting) one rigid
    // bone or the vertex-major lists and their duplicates. `inf` gets at least vertexCount entries.
    void positionInfluences(const uint8_t* mesh, Influences& inf, uint8_t& paths, Problems& problems);
    // The same for normals: bone-major adds (vertex index space), then one rigid bone or the per-normal lists.
    void normalInfluences(const uint8_t* mesh, Influences& inf, uint8_t& paths, Problems& problems);
    bool sameInfluences(std::vector<Influence> a, std::vector<Influence> b);

    // out[v] = sum of w * (M v + t) (or M v without `translate`) over at most `limit` influences per vertex (the
    // largest, scaled to the full sum), for `count` float3 vertices from `src`. The bones are `palette` + binding *
    // `paletteStride` floats, kPaletteFloats each (a binding array: binding matrices, stride Binding::size / 4).
    void skin(const float* palette, uint32_t paletteStride, const Influences& inf, const float* src, uint32_t count,
        bool translate, uint32_t limit, float* out);
    // One vertex of the same.
    void skinVertex(const float* palette, uint32_t paletteStride, const std::vector<Influence>& list, const float* v,
        bool translate, float* out);

    // The bones of a mesh's bindings as skin() takes them.
    inline const float* bindingPalette(const uint8_t* mesh)
    {
        return reinterpret_cast<const float*>(field<const uint8_t*>(mesh, Mesh::bindings) + Binding::matrix);
    }
    constexpr uint32_t kBindingPaletteStride = Binding::size / 4;
}
