#pragma once
// Textures of an item type for its inventory icon (gear.inv.set_type_texture): the game draws an icon through the model renderer's drawItem
// (ENG 0x40E9C0, the same call Gear's own item icons make) for an item type, and this hook brackets it for SacredBild, whose device applies the
// textures to the meshes of the draw.
namespace ItemTexture
{
    bool install();
}
