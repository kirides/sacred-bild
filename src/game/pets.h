#pragma once

// Pets for Sacred Gear's mods: objects that are drawn larger (the 3D model's world matrix is scaled about the
// object's own origin while the engine draws that object) and/or cannot be picked in the world (they are taken out of
// the world view's pick list, so a click, the hover cursor and the controller's aim assist never see them).
// Sacred Gear drives it through the exports SacredBild_PetSet / SacredBild_PetClear (see src/exports.def).
#include <cstdint>

struct _D3DMATRIX;

namespace Sacred
{
    struct cWorldView;
}

namespace Pets
{
    // Hooks cObject3D::drawModel (the scale scope). The pick filter is called by AimAssist's world pick hook.
    void install();

    // Removes the untargetable objects from the view's pick list (under its lock).
    void filterPick(Sacred::cWorldView* view);

    // Whether any pet is registered (cheap, lock free).
    bool any();

    // Whether the engine is drawing a scaled pet's model on this thread now.
    bool scaling();

    // While the engine draws a scaled pet: multiplies the 3x3 part of a world matrix, the translation stays.
    void scaleWorld(_D3DMATRIX& m);
}
