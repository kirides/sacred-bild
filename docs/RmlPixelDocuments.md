# RmlUi documents in screen pixels (design note)

Why: RmlUi documents are laid out in layout units (1px = one unit of the 1024 x 768 layout) and stretched to the screen by the renderer; the
text is rasterized at size x scale and shrunk back (`ScaledFontEngine`). Every box edge, border and glyph that falls on a fractional screen
pixel blurs or drifts by one pixel. A `dp` document (`ui.rml.load(path, {dp = true})`) is laid out in screen pixels with `1dp` = one layout
unit (the context's density ratio is the canvas scale), so text and boxes snap to real pixels.

## Done (this branch)

- `ports/rmlui` patches: glyphs unhinted; FreeType face sizes in 1/16 pixel (`freetype-no-hinting.patch`); RmlUi asks the font engine for the
  font size in 1/16 pixel instead of cutting it to whole pixels (`font-size-sixteenths.patch`); glyph advances rounded, not cut.
- `ScaledFontEngine` rasterizes at exactly `size * scale` (layout-unit documents) or exactly the asked size (dp documents).
  Layout-unit documents are pixel-identical to before apart from this.

## What a conversion of a mod means

1. **Units.** In a dp document `px` is a screen pixel and `dp` a layout unit. A stylesheet written for layout units therefore needs every
   length changed from `px` to `dp`, except hairlines (`1px` borders and lines stay one real pixel). `dp` also works in layout-unit
   documents (there it equals px), so a converted stylesheet runs in both modes: convert `sacred-theme/theme.rcss` first, then each mod.
   Numbers the Lua side writes into styles (`data-style-left="c.x"`, cell sizes) are layout units: they need a `dp` suffix.
2. **Art.** `gear:image/<n>` decorators and `<img>` take their natural size from the picture in pixels. In a layout-unit document that
   size is in layout units, which is right; in a dp document it would be too small by the canvas scale. Open decision: report the
   picture size times the scale from `RenderInterface::LoadTexture` for dp documents (RmlUi caches the size per source, so a picture used
   by both kinds of document needs a different cache key, for instance a `?dp` variant of the source that Gear adds itself), or write
   the sizes of the game's art explicitly in `dp` in the theme.
3. **Layout editor and `rml_move` / `rml_resize`.** They convert layout units to the document's pixels with `d.unit`; they already take
   dp documents into account, but each panel position and clamp must be checked in a dp document.
4. **Hit testing and the pad cursor** use `d.unit` as well; check the same.

## Plan

1. Theme conversion (`px` to `dp`, hairlines excepted) and the art decision (2).
2. One screen as a spike: Better SaveLoad loaded with `{dp = true}`, screenshot diff against the layout-unit version at the default 1600x900 only (John's standing rule: no multi-resolution testing).
   
3. A conversion script for the other mods' stylesheets (and the style values written by Lua), then one mod per commit with a
   before / after screenshot each.
