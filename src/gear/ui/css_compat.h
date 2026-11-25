#pragma once
// CssCompat: web-CSS conveniences RmlUi lacks, implemented without touching RmlUi.
//
// 1. Stylesheet pre-processing (applied to .rcss files and <style> blocks of .rml files
//    as they are read through the virtual file system):
//    * @font-face { font-family: "X"; src: url("x.ttf") format("truetype"), url(x.woff2); }
//      RmlUi supports @font-face but passes `src` verbatim to the file loader - no url(),
//      no format(), no resolution relative to the stylesheet. The pre-processor resolves the
//      first usable source through a callback and rewrites `src` to that resolved path.
//    * `selector::before` / `selector:before` become `selector`; together with the `content`
//      property (below) this makes icon-font CSS work:  .md-home::before { content: "\e88a"; }
//    Line structure is preserved, so RmlUi's error line numbers still match the file.
//
// 2. The CSS `content` property, applied to the element itself (RmlUi has no pseudo
//    elements): a generated text node is kept as the element's first child. Values support
//    CSS escapes ("\e88a"), plain text, `none`, and - for fonts with ligature tables - icon
//    names ("home"), which RmlUi could not shape on its own.

#include <RmlUi/Core/ID.h>
#include <RmlUi/Core/Types.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Rml {
class Context;
class Element;
} // namespace Rml

namespace GearCss {

struct CssPreprocessHooks {
    // Resolves a font source as written in url(...) to a loadable path, or nullopt.
    std::function<std::optional<std::string>(std::string_view url)> resolve_font;
    // Called for each @font-face with its family and resolved path.
    std::function<void(const std::string& family, const std::string& resolved_path)> on_font_face;
    std::vector<std::string>* warnings = nullptr;
};

[[nodiscard]] std::string preprocessStyleSheet(std::string_view css, const CssPreprocessHooks& hooks);
// Applies preprocessStyleSheet to the contents of every <style> element.
[[nodiscard]] std::string preprocessRmlDocument(std::string_view rml, const CssPreprocessHooks& hooks);

// Decodes a CSS string value: strips quotes, resolves \hex escapes and \x escapes.
[[nodiscard]] std::string decodeCssString(std::string_view value);

class CssContentSupport {
public:
    // Maps (font-family, text) to replacement text, e.g. a ligature name to its glyph.
    using GlyphLookup = std::function<std::optional<std::string>(std::string_view family, std::string_view text)>;

    explicit CssContentSupport(GlyphLookup lookup = {});

    // Registers the `content` property. Call after Rml::Initialise, before style sheets load.
    void registerProperty();
    // Synchronises generated content for all documents of a context. Call every frame
    // before Context::Update (new content is laid out by that update).
    void apply(Rml::Context* context);

private:
    void applyElement(Rml::Element* element);

    GlyphLookup lookup_;
    Rml::PropertyId property_id_ = Rml::PropertyId::Invalid;
};

} // namespace GearCss
