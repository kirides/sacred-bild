#pragma once

// [UI] NativeFonts (Backend=nvrhi): the game's text at the UI's scale instead of stretched from 1024x768.
//
// The game makes its text from 256x256 font pages: for every character a font shows for the first time, the glyph
// rasterizer (cFontTTF2, ENG 0x651CB0) draws it with GDI into a small system memory surface and the glyph copy
// (0x64E860) puts it into a free cell of a page (16-bit, white, alpha from the gray level); text is drawn as quads
// with the cells' texture coordinates, in 1024x768 units, and the UI canvas stretches them. Here every glyph the game
// copies is drawn again with GDI in the same font at the UI's scale (the menus' canvas, in steps of 1/8) and put into
// the same cell, scaled, of a sharper copy of the page (BGRA8, 256 * scale). When a page is drawn from, the device
// samples the copy: the texture coordinates are relative, so they hit the same cells, and the layout, line breaks
// and hit boxes stay the game's. A change of scale (another window size) draws the page's glyphs again.
namespace FontShadow
{
    // Queues the hooks in the caller's Patch transaction (NVRHI backend only).
    void install();
}
