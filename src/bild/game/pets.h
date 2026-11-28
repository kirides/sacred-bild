#pragma once

// Pets for Sacred Gear's mods: objects that are drawn larger (the 3D model's world matrix is scaled about the
// object's own origin while the engine draws that object) and/or cannot be picked in the world (they are taken out of
// the world view's pick list, so a click, the hover cursor and the controller's aim assist never see them).
// Sacred Gear drives it through the exports SacredBild_PetSet / SacredBild_PetClear (see src/bild/exports.def).
#include <cstdint>

struct _D3DMATRIX;
struct IDirectDrawSurface7;
struct IDirect3DDevice7;

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

    // Tints (a colour multiplied over an object's model, whole or per mesh): a character's model is drawn as a few meshes, one
    // DrawIndexedPrimitiveStrided each, in a fixed order; `part` 0 is the whole model, 1.. the meshes in the order they are drawn.
    // While the engine draws an object that has tints (its drawModel, not its shadow), `tintNext` answers for each mesh draw in
    // turn whether it has a colour (0xAARRGGBB, white 0xFFFFFFFF: none).
    bool tinting();
    // Whether the meshes being drawn are an item icon of a type with textures of its own (SacredBild_IconBegin): the UI pass applies them too.
    bool iconTinting();
    // `argb` the colour (0: none) and `texture` the token of another texture for the mesh (0: none), true if either is set. (A mesh is
    // counted whether it has them or not.)
    bool tintNext(uint32_t& argb, uint32_t& texture);
    // The surface of a texture token, from Sacred Gear's resolver (render thread); nullptr if there is none.
    IDirectDrawSurface7* resolveTexture(IDirect3DDevice7* device, uint32_t token);

    // While the engine draws a scaled pet: multiplies the 3x3 part of a world matrix, the translation stays.
    void scaleWorld(_D3DMATRIX& m);
}
