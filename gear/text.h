#pragma once
#include <cstdint>
#include <string>

struct IDirect3DDevice7;

// Mod text in a vector font, rasterized for the screen's pixels: the UI's layout is drawn scaled to the screen
// (SacredBild's canvas), so text in the game's bitmap fonts comes out stretched and soft. Here each font is rasterized
// (GDI, grayscale antialiasing) at its size times the canvas scale into glyph atlases, and every glyph is drawn 1:1 on
// the screen's pixel grid. Sizes and positions stay in layout units.
namespace Text
{
    enum Flags : uint32_t
    {
        Bold = 1,
        Italic = 2,
        Shadow = 4,     // a 1-pixel dark shadow down and right
        Outline = 8,    // a 1-pixel dark outline
    };

    struct Style
    {
        std::wstring face;          // empty: the default UI face
        float size = 13.0f;         // the em height in layout units
        uint32_t flags = 0;
    };

    // Screen pixels per layout unit and where layout (0, 0) lies on the screen, for the frame the caller draws in.
    // Without SacredBild 1, 0, 0.
    struct Placement
    {
        float scale = 1.0f, originX = 0.0f, originY = 0.0f;

        float toLayoutX(float px) const { return (px - originX) / scale; }
        float toLayoutY(float py) const { return (py - originY) / scale; }
        float toScreenX(float x) const { return x * scale + originX; }
        float toScreenY(float y) const { return y * scale + originY; }
    };

    // The widest line and the lines' height, in layout units, at the canvas scale `scale`. Any thread.
    bool measure(const Style& style, const std::wstring& text, float scale, float& width, float& height);

    // Draws `text` with its top-left at (x, y); a width > 0 aligns each line in it (0 left, 1 center, 2 right); a
    // height > 0 places the lines in it (valign 0 top, 1 middle: the capitals centred, 2 bottom); valign 3: y is the
    // first line's baseline.
    // Render thread, inside the overlay's device states (alpha blending on, stage 0 modulating texture and
    // diffuse); sets textures on stage 0 through `setTexture`.
    using SetTextureFn = void (*)(void* ctx, void* surface);
    void draw(IDirect3DDevice7* device, const Placement& placement, const Style& style, const std::wstring& text,
              float x, float y, float width, float height, int align, int valign, uint32_t color,
              SetTextureFn setTexture, void* ctx);

    // Snaps a layout coordinate to the screen's pixel grid.
    inline float snap(float screen) { return static_cast<float>(static_cast<int>(screen + (screen >= 0 ? 0.5f : -0.5f))); }
    inline float snapX(const Placement& p, float x) { return p.toLayoutX(snap(p.toScreenX(x))); }
    inline float snapY(const Placement& p, float y) { return p.toLayoutY(snap(p.toScreenY(y))); }
}
