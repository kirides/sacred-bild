#pragma once

// [Debug] SkinCheck: groundwork for skinning characters on the GPU. Granny 1.x deforms every character mesh on the
// CPU (granny.dll FUN_1001e660, ~20 % of the render thread zoomed out, and again for each character's shadow). This
// hook lets it run, rebuilds per-vertex bone weights from the mesh's influence lists, skins the mesh again with
// Granny's own bone matrices (with all influences, and with at most four per vertex as a vertex shader would) and
// logs how far both are from Granny's result, plus what a GPU path has to handle: bones per mesh, influences per
// vertex, normals weighted differently from their positions.
namespace SkinCheck
{
    // Queues the hook in the caller's Patch transaction (granny.dll is mapped as an import of the exe by then).
    void install();
}
