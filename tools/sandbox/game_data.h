#pragma once
#include <cstdint>
#include <string>
#include <vector>

// The game's UI pictures without the game: read straight from the files of a Sacred Gold install (read only, nothing is written there).
//
// - Sacred.exe holds the UI image table (sUITextureDef, 0x54-byte records from "INVALID" to "INVALID_MAX", the ids gear.ui.image and
//   gear:image/N use): +0x08 the TGA sheet's name, +0x2C..+0x38 the image's rectangle in sheet pixels (left, top, right, bottom, inclusive).
// - PAK\Texture.TMP lists every texture of PAK\texture.pak and its overlays Texture00..15.pak (0x118 bytes of header, then 0x50-byte
//   records): +0x00 name, +0x20 / +0x22 width / height, +0x24 storage type, +0x29 archive slot (0 texture.pak, n: Texture{n-1}.pak),
//   +0x2A the offset of the entry's header in that archive. The record's index is the texture manager's handle (Ui::textureImage).
// - An archive entry is that 0x50-byte header again, then the pixels: type 0 raw A4R4G4B4, 4 zlib-compressed A4R4G4B4, 6 raw A8R8G8B8
//   (the only types Gold's archives use), rows top to bottom.
namespace GameData
{
    struct UiImage
    {
        std::string sheet;              // TGA name as the table writes it
        int left = 0, top = 0, right = 0, bottom = 0;   // sheet pixels, inclusive
        int width = 0, height = 0;      // right - left + 1, bottom - top + 1
    };

    struct Pixels
    {
        int width = 0, height = 0;
        std::vector<uint32_t> argb;     // 0xAARRGGBB, rows top to bottom
    };

    // Reads the tables of the install at `gameDir` (Sacred.exe, PAK\Texture.TMP). False with `error` set if a file is missing or unreadable.
    bool open(const std::wstring& gameDir, std::string& error);

    // UI image ids 0 .. count-1 (0 is INVALID).
    const std::vector<UiImage>& uiImages();

    // The Texture.TMP index of a texture by its name (any case), -1 if there is none.
    int textureIndex(const std::string& name);
    int textureCount();
    std::string textureName(int index);
    bool textureSize(int index, int& width, int& height);

    // Decodes texture `index` (thread safe; the archives stay open).
    bool loadTexture(int index, Pixels& out, std::string& error);
}
