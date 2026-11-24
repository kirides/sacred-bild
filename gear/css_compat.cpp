#include "css_compat.h"


#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/PropertyDefinition.h>
#include <RmlUi/Core/StyleSheetSpecification.h>

#include <algorithm>
#include <cctype>
#include <charconv>

namespace GearCss {
namespace {


std::string encodeUtf8(char32_t c)
{
    std::string out;
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
    return out;
}

constexpr const char* kGeneratedMarker = "data-css-content";

std::string_view trim(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

std::string lower(std::string_view s)
{
    std::string out(s);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Splits at top-level separators (not inside parentheses or quotes).
std::vector<std::string_view> splitTopLevel(std::string_view s, char separator)
{
    std::vector<std::string_view> parts;
    int depth = 0;
    char quote = 0;
    size_t start = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (quote) {
            if (c == '\\')
                ++i;
            else if (c == quote)
                quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '(') {
            ++depth;
        } else if (c == ')') {
            depth = std::max(0, depth - 1);
        } else if (c == separator && depth == 0) {
            parts.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    parts.push_back(s.substr(start));
    return parts;
}

std::string unquote(std::string_view s)
{
    s = trim(s);
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front())
        return decodeCssString(s);
    return std::string(s);
}

// Extracts the argument of fn(...) at the start of `item`, e.g. url("x") -> x.
std::optional<std::string> functionArgument(std::string_view item, std::string_view fn)
{
    item = trim(item);
    if (item.size() <= fn.size() + 1 || lower(item.substr(0, fn.size())) != fn || item[fn.size()] != '(')
        return std::nullopt;
    const auto close = item.find(')', fn.size());
    if (close == std::string_view::npos)
        return std::nullopt;
    return unquote(item.substr(fn.size() + 1, close - fn.size() - 1));
}

bool unsupportedFormat(std::string_view item, std::string_view url)
{
    const std::string l = lower(item);
    const std::string u = lower(url);
    return l.find("woff") != std::string::npos || l.find("embedded-opentype") != std::string::npos || l.find("svg") != std::string::npos
        || u.ends_with(".woff") || u.ends_with(".woff2") || u.ends_with(".eot") || u.ends_with(".svg");
}

void warn(const CssPreprocessHooks& hooks, std::string message)
{
    if (hooks.warnings)
        hooks.warnings->push_back(std::move(message));
}

// Rewrites the `src` declaration of one @font-face block body (text between the braces):
// the first resolvable, loadable source replaces the whole list. Line count is preserved.
std::string rewriteFontFace(std::string_view body, const CssPreprocessHooks& hooks)
{
    std::string out;
    const auto decls = splitTopLevel(body, ';');
    for (size_t d = 0; d < decls.size(); ++d) {
        const std::string_view decl = decls[d];
        const auto colon = decl.find(':');
        std::string piece(decl);
        if (colon != std::string_view::npos && lower(trim(decl.substr(0, colon))) == "src") {
            std::optional<std::string> resolved;
            for (std::string_view item : splitTopLevel(decl.substr(colon + 1), ',')) {
                item = trim(item);
                if (functionArgument(item, "local"))
                    continue; // local() system fonts: not supported
                std::optional<std::string> url = functionArgument(item, "url");
                if (!url) // bare or quoted path, possibly followed by format(...)
                    url = unquote(item.substr(0, item.find_first_of(" \t\n")));
                if (url->empty() || unsupportedFormat(item, *url))
                    continue;
                if (hooks.resolve_font)
                    resolved = hooks.resolve_font(*url);
                if (resolved)
                    break;
                warn(hooks, "@font-face: cannot resolve '" + *url + "'");
            }
            const auto newlines = static_cast<size_t>(std::ranges::count(decl, '\n'));
            piece = " src: " + (resolved ? *resolved : std::string("/@!unresolved-font")) + std::string(newlines, '\n');
        }
        out += piece;
        if (d + 1 < decls.size())
            out += ';';
    }
    return out;
}

} // namespace

std::string decodeCssString(std::string_view value)
{
    value = trim(value);
    if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front())
        value = value.substr(1, value.size() - 2);
    std::string out;
    for (size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (c != '\\' || i + 1 >= value.size()) {
            out += c;
            continue;
        }
        // \hhhhhh (1-6 hex digits, optionally followed by one whitespace) or \x
        size_t j = i + 1;
        while (j < value.size() && j - (i + 1) < 6 && std::isxdigit(static_cast<unsigned char>(value[j])))
            ++j;
        if (j == i + 1) {
            out += value[j];
            i = j;
            continue;
        }
        unsigned cp = 0;
        std::from_chars(value.data() + i + 1, value.data() + j, cp, 16);
        out += encodeUtf8(cp > 0x10FFFF || cp == 0 ? 0xFFFD : static_cast<char32_t>(cp));
        if (j < value.size() && (value[j] == ' ' || value[j] == '\t' || value[j] == '\n'))
            ++j;
        i = j - 1;
    }
    return out;
}

std::string preprocessStyleSheet(std::string_view css, const CssPreprocessHooks& hooks)
{
    std::string out;
    out.reserve(css.size());

    // Block stack: true = declaration block, false = group (@media etc.)
    std::vector<bool> blocks;
    size_t statement_start = 0; // start of the current selector / at-rule prelude in `css`
    size_t i = 0;
    while (i < css.size()) {
        const char c = css[i];
        // Comments
        if (c == '/' && i + 1 < css.size() && css[i + 1] == '*') {
            const auto end = css.find("*/", i + 2);
            const size_t stop = end == std::string_view::npos ? css.size() : end + 2;
            out += css.substr(i, stop - i);
            i = stop;
            continue;
        }
        // Strings
        if (c == '"' || c == '\'') {
            size_t j = i + 1;
            while (j < css.size() && css[j] != c) {
                if (css[j] == '\\')
                    ++j;
                ++j;
            }
            const size_t stop = std::min(css.size(), j + 1);
            out += css.substr(i, stop - i);
            i = stop;
            continue;
        }
        const bool in_declarations = !blocks.empty() && blocks.back();

        if (!in_declarations && c == '@' && lower(css.substr(i, 10)) == "@font-face") {
            const auto open = css.find('{', i);
            const auto close = open == std::string_view::npos ? std::string_view::npos : css.find('}', open);
            if (close == std::string_view::npos) {
                out += css.substr(i);
                break;
            }
            const std::string_view body = css.substr(open + 1, close - open - 1);
            // Re-parse to know the family even if it is declared after src.
            std::string family;
            for (std::string_view decl : splitTopLevel(body, ';'))
                if (const auto colon = decl.find(':'); colon != std::string_view::npos && lower(trim(decl.substr(0, colon))) == "font-family")
                    family = unquote(decl.substr(colon + 1));
            CssPreprocessHooks local = hooks;
            local.on_font_face = nullptr;
            std::string rewritten = rewriteFontFace(body, local);
            if (hooks.on_font_face && !family.empty()) {
                // Find the resolved src again from the rewritten text.
                for (std::string_view decl : splitTopLevel(rewritten, ';'))
                    if (const auto colon = decl.find(':'); colon != std::string_view::npos && lower(trim(decl.substr(0, colon))) == "src") {
                        const std::string path(trim(decl.substr(colon + 1)));
                        if (!path.starts_with("/@!"))
                            hooks.on_font_face(family, path);
                    }
            }
            out += css.substr(i, open + 1 - i);
            out += rewritten;
            out += '}';
            i = close + 1;
            statement_start = i;
            continue;
        }

        if (c == '{') {
            // A group rule (@media, @supports ...) contains rules, everything else declarations.
            const std::string prelude = lower(trim(css.substr(statement_start, i - statement_start)));
            const bool group = prelude.starts_with("@media") || prelude.starts_with("@supports") || prelude.starts_with("@layer");
            blocks.push_back(!group && !in_declarations);
            out += c;
            ++i;
            statement_start = i;
            continue;
        }
        if (c == '}') {
            if (!blocks.empty())
                blocks.pop_back();
            out += c;
            ++i;
            statement_start = i;
            continue;
        }
        if (c == ';' && !in_declarations) {
            out += c;
            ++i;
            statement_start = i;
            continue;
        }
        // Selectors: drop ::before / :before (content applies to the element itself).
        if (!in_declarations && c == ':') {
            const std::string ahead = lower(css.substr(i, 8));
            if (ahead.starts_with("::before")) {
                out += "        "; // keep column positions
                i += 8;
                continue;
            }
            if (ahead.starts_with(":before")) {
                out += "       ";
                i += 7;
                continue;
            }
            if (ahead.starts_with("::after") || ahead.starts_with(":after"))
                warn(hooks, "::after is not supported (only ::before, which maps to the element itself)");
        }
        out += c;
        ++i;
    }
    return out;
}

std::string preprocessRmlDocument(std::string_view rml, const CssPreprocessHooks& hooks)
{
    std::string out;
    size_t pos = 0;
    while (true) {
        // Case-insensitive search for <style
        size_t open = std::string_view::npos;
        for (size_t k = pos; k + 6 <= rml.size(); ++k) {
            if (rml[k] == '<' && lower(rml.substr(k + 1, 5)) == "style" && (k + 6 == rml.size() || rml[k + 6] == '>' || std::isspace(static_cast<unsigned char>(rml[k + 6])))) {
                open = k;
                break;
            }
        }
        if (open == std::string_view::npos)
            break;
        const auto content_start = rml.find('>', open);
        if (content_start == std::string_view::npos)
            break;
        size_t close = std::string_view::npos;
        for (size_t k = content_start; k + 8 <= rml.size(); ++k)
            if (rml[k] == '<' && rml[k + 1] == '/' && lower(rml.substr(k + 2, 5)) == "style") {
                close = k;
                break;
            }
        if (close == std::string_view::npos)
            break;
        out += rml.substr(pos, content_start + 1 - pos);
        out += preprocessStyleSheet(rml.substr(content_start + 1, close - content_start - 1), hooks);
        pos = close;
    }
    out += rml.substr(pos);
    return out;
}

// ============================================================================================
// content property
// ============================================================================================
CssContentSupport::CssContentSupport(GlyphLookup lookup) : lookup_(std::move(lookup)) {}

void CssContentSupport::registerProperty()
{
    property_id_ = Rml::StyleSheetSpecification::GetPropertyId("content");
    if (property_id_ == Rml::PropertyId::Invalid) {
        Rml::StyleSheetSpecification::RegisterProperty("content", "none", false, true).AddParser("string");
        property_id_ = Rml::StyleSheetSpecification::GetPropertyId("content");
    }
}

void CssContentSupport::apply(Rml::Context* context)
{
    if (!context || property_id_ == Rml::PropertyId::Invalid)
        return;
    for (int i = 0; i < context->GetNumDocuments(); ++i)
        applyElement(context->GetDocument(i));
}

void CssContentSupport::applyElement(Rml::Element* element)
{
    if (!element || dynamic_cast<Rml::ElementText*>(element))
        return;

    Rml::Element* first = element->GetFirstChild();
    auto* generated = (first && first->HasAttribute(kGeneratedMarker)) ? static_cast<Rml::ElementText*>(first) : nullptr;

    std::string text;
    bool want = false;
    if (const Rml::Property* p = element->GetProperty(property_id_)) {
        const std::string raw = p->Get<Rml::String>();
        const std::string_view trimmed = trim(raw);
        if (!trimmed.empty() && lower(trimmed) != "none" && lower(trimmed) != "normal") {
            want = true;
            text = decodeCssString(trimmed);
            if (lookup_)
                if (auto glyph = lookup_(element->GetComputedValues().font_family(), text))
                    text = *glyph;
        }
    }

    if (want) {
        if (!generated) {
            if (Rml::ElementDocument* doc = element->GetOwnerDocument()) {
                Rml::ElementPtr node = doc->CreateTextNode(text);
                node->SetAttribute(kGeneratedMarker, true);
                element->InsertBefore(std::move(node), element->GetFirstChild());
            }
        } else if (generated->GetText() != text) {
            generated->SetText(text);
        }
    } else if (generated) {
        element->RemoveChild(generated);
    }

    for (int i = 0; i < element->GetNumChildren(); ++i) {
        Rml::Element* child = element->GetChild(i);
        if (!child->HasAttribute(kGeneratedMarker))
            applyElement(child);
    }
}

} // namespace GearCss
