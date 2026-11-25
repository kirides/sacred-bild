#include "rml.h"
#include "log.h"
#include "padnav.h"
#include "css_compat.h"
#include "gear_res.h"
#include "ui.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/DataTypes.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/DataVariable.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>

#include <lua.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <cstring>
#include <memory>
#include <optional>
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// stb_image decodes the PNG / TGA / JPG files a document names. Its warnings are not ours.
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#define STBI_NO_STDIO
#include <stb_image.h>

#include "dds.h"
#include "vfs.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace fs = std::filesystem;

namespace
{
    constexpr int kLayoutWidth = 1024, kLayoutHeight = 768;     // the UI's layout, in which a context is as large as the canvas
    constexpr size_t kMaxDocuments = 64;
    constexpr int kMaxMeshesPerDraw = 1500;                     // below the Lua side's limit of draw commands per mod
    constexpr size_t kMaxFileSize = 64u * 1024 * 1024;

    // ---- Paths: RmlUi reads files of the mod's own folder (and the system's font files while it loads fonts) ----

    std::wstring g_modsDir;
    fs::path g_root;            // the folder the current call may read from (empty: none)
    bool g_fontDp = false;      // the document being handled is laid out in screen pixels (its text is rasterized as asked, not scaled)
    fs::path g_themeRoot;       // the theme mod's folder, readable too while a document that uses the theme is handled
    bool g_allowFonts = false;  // the Windows font folder is readable too

    fs::path fromUtf8(const std::string& s)
    {
        return fs::path(std::u8string(s.begin(), s.end()));
    }

    // The documents' own URLs are "file:///B:/..." (see create) and the links in them resolve against that: back to a path.
    std::string plainPath(const std::string& s)
    {
        return s.compare(0, 8, "file:///") == 0 ? s.substr(8) : s;
    }

    std::string toUtf8(const fs::path& p)
    {
        const std::u8string u = p.generic_u8string();
        return std::string(u.begin(), u.end());
    }

    bool inside(const fs::path& root, const fs::path& path)
    {
        std::error_code ec;
        const fs::path r = fs::weakly_canonical(root, ec);
        if (ec)
        {
            return false;
        }
        const fs::path p = fs::weakly_canonical(path, ec);
        if (ec)
        {
            return false;
        }
        const fs::path rel = p.lexically_relative(r);
        return !rel.empty() && *rel.begin() != ".." && !rel.is_absolute();
    }

    fs::path fontsDir()
    {
#ifdef _WIN32
        wchar_t dir[MAX_PATH] = {};
        const UINT n = GetWindowsDirectoryW(dir, MAX_PATH);
        if (n > 0 && n < MAX_PATH)
        {
            return fs::path(dir) / L"Fonts";
        }
        return fs::path(L"C:\\Windows\\Fonts");
#else
        return fs::path("/usr/share/fonts");
#endif
    }

    // Hot reload: the files the document being (re)loaded pulls in, with their write times (null while no document loads).
    std::map<fs::path, fs::file_time_type>* g_deps = nullptr;
    bool g_hotReload = false;

    void recordDep(const fs::path& path)
    {
        if (!g_deps)
        {
            return;
        }
        std::error_code ec;
        const fs::file_time_type t = fs::last_write_time(path, ec);
        if (!ec)
        {
            (*g_deps)[path] = t;
        }
    }

    bool readable(const fs::path& path)
    {
        if (!g_root.empty() && inside(g_root, path))
        {
            return true;
        }
        if (!g_themeRoot.empty() && inside(g_themeRoot, path))
        {
            return true;
        }
        return g_allowFonts && inside(fontsDir(), path);
    }

    // The mod's folder, "" if `dir` is no mod folder name (a name only: no separators, no drive, no "..").
    fs::path modRoot(const std::string& dir)
    {
        if (dir.empty() || dir == "." || dir == ".." || dir.find_first_of("\\/:") != std::string::npos || g_modsDir.empty())
        {
            return {};
        }
        return fs::path(g_modsDir) / fromUtf8(dir);
    }

    // ---- Shared files between mods: "@mod-id/path" ----
    // A mod lists what it shares in mod.lua (`exports = {'templates/', 'styles/*.rcss', 'images/', 'fonts/', 'file.ext'}`, "*" for all) and, while it
    // runs, any document can name those files as @mod-id/path (a link, a template, an image, a font). Nothing else of that mod is reachable, and no
    // path leaves its folder.
    struct Export
    {
        std::string dir;
        std::vector<std::string> patterns;
    };
    std::map<std::string, Export> g_exports;     // mod id -> its folder name and patterns

    std::string lowered(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    bool exportMatches(const std::string& pattern, const std::string& rel)
    {
        const std::string p = lowered(pattern), r = lowered(rel);
        if (p == "*")
        {
            return true;
        }
        if (!p.empty() && p.back() == '/')
        {
            return r.compare(0, p.size(), p) == 0;
        }
        const size_t star = p.find('*');
        if (star == std::string::npos)
        {
            return p == r;
        }
        const size_t slash = p.rfind('/');
        const std::string dirPart = slash == std::string::npos ? std::string() : p.substr(0, slash + 1);
        const std::string name = p.substr(slash == std::string::npos ? 0 : slash + 1);       // "*.ext" (one star)
        const std::string suffix = name.substr(name.find('*') + 1);
        if (r.compare(0, dirPart.size(), dirPart) != 0)
        {
            return false;
        }
        const std::string rest = r.substr(dirPart.size());
        return rest.find('/') == std::string::npos && rest.size() >= suffix.size() && rest.compare(rest.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    // "@mod-id/some/file" -> the file on disk, if the mod runs and exports it; empty otherwise. `isShared` tells the text was a reference at all.
    fs::path resolveShared(const std::string& text, bool* isShared = nullptr)
    {
        if (isShared)
        {
            *isShared = !text.empty() && text[0] == '@';
        }
        if (text.empty() || text[0] != '@')
        {
            return {};
        }
        const size_t slash = text.find('/');
        if (slash == std::string::npos || g_modsDir.empty())
        {
            return {};
        }
        const auto it = g_exports.find(text.substr(1, slash - 1));
        std::string rel = text.substr(slash + 1);
        const size_t query = rel.find('?');
        if (query != std::string::npos)
        {
            rel.resize(query);
        }
        if (it == g_exports.end() || rel.empty() || rel.find("..") != std::string::npos || rel.find(':') != std::string::npos || rel.find('\\') != std::string::npos || rel[0] == '/')
        {
            return {};
        }
        for (const std::string& pattern : it->second.patterns)
        {
            if (exportMatches(pattern, rel))
            {
                const fs::path root = modRoot(it->second.dir);
                const fs::path file = root / fromUtf8(rel);
                return !root.empty() && inside(root, file) ? file : fs::path();
            }
        }
        return {};
    }

    // The mod folder a file of the mods folder lies in (where its packs are, src/gear/vfs.h), empty for any other file.
    fs::path packRootOf(const fs::path& file)
    {
        if (g_modsDir.empty())
        {
            return {};
        }
        const fs::path rel = file.lexically_normal().lexically_relative(fs::path(g_modsDir).lexically_normal());
        if (rel.empty() || rel.is_absolute() || *rel.begin() == ".." || std::distance(rel.begin(), rel.end()) < 2)
        {
            return {};
        }
        return fs::path(g_modsDir) / *rel.begin();
    }

    // A file that is not on disk, from the mod's packs.
    bool readPacked(const fs::path& path, std::string& out)
    {
        const fs::path root = packRootOf(path);
        return !root.empty() && Gear::Vfs::read(root, path, out);
    }

    bool readAll(const fs::path& path, std::string& out, bool trusted = false)
    {
        if (!trusted && !readable(path))
        {
            return false;
        }
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            return readPacked(path, out);
        }
        recordDep(path);
        in.seekg(0, std::ios::end);
        const std::streamoff size = in.tellg();
        if (size < 0 || static_cast<uint64_t>(size) > kMaxFileSize)
        {
            return false;
        }
        in.seekg(0, std::ios::beg);
        out.resize(static_cast<size_t>(size));
        in.read(out.data(), size);
        return static_cast<std::streamoff>(in.gcount()) == size;
    }

    // ---- The interfaces ----

    class SystemInterface final : public Rml::SystemInterface
    {
    public:
        double GetElapsedTime() override
        {
            using Clock = std::chrono::steady_clock;
            static const Clock::time_point start = Clock::now();
            return std::chrono::duration<double>(Clock::now() - start).count();
        }

        // "@mod-id/file" is a complete reference, whatever document names it; the links inside a shared file are joined to it as usual.
        void JoinPath(Rml::String& translated_path, const Rml::String& document_path, const Rml::String& path) override
        {
            if (!path.empty() && path[0] == '@')
            {
                translated_path = path;
                return;
            }
            Rml::SystemInterface::JoinPath(translated_path, document_path, path);
        }

        // The Windows clipboard (Unicode text) for the text fields' Ctrl+C / Ctrl+X / Ctrl+V.
        void SetClipboardText(const Rml::String& text) override
        {
            const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            if (n < 0 || text.size() > (1u << 20))
            {
                return;
            }
            HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (static_cast<size_t>(n) + 1) * sizeof(wchar_t));
            if (!memory)
            {
                return;
            }
            if (auto* dst = static_cast<wchar_t*>(GlobalLock(memory)))
            {
                if (n > 0)
                {
                    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), dst, n);
                }
                dst[n] = 0;
                GlobalUnlock(memory);
                if (OpenClipboard(nullptr))
                {
                    EmptyClipboard();
                    if (SetClipboardData(CF_UNICODETEXT, memory))
                    {
                        memory = nullptr;           // the clipboard owns it now
                    }
                    CloseClipboard();
                }
            }
            if (memory)
            {
                GlobalFree(memory);
            }
        }

        void GetClipboardText(Rml::String& text) override
        {
            text.clear();
            if (!OpenClipboard(nullptr))
            {
                return;
            }
            if (HANDLE data = GetClipboardData(CF_UNICODETEXT))
            {
                if (const auto* src = static_cast<const wchar_t*>(GlobalLock(data)))
                {
                    const int n = WideCharToMultiByte(CP_UTF8, 0, src, -1, nullptr, 0, nullptr, nullptr);
                    if (n > 1)
                    {
                        text.resize(static_cast<size_t>(n) - 1);
                        WideCharToMultiByte(CP_UTF8, 0, src, -1, text.data(), n, nullptr, nullptr);
                    }
                    GlobalUnlock(data);
                }
            }
            CloseClipboard();
        }

        bool LogMessage(Rml::Log::Type type, const Rml::String& message) override
        {
            const char* kind = type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT ? "error"
                               : type == Rml::Log::LT_WARNING                            ? "warning"
                                                                                         : "info";
            LOG("RmlUi {}: {}", kind, message);
            return true;
        }
    };

    // The directory part of a document-relative reference ("@id/styles/a.rcss" -> "@id/styles"; a disk path -> its folder).
    std::string logicalDir(const std::string& ref, const fs::path& real)
    {
        if (!ref.empty() && ref[0] == '@')
        {
            const size_t slash = ref.rfind('/');
            return slash == std::string::npos ? ref : ref.substr(0, slash);
        }
        return toUtf8(real.parent_path());
    }

    bool fileExists(const std::string& ref)
    {
        bool shared = false;
        const fs::path p = resolveShared(ref, &shared);
        std::error_code ec;
        if (shared)
        {
            return !p.empty() && fs::is_regular_file(p, ec);
        }
        const fs::path plain = fromUtf8(plainPath(ref));
        return readable(plain) && fs::is_regular_file(plain, ec);
    }

    // The web conveniences of a stylesheet (and of the <style> blocks of a document): @font-face with url() and format(), ::before with content.
    // A font source is looked up next to the stylesheet, in its fonts/ folder, in the mod's fonts/ folder, or as @mod-id/file.
    std::string preprocessCss(const std::string& text, const std::string& dir, bool markup)
    {
        GearCss::CssPreprocessHooks hooks;
        hooks.resolve_font = [dir](std::string_view url) -> std::optional<std::string> {
            std::string u(url);
            const size_t query = u.find_first_of("?#");
            if (query != std::string::npos)
            {
                u.resize(query);
            }
            if (u.empty())
            {
                return std::nullopt;
            }
            if (u[0] == '@')
            {
                return fileExists(u) ? std::optional<std::string>(u) : std::nullopt;
            }
            if (u.compare(0, 2, "./") == 0)
            {
                u.erase(0, 2);
            }
            const std::string rootDir = g_root.empty() ? std::string() : toUtf8(g_root);
            std::vector<std::string> bases = {dir + "/", dir + "/fonts/"};
            if (!rootDir.empty())
            {
                bases.push_back(rootDir + "/fonts/");
                bases.push_back(rootDir + "/");
            }
            for (const std::string& base : bases)
            {
                const std::string candidate = base + u;
                if (u.find("..") == std::string::npos && fileExists(candidate))
                {
                    return candidate;
                }
            }
            return std::nullopt;
        };
        return markup ? GearCss::preprocessRmlDocument(text, hooks) : GearCss::preprocessStyleSheet(text, hooks);
    }

    bool hasExtension(const fs::path& p, const char* ext)
    {
        return lowered(toUtf8(p.extension())) == ext;
    }

    class FileInterface final : public Rml::FileInterface
    {
    public:
        Rml::FileHandle Open(const Rml::String& path) override
        {
            bool shared = false;
            fs::path p = resolveShared(path, &shared);
            if (shared ? p.empty() : false)
            {
                LOG("RmlUi: {} is not shared (the mod does not run, or does not export it)", path);
                return 0;
            }
            if (!shared)
            {
                p = fromUtf8(plainPath(path));
                if (!readable(p))
                {
                    LOG("RmlUi: a document may not read {}", path);
                    return 0;
                }
            }
            auto file = std::make_unique<Handle>();
            file->in.open(p, std::ios::binary);
            std::string packed;
            const bool fromPack = !file->in && readPacked(p, packed);
            if (!file->in && !fromPack)
            {
                return 0;
            }
            if (!fromPack)
            {
                recordDep(p);
            }
            const bool css = hasExtension(p, ".rcss"), rml = hasExtension(p, ".rml");
            if (fromPack && !css && !rml)
            {
                file->mem = std::move(packed);
                file->memory = true;
            }
            else if (css || rml)
            {
                // read through the pre-processor (a style sheet may also be a template with <style>)
                std::string text = fromPack ? std::move(packed) : std::string((std::istreambuf_iterator<char>(file->in)), std::istreambuf_iterator<char>());
                file->in.close();
                file->mem = preprocessCss(text, logicalDir(shared ? path : plainPath(path), p), rml);
                file->memory = true;
            }
            return reinterpret_cast<Rml::FileHandle>(file.release());
        }

        void Close(Rml::FileHandle file) override { delete handle(file); }

        size_t Read(void* buffer, size_t size, Rml::FileHandle file) override
        {
            Handle* h = handle(file);
            if (h->memory)
            {
                const size_t n = std::min(size, h->mem.size() - std::min(h->mem.size(), h->pos));
                std::memcpy(buffer, h->mem.data() + h->pos, n);
                h->pos += n;
                return n;
            }
            h->in.read(static_cast<char*>(buffer), static_cast<std::streamsize>(size));
            return static_cast<size_t>(h->in.gcount());
        }

        bool Seek(Rml::FileHandle file, long offset, int origin) override
        {
            Handle* h = handle(file);
            if (h->memory)
            {
                const long base = origin == 1 ? static_cast<long>(h->pos) : origin == 2 ? static_cast<long>(h->mem.size()) : 0;
                const long target = base + offset;
                if (target < 0 || static_cast<size_t>(target) > h->mem.size())
                {
                    return false;
                }
                h->pos = static_cast<size_t>(target);
                return true;
            }
            h->in.clear();
            const std::ios::seekdir dir = origin == 1 ? std::ios::cur : origin == 2 ? std::ios::end : std::ios::beg;
            h->in.seekg(offset, dir);
            return !h->in.fail();
        }

        size_t Tell(Rml::FileHandle file) override
        {
            Handle* h = handle(file);
            if (h->memory)
            {
                return h->pos;
            }
            const std::streamoff p = h->in.tellg();
            return p < 0 ? 0 : static_cast<size_t>(p);
        }

    private:
        struct Handle
        {
            std::ifstream in;
            std::string mem;
            size_t pos = 0;
            bool memory = false;
        };
        static Handle* handle(Rml::FileHandle file) { return reinterpret_cast<Handle*>(file); }
    };

    // Where the render interface puts what the current draw records (set by rml_draw).
    struct Target
    {
        uint8_t layer = 1;
        int8_t window = -1;
        float unit = 1.0f;          // RmlUi pixels per layout unit
        bool scissor = false;
        float clip[4] = {};         // layout units
        Rml::Vector2f origin{0.0f, 0.0f};   // where the context's (0, 0) lies in layout pixels (a document that starts at the screen's edge)
        int count = 0;
        std::shared_ptr<const std::array<float, 16>> matrix;    // the element's transform (SetTransform), null: none
    };
    Target g_target;

    struct TextureInfo
    {
        uint32_t id = 0;            // a game image id (picture == false) or a picture id
        uint8_t flip = 0;
        bool picture = false;
        bool point = false;         // "?point": nearest-texel sampling (no bilinear blur)
        bool dp = false;            // "?dp": the picture's natural size is in layout units, not pixels (for documents laid out in screen pixels): its size is scaled by the canvas
        bool item = false;          // "gear:item/...": an item's model, drawn by the game's model renderer into the box of the geometry
        uint32_t itemType = 0;      // the item type to draw (a temporary item, single player only) ...
        uint32_t itemObject = 0;    // ... or the live item object
        uint32_t shared = 0;        // a shared texture (gear.ui.texture): its id in g_shared; the picture belongs to the registry, not to this handle
        std::string source;         // a file image's source as RmlUi knows it (empty for the game's own): for releasing it when it is not drawn any more
        uint64_t lastDraw = 0;      // GetTickCount64 of its last draw, or of its load
    };

    // "gear:image/54" (the game's UI image 54), "gear:texture/GUI_MOVE_RUNDUM.TGA" (a texture of the game's
    // Texture.pak); a "?flip=h", "?flip=v" or "?flip=hv" mirrors it.
    bool parseGearTexture(const std::string& source, TextureInfo& info)
    {
        std::string s = source.substr(5);
        info.flip = 0;
        info.point = false;
        info.dp = false;
        const size_t q = s.find('?');
        if (q != std::string::npos)
        {
            std::string opt = s.substr(q + 1);
            s.resize(q);
            // "?dp" (the size in layout units), alone or after / before the others, e.g. "?dp&point"
            for (const std::string flag : {"&dp", "dp&", "dp"})
            {
                const size_t at = opt.find(flag);
                if (at != std::string::npos && (flag != "dp" || opt == "dp"))
                {
                    opt.erase(at, flag.size());
                    info.dp = true;
                    break;
                }
            }
            // "?point" (nearest-texel sampling), alone or after / before a flip: "?flip=h&point"
            for (const std::string flag : {"&point", "point&", "point"})
            {
                const size_t at = opt.find(flag);
                if (at != std::string::npos && (flag != "point" || opt == "point"))
                {
                    opt.erase(at, flag.size());
                    info.point = true;
                    break;
                }
            }
            // "?rot=90" (clockwise), "?rot=180", "?rot=270": the picture turned (a turned picture needs no CSS transform, which a scroll bar's arrow
            // in a popup list gets drawn in the wrong place with); the flip bits: 1 = u, 2 = v, 4 = u and v swapped first
            if (opt == "rot=90")
            {
                info.flip |= 4 | 2;
            }
            else if (opt == "rot=180")
            {
                info.flip |= 1 | 2;
            }
            else if (opt == "rot=270")
            {
                info.flip |= 4 | 1;
            }
            if (opt == "flip=h" || opt == "flip=hv" || opt == "flip=vh")
            {
                info.flip |= 1;
            }
            if (opt == "flip=v" || opt == "flip=hv" || opt == "flip=vh")
            {
                info.flip |= 2;
            }
        }
        info.picture = false;
        info.item = false;
        if (s.rfind("item/", 0) == 0)
        {
            // "gear:item/1724" (an item of that type) or "gear:item/id/981" (the live item object): the game's own model of the
            // item, as its inventory windows draw it, in the box of the element (see gear.ui.item_icon).
            std::string number = s.substr(5);
            const bool object = number.rfind("id/", 0) == 0;
            if (object)
            {
                number = number.substr(3);
            }
            if (number.empty() || number.size() > 9 || number.find_first_not_of("0123456789") != std::string::npos)
            {
                return false;
            }
            const uint32_t value = static_cast<uint32_t>(std::stoul(number));
            if (value == 0)
            {
                return false;
            }
            info.item = true;
            info.itemType = object ? 0 : value;
            info.itemObject = object ? value : 0;
            info.id = 0;
            return true;
        }
        if (s.rfind("image/", 0) == 0)
        {
            const std::string number = s.substr(6);
            if (number.empty() || number.size() > 10 || number.find_first_not_of("0123456789") != std::string::npos)
            {
                return false;
            }
            const unsigned long long value = std::stoull(number);
            info.id = value < 0xFFFFFFFFull ? static_cast<uint32_t>(value) : 0;
            return info.id != 0;
        }
        if (s.rfind("texture/", 0) == 0)
        {
            info.id = Ui::textureImage(s.substr(8));
            return info.id != 0;
        }
        if (s.rfind("pad/", 0) == 0)
        {
            // A button prompt of the pad in use ("a", "b", "x", "y", "lb", "rb", "lt", "rt", "back", "start", "l3", "r3", "up",
            // "down", "left", "right"; "confirm" is a, "cancel" b, "menu" start), drawn by SacredBild.
            std::string name = s.substr(4);
            for (char& c : name)
            {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            static const char* const names[] = {"a", "b", "x", "y", "lb", "rb", "lt", "rt", "back", "start", "l3", "r3", "up", "down", "left", "right"};
            if (name == "confirm") name = "a";
            else if (name == "cancel") name = "b";
            else if (name == "menu") name = "start";
            unsigned button = 0;
            for (unsigned i = 0; i < 16; ++i)
            {
                if (name == names[i])
                {
                    button = 1u << i;
                }
            }
            std::vector<unsigned> pixels;
            if (!button || !PadNav::glyph(button, pixels))
            {
                return false;
            }
            info.id = Ui::pictureCreate32(64, 64, std::vector<uint32_t>(pixels.begin(), pixels.end()), true);
            info.picture = true;
            return info.id != 0;
        }
        return false;
    }

    // <gear-ring value="0.4" thickness="3" on="#e0c040" off="#40404080" start="0" dots="0" segments="96"/>: a ring (or a ring of dots) that is
    // lit clockwise from the top (`start` degrees further round) for the fraction `value` of its circle, in the size of its box (CSS width and
    // height, display: inline-block). Data-driven: data-attr-value="expression" redraws it when the number changes. Its geometry goes through
    // the same render interface as every other element (no shader or layer needed).
    class RingElement final : public Rml::Element
    {
    public:
        explicit RingElement(const Rml::String& tag) : Rml::Element(tag) {}

    protected:
        void OnAttributeChange(const Rml::ElementAttributes& changed) override
        {
            Rml::Element::OnAttributeChange(changed);
            m_dirty = true;
        }

        void OnResize() override
        {
            Rml::Element::OnResize();
            m_dirty = true;
        }

        void OnRender() override
        {
            const Rml::Vector2f size = GetBox().GetSize(Rml::BoxArea::Content);
            Rml::RenderManager* manager = GetRenderManager();
            if (size.x <= 0 || size.y <= 0 || !manager)
            {
                return;
            }
            if (m_dirty || !m_geometry)
            {
                m_dirty = false;
                m_geometry = manager->MakeGeometry(build(size));
            }
            m_geometry.Render(GetAbsoluteOffset(Rml::BoxArea::Content));
        }

    private:
        // "#rgb", "#rrggbb" or "#rrggbbaa" (default when the text is none of them).
        static Rml::ColourbPremultiplied color(const Rml::String& text, Rml::Colourb fallback)
        {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            Rml::Colourb out = fallback;
            if (!text.empty() && text[0] == '#')
            {
                const size_t n = text.size() - 1;
                int v[8] = {};
                bool ok = n == 3 || n == 6 || n == 8;
                for (size_t i = 0; ok && i < n; ++i)
                {
                    v[i] = hex(text[i + 1]);
                    ok = v[i] >= 0;
                }
                if (ok && n == 3)
                {
                    out = Rml::Colourb(static_cast<Rml::byte>(v[0] * 17), static_cast<Rml::byte>(v[1] * 17), static_cast<Rml::byte>(v[2] * 17), 255);
                }
                else if (ok)
                {
                    out = Rml::Colourb(static_cast<Rml::byte>(v[0] * 16 + v[1]), static_cast<Rml::byte>(v[2] * 16 + v[3]),
                                       static_cast<Rml::byte>(v[4] * 16 + v[5]), n == 8 ? static_cast<Rml::byte>(v[6] * 16 + v[7]) : Rml::byte(255));
                }
            }
            return out.ToPremultiplied();
        }

        Rml::Mesh build(Rml::Vector2f size)
        {
            const float value = std::clamp(GetAttribute<float>("value", 1.0f), 0.0f, 1.0f);
            const float thickness = std::max(0.5f, GetAttribute<float>("thickness", 3.0f));
            const float start = GetAttribute<float>("start", 0.0f) * 3.14159265f / 180.0f;
            const int dots = std::clamp(GetAttribute<int>("dots", 0), 0, 400);
            const int segments = std::clamp(GetAttribute<int>("segments", 96), 8, 360);
            const Rml::ColourbPremultiplied on = color(GetAttribute<Rml::String>("on", ""), Rml::Colourb(255, 255, 255, 255));
            const Rml::ColourbPremultiplied off = color(GetAttribute<Rml::String>("off", ""), Rml::Colourb(255, 255, 255, 40));
            const Rml::Vector2f center(size.x / 2, size.y / 2);
            const float outer = std::min(size.x, size.y) / 2;
            const float inner = std::max(0.0f, outer - thickness);
            const int count = dots > 0 ? dots : segments;
            const float turn = 6.28318531f;
            Rml::Mesh mesh;
            mesh.vertices.reserve(static_cast<size_t>(count) * 4);
            mesh.indices.reserve(static_cast<size_t>(count) * 6);
            auto point = [&](float angle, float radius) { return Rml::Vector2f(center.x + radius * std::sin(angle), center.y - radius * std::cos(angle)); };
            auto quad = [&](Rml::Vector2f a, Rml::Vector2f b, Rml::Vector2f c, Rml::Vector2f d, Rml::ColourbPremultiplied col) {
                const int base = static_cast<int>(mesh.vertices.size());
                for (const Rml::Vector2f& v : {a, b, c, d})
                {
                    Rml::Vertex vertex;
                    vertex.position = v;
                    vertex.colour = col;
                    vertex.tex_coord = {0, 0};
                    mesh.vertices.push_back(vertex);
                }
                for (int i : {0, 1, 2, 0, 2, 3})
                {
                    mesh.indices.push_back(base + i);
                }
            };
            for (int i = 0; i < count; ++i)
            {
                const bool lit = (static_cast<float>(i) + 0.5f) / static_cast<float>(count) <= value;
                if (dots > 0)
                {
                    // a dot on the ring, as large as the ring is thick
                    const float angle = start + turn * (static_cast<float>(i) + 0.5f) / static_cast<float>(count) - turn / static_cast<float>(count) / 2 + turn * 0.5f / static_cast<float>(count);
                    const Rml::Vector2f c = point(angle, outer - thickness / 2);
                    const float h = thickness / 2;
                    quad({c.x - h, c.y - h}, {c.x + h, c.y - h}, {c.x + h, c.y + h}, {c.x - h, c.y + h}, lit ? on : off);
                }
                else
                {
                    const float a0 = start + turn * static_cast<float>(i) / static_cast<float>(count);
                    const float a1 = start + turn * static_cast<float>(i + 1) / static_cast<float>(count);
                    quad(point(a0, outer), point(a1, outer), point(a1, inner), point(a0, inner), lit ? on : off);
                }
            }
            return mesh;
        }

        Rml::Geometry m_geometry;
        bool m_dirty = true;
    };

    // ---- Shared textures: gear.ui.texture.load ----
    // A mod asks for an image file once and gets a handle; the same file asked for again (by this or another mod) is the same texture with one more
    // reference. A handle is shown in documents as the image source "gear:tex/<id>". The picture is freed when the mods' references are gone and RmlUi
    // has let go of the source too (the host purges it from RmlUi's cache at that moment); the surface itself waits for the frames in flight (ui.cpp).
    struct SharedTexture
    {
        uint32_t picture = 0;
        int width = 0, height = 0;
        int modRefs = 0;        // handles given to Lua
        int rmlRefs = 0;        // RmlUi's loads of "gear:tex/<id>"
        std::string key;
    };
    std::unordered_map<uint32_t, SharedTexture> g_shared;
    std::unordered_map<std::string, uint32_t> g_sharedByKey;
    uint32_t g_nextShared = 0;

    void destroyIfUnused(uint32_t id)
    {
        const auto it = g_shared.find(id);
        if (it == g_shared.end() || it->second.modRefs > 0 || it->second.rmlRefs > 0)
        {
            return;
        }
        LOG("RmlUi: shared texture {} freed ({} still loaded)", id, g_shared.size() - 1);
        Ui::pictureRelease(it->second.picture);
        g_sharedByKey.erase(it->second.key);
        g_shared.erase(it);
    }

    // The picture of an image file's bytes (PNG, TGA, JPG, DDS) and its size; 0 with `error`.
    uint32_t pictureFromImageBytes(const std::string& bytes, int& w, int& h, std::string& error)
    {
        if (Gear::Dds::isDds(bytes.data(), bytes.size()))
        {
            Gear::Dds::Image dds;
            if (!Gear::Dds::load(bytes.data(), bytes.size(), true, dds, error))
            {
                return 0;
            }
            w = dds.width;
            h = dds.height;
            uint32_t id = 0;
            if (dds.kind == Gear::Dds::Kind::Block)
            {
                id = Ui::pictureCreateBlocks(dds.width, dds.height, dds.fourcc, std::move(dds.blocks));
            }
            else
            {
                for (uint32_t& p : dds.argb)
                {
                    // RmlUi blends premultiplied colors.
                    const uint32_t a = p >> 24, r = (p >> 16 & 255) * a / 255, g = (p >> 8 & 255) * a / 255, b = (p & 255) * a / 255;
                    p = a << 24 | r << 16 | g << 8 | b;
                }
                id = Ui::pictureCreate32(dds.width, dds.height, std::move(dds.argb), true);
            }
            if (!id)
            {
                error = "the picture is too large";
            }
            return id;
        }
        int channels = 0;
        stbi_uc* data = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()), &w, &h, &channels, 4);
        if (!data)
        {
            error = "cannot decode the image";
            return 0;
        }
        std::vector<uint32_t> pixels(size_t(w) * h);
        for (size_t i = 0; i < pixels.size(); ++i)
        {
            const uint32_t a = data[i * 4 + 3];
            // RmlUi blends premultiplied colors.
            const uint32_t r = data[i * 4] * a / 255, g = data[i * 4 + 1] * a / 255, b = data[i * 4 + 2] * a / 255;
            pixels[i] = a << 24 | r << 16 | g << 8 | b;
        }
        stbi_image_free(data);
        const uint32_t id = Ui::pictureCreate32(w, h, std::move(pixels), true);
        if (!id)
        {
            error = "the picture is too large";
        }
        return id;
    }

    class RenderInterface final : public Rml::RenderInterface
    {
    public:
        Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override
        {
            std::vector<Ui::MeshVertex> vs;
            vs.reserve(vertices.size());
            for (const Rml::Vertex& v : vertices)
            {
                const uint32_t color = uint32_t(v.colour.alpha) << 24 | uint32_t(v.colour.red) << 16 |
                                       uint32_t(v.colour.green) << 8 | uint32_t(v.colour.blue);
                vs.push_back({v.position.x, v.position.y, color, v.tex_coord.x, v.tex_coord.y});
            }
            std::vector<int> is(indices.data(), indices.data() + indices.size());
            // The box of the geometry (an element's background or image quad): where an item's model goes.
            std::array<float, 4> box = {0, 0, 0, 0};
            if (!vertices.empty())
            {
                box = {vertices[0].position.x, vertices[0].position.y, vertices[0].position.x, vertices[0].position.y};
                for (const Rml::Vertex& v : vertices)
                {
                    box[0] = std::min(box[0], v.position.x);
                    box[1] = std::min(box[1], v.position.y);
                    box[2] = std::max(box[2], v.position.x);
                    box[3] = std::max(box[3], v.position.y);
                }
            }
            const uint32_t mesh = Ui::meshCreate(std::move(vs), std::move(is));
            if (mesh)
            {
                m_bounds[mesh] = box;
            }
            return mesh;
        }

        void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override
        {
            if (g_target.count >= kMaxMeshesPerDraw)
            {
                return;
            }
            if (texture)
            {
                const auto item = m_textures.find(texture);
                if (item != m_textures.end() && item->second.item)
                {
                    renderItem(geometry, translation, item->second);
                    return;
                }
            }
            Ui::Command c;
            c.kind = Ui::Kind::Mesh;
            c.layer = static_cast<Ui::Layer>(g_target.layer);
            c.window = g_target.window;
            c.region = static_cast<uint32_t>(geometry);
            c.x = translation.x + g_target.origin.x;
            c.y = translation.y + g_target.origin.y;
            c.w = g_target.unit;
            c.matrix = g_target.matrix;
            if (texture)
            {
                const auto it = m_textures.find(texture);
                if (it == m_textures.end())
                {
                    return;
                }
                if (!it->second.source.empty())
                {
                    it->second.lastDraw = GetTickCount64();
                }
                c.image = it->second.id;
                c.flip = it->second.flip;
                c.point = it->second.point;
            }
            if (g_target.scissor)
            {
                c.clipped = true;
                std::copy(std::begin(g_target.clip), std::end(g_target.clip), std::begin(c.clip));
            }
            if (Ui::add(std::move(c)))
            {
                ++g_target.count;
            }
        }

        void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override
        {
            m_bounds.erase(static_cast<uint32_t>(geometry));
            Ui::meshRelease(static_cast<uint32_t>(geometry));
        }

        // A picture whose size is in layout units (source "?dp") has its natural size in screen pixels: the size times the canvas scale.
        static void scaleToLayoutUnits(Rml::Vector2i& dimensions)
        {
            const float scale = std::max(0.5f, Ui::canvasScale());
            dimensions.x = std::max(1, static_cast<int>(std::lround(dimensions.x * scale)));
            dimensions.y = std::max(1, static_cast<int>(std::lround(dimensions.y * scale)));
        }

        Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override
        {
            TextureInfo info;
            if (source.rfind("gear:tex/", 0) == 0)
            {
                const auto it = g_shared.find(static_cast<uint32_t>(std::strtoul(source.c_str() + 9, nullptr, 10)));
                if (it == g_shared.end())
                {
                    LOG("RmlUi: no shared texture {}", source);
                    return 0;
                }
                ++it->second.rmlRefs;
                info.id = it->second.picture;
                info.picture = true;
                info.shared = it->first;
                dimensions = {it->second.width, it->second.height};
                return add(info);
            }
            if (source.rfind("gear:", 0) == 0)
            {
                Ui::ImageInfo image;
                if (parseGearTexture(source, info) && info.item)
                {
                    dimensions = {64, 64};      // an item's model: any size, drawn contained in the element's box
                    return add(info);
                }
                if (info.item)
                {
                    LOG("RmlUi: bad item source {}", source);
                    return 0;
                }
                if (info.picture)
                {
                    dimensions = {64, 64};      // a pad button prompt
                    return add(info);
                }
                if (!info.picture && info.id >= Ui::kPictureBase)
                {
                    // gear:image/<picture id>: a picture of Gear's own that the mod made (gear.game.save_picture), drawn as it is and
                    // not released with the document (the mod keeps it).
                    dimensions = {128, 96};
                    return add(info);
                }
                if (!info.picture && (info.id == 0 || !Ui::imageInfo(info.id, image) || image.width <= 0 || image.height <= 0))
                {
                    LOG("RmlUi: no game image for {}", source);
                    return 0;
                }
                dimensions = (info.flip & 4) ? Rml::Vector2i{image.height, image.width} : Rml::Vector2i{image.width, image.height};     // turned by 90 degrees: the sides swap
                if (info.dp)
                {
                    scaleToLayoutUnits(dimensions);
                }
                return add(info);
            }
            std::string path = plainPath(source);
            info.source = source;
            info.lastDraw = GetTickCount64();
            bool dpSize = false;
            if (path.size() > 3 && path.compare(path.size() - 3, 3, "?dp") == 0)
            {
                path.resize(path.size() - 3);       // "file.png?dp": the size of the picture is in layout units (documents in screen pixels)
                dpSize = true;
            }
            std::string bytes;
            bool sharedImage = false;
            fs::path imageFile = resolveShared(path, &sharedImage);
            if (!sharedImage)
            {
                imageFile = fromUtf8(path);
            }
            if (imageFile.empty() || !readAll(imageFile, bytes, sharedImage))
            {
                LOG("RmlUi: cannot read the image {}", source);
                return 0;
            }
            int w = 0, h = 0;
            std::string error;
            info.id = pictureFromImageBytes(bytes, w, h, error);
            if (!info.id)
            {
                LOG("RmlUi: cannot load the image {}: {}", source, error);
                return 0;
            }
            info.picture = true;
            dimensions = {w, h};
            if (dpSize)
            {
                scaleToLayoutUnits(dimensions);
            }
            return add(info);
        }

        Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) override
        {
            if (dimensions.x <= 0 || dimensions.y <= 0 || source.size() < size_t(dimensions.x) * dimensions.y * 4)
            {
                return 0;
            }
            std::vector<uint32_t> pixels(size_t(dimensions.x) * dimensions.y);
            const Rml::byte* s = source.data();
            for (size_t i = 0; i < pixels.size(); ++i)
            {
                // RGBA bytes, alpha already multiplied in.
                pixels[i] = uint32_t(s[i * 4 + 3]) << 24 | uint32_t(s[i * 4]) << 16 | uint32_t(s[i * 4 + 1]) << 8 | s[i * 4 + 2];
            }
            TextureInfo info;
            info.id = Ui::pictureCreate32(dimensions.x, dimensions.y, std::move(pixels), true);
            if (!info.id)
            {
                return 0;
            }
            info.picture = true;
            return add(info);
        }

        void ReleaseTexture(Rml::TextureHandle texture) override
        {
            const auto it = m_textures.find(texture);
            if (it == m_textures.end())
            {
                return;
            }
            const uint32_t shared = it->second.shared;
            if (it->second.picture && !shared)
            {
                Ui::pictureRelease(it->second.id);
            }
            m_textures.erase(it);
            if (shared)
            {
                const auto entry = g_shared.find(shared);
                if (entry != g_shared.end())
                {
                    --entry->second.rmlRefs;
                }
                destroyIfUnused(shared);
            }
        }

        // CSS transforms: the matrix goes along with every mesh until it is reset (null).
        void SetTransform(const Rml::Matrix4f* transform) override
        {
            if (!transform)
            {
                g_target.matrix.reset();
                return;
            }
            auto m = std::make_shared<std::array<float, 16>>();
            for (int column = 0; column < 4; ++column)
            {
                for (int row = 0; row < 4; ++row)
                {
                    (*m)[static_cast<size_t>(column * 4 + row)] = (*transform)[column][row];
                }
            }
            // RmlUi's matrix is in the context's coordinates; Gear adds the document's origin (a document that starts at the screen's edge, see
            // Doc::originPx) to the vertices before the matrix, so the matrix is moved there: M' = T(origin) * M * T(-origin). (Without this
            // every transformed part of such a document was placed off by its origin: the scroll bar's turned arrows, a button's hover scale.)
            const float ox = g_target.origin.x, oy = g_target.origin.y;
            if (ox != 0.0f || oy != 0.0f)
            {
                auto& r = *m;
                for (int row = 0; row < 4; ++row)
                {
                    r[static_cast<size_t>(3 * 4 + row)] -= ox * r[static_cast<size_t>(0 * 4 + row)] + oy * r[static_cast<size_t>(1 * 4 + row)];
                }
                for (int column = 0; column < 4; ++column)
                {
                    r[static_cast<size_t>(column * 4 + 0)] += ox * r[static_cast<size_t>(column * 4 + 3)];
                    r[static_cast<size_t>(column * 4 + 1)] += oy * r[static_cast<size_t>(column * 4 + 3)];
                }
            }
            g_target.matrix = std::move(m);
        }

        void EnableScissorRegion(bool enable) override { g_target.scissor = enable; }

        void SetScissorRegion(Rml::Rectanglei region) override
        {
            const float unit = g_target.unit > 0 ? g_target.unit : 1.0f;
            g_target.clip[0] = (static_cast<float>(region.Left()) + g_target.origin.x) / unit;
            g_target.clip[1] = (static_cast<float>(region.Top()) + g_target.origin.y) / unit;
            g_target.clip[2] = static_cast<float>(region.Width()) / unit;
            g_target.clip[3] = static_cast<float>(region.Height()) / unit;
        }

    private:
        // An item's model ("gear:item/..."): drawn by the game's model renderer into the box of the geometry (translated), as large as
        // fits; CSS transforms and tints do not apply to it. The clip of the element cuts it as for any item icon.
        void renderItem(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, const TextureInfo& info)
        {
            const auto b = m_bounds.find(static_cast<uint32_t>(geometry));
            if (b == m_bounds.end())
            {
                return;
            }
            const float unit = g_target.unit > 0 ? g_target.unit : 1.0f;
            Ui::Command c;
            c.kind = Ui::Kind::ItemIcon;
            c.layer = static_cast<Ui::Layer>(g_target.layer);
            c.window = g_target.window;
            c.x = (translation.x + b->second[0] + g_target.origin.x) / unit;
            c.y = (translation.y + b->second[1] + g_target.origin.y) / unit;
            c.w = (b->second[2] - b->second[0]) / unit;
            c.h = (b->second[3] - b->second[1]) / unit;
            c.region = info.itemObject;
            c.image = info.itemType;
            if (c.w <= 0 || c.h <= 0)
            {
                return;
            }
            if (g_target.scissor)
            {
                c.clipped = true;
                std::copy(std::begin(g_target.clip), std::end(g_target.clip), std::begin(c.clip));
            }
            if (Ui::add(std::move(c)))
            {
                ++g_target.count;
            }
        }

        Rml::TextureHandle add(const TextureInfo& info)
        {
            const Rml::TextureHandle handle = ++m_next;
            m_textures.emplace(handle, info);
            return handle;
        }
    public:
        // RmlUi keeps every texture it ever loaded until it is told to let go. A file image that no frame has drawn for a while (the minimap's
        // map tiles the hero walked away from) is released: its picture and GPU texture go, RmlUi loads it again if it is needed later.
        void sweepFileTextures()
        {
            constexpr uint64_t kIdleMs = 8000;
            const uint64_t now = GetTickCount64();
            if (now - m_lastSweep < 2000)
            {
                return;
            }
            m_lastSweep = now;
            std::vector<std::string> idle;
            for (const auto& [handle, info] : m_textures)
            {
                if (!info.source.empty() && now - info.lastDraw > kIdleMs)
                {
                    idle.push_back(info.source);
                }
            }
            for (const std::string& source : idle)
            {
                Rml::ReleaseTexture(source);
            }
        }

    private:
        uint64_t m_lastSweep = 0;

        std::unordered_map<Rml::TextureHandle, TextureInfo> m_textures;
        std::unordered_map<uint32_t, std::array<float, 4>> m_bounds;      // mesh id -> left, top, right, bottom in RmlUi pixels
        Rml::TextureHandle m_next = 0;
    };

    // Never destroyed: RmlUi's own statics outlive ours when the DLL unloads with the process.
    SystemInterface* g_system = nullptr;
    FileInterface* g_file = nullptr;
    GearCss::CssContentSupport* g_content = nullptr;    // the CSS `content` property (generated text, icon fonts)
    RenderInterface* g_render = nullptr;
    bool g_started = false;
    bool g_startFailed = false;

    // ---- Documents ----

    struct Doc;

    struct QueuedEvent
    {
        int listener = 0;
        std::string type, target, current;
        std::vector<std::pair<std::string, std::string>> params;
        std::string call;                       // a data-event-* callback: its name (listener is 0)
        std::vector<std::string> args;          // ... and its arguments
    };

    // The data model: a tree of values (what the mod's Lua table holds) that RmlUi's data bindings read and write.
    struct Node
    {
        enum class Type { Nil, Bool, Num, Str, Arr, Obj } type = Type::Nil;
        bool b = false;
        double n = 0;
        std::string s;
        std::vector<Node> arr;
        std::vector<std::pair<std::string, Node>> obj;
    };

    std::atomic<unsigned> g_modelEdits{0};      // counts the values the document itself changed (data-value)

    class NodeDef final : public Rml::VariableDefinition
    {
    public:
        explicit NodeDef(Rml::DataVariableType type) : Rml::VariableDefinition(type) {}

        bool Get(void* ptr, Rml::Variant& variant) override
        {
            const Node& n = *static_cast<Node*>(ptr);
            switch (n.type)
            {
            case Node::Type::Bool: variant = n.b; return true;
            case Node::Type::Num: variant = n.n; return true;
            case Node::Type::Str: variant = n.s; return true;
            case Node::Type::Nil: variant = Rml::String(); return true;
            default: return false;
            }
        }

        bool Set(void* ptr, const Rml::Variant& variant) override
        {
            Node& n = *static_cast<Node*>(ptr);
            bool ok = false;
            if (n.type == Node::Type::Bool)
            {
                ok = variant.GetInto(n.b);
            }
            else if (n.type == Node::Type::Num)
            {
                ok = variant.GetInto(n.n);
            }
            else if (n.type == Node::Type::Str || n.type == Node::Type::Nil)
            {
                ok = variant.GetInto(n.s);
                if (ok)
                {
                    n.type = Node::Type::Str;
                }
            }
            if (ok)
            {
                ++g_modelEdits;
            }
            return ok;
        }

        int Size(void* ptr) override
        {
            const Node& n = *static_cast<Node*>(ptr);
            return n.type == Node::Type::Arr ? static_cast<int>(n.arr.size()) : static_cast<int>(n.obj.size());
        }

        Rml::DataVariable Child(void* ptr, const Rml::DataAddressEntry& address) override;
    };

    NodeDef g_scalarDef(Rml::DataVariableType::Scalar), g_arrayDef(Rml::DataVariableType::Array), g_structDef(Rml::DataVariableType::Struct);

    Rml::DataVariable variableOf(Node& n)
    {
        return Rml::DataVariable(n.type == Node::Type::Arr ? static_cast<Rml::VariableDefinition*>(&g_arrayDef)
                                 : n.type == Node::Type::Obj ? static_cast<Rml::VariableDefinition*>(&g_structDef)
                                                             : static_cast<Rml::VariableDefinition*>(&g_scalarDef),
                                 &n);
    }

    Rml::DataVariable NodeDef::Child(void* ptr, const Rml::DataAddressEntry& address)
    {
        Node& n = *static_cast<Node*>(ptr);
        if (n.type == Node::Type::Arr)
        {
            if (address.index >= 0 && address.index < static_cast<int>(n.arr.size()))
            {
                return variableOf(n.arr[static_cast<size_t>(address.index)]);
            }
            if (address.name == "size")
            {
                return Rml::MakeLiteralIntVariable(static_cast<int>(n.arr.size()));
            }
        }
        else if (n.type == Node::Type::Obj)
        {
            for (auto& [key, value] : n.obj)
            {
                if (key == address.name)
                {
                    return variableOf(value);
                }
            }
        }
        return Rml::DataVariable();
    }

    // Lua value at `index` -> Node (depth-limited; functions and userdata become nil; a table with only 1..n keys is an array).
    void toNode(lua_State* L, int index, Node& out, int depth)
    {
        if (index < 0 && index > LUA_REGISTRYINDEX)
        {
            index = lua_gettop(L) + index + 1;
        }
        out = Node();
        switch (lua_type(L, index))
        {
        case LUA_TBOOLEAN: out.type = Node::Type::Bool; out.b = lua_toboolean(L, index) != 0; break;
        case LUA_TNUMBER: out.type = Node::Type::Num; out.n = lua_tonumber(L, index); break;
        case LUA_TSTRING: out.type = Node::Type::Str; out.s = lua_tostring(L, index); break;
        case LUA_TTABLE:
        {
            if (depth > 8)
            {
                break;
            }
            const size_t count = lua_objlen(L, index);
            size_t keys = 0;
            lua_pushnil(L);
            while (lua_next(L, index))
            {
                ++keys;
                lua_pop(L, 1);
            }
            if (keys == count)
            {
                out.type = Node::Type::Arr;
                out.arr.resize(count);
                for (size_t i = 0; i < count; ++i)
                {
                    lua_rawgeti(L, index, static_cast<int>(i + 1));
                    toNode(L, -1, out.arr[i], depth + 1);
                    lua_pop(L, 1);
                }
            }
            else
            {
                out.type = Node::Type::Obj;
                lua_pushnil(L);
                while (lua_next(L, index))
                {
                    if (lua_type(L, -2) == LUA_TSTRING)
                    {
                        out.obj.emplace_back(lua_tostring(L, -2), Node());
                        toNode(L, -1, out.obj.back().second, depth + 1);
                    }
                    lua_pop(L, 1);
                }
            }
            break;
        }
        default: break;
        }
    }

    void pushNode(lua_State* L, const Node& n)
    {
        switch (n.type)
        {
        case Node::Type::Bool: lua_pushboolean(L, n.b); break;
        case Node::Type::Num: lua_pushnumber(L, n.n); break;
        case Node::Type::Str: lua_pushlstring(L, n.s.data(), n.s.size()); break;
        case Node::Type::Arr:
            lua_createtable(L, static_cast<int>(n.arr.size()), 0);
            for (size_t i = 0; i < n.arr.size(); ++i)
            {
                pushNode(L, n.arr[i]);
                lua_rawseti(L, -2, static_cast<int>(i + 1));
            }
            break;
        case Node::Type::Obj:
            lua_createtable(L, 0, static_cast<int>(n.obj.size()));
            for (const auto& [key, value] : n.obj)
            {
                pushNode(L, value);
                lua_setfield(L, -2, key.c_str());
            }
            break;
        default: lua_pushnil(L); break;
        }
    }

    // Whether `e` matches a selector: simple ones ("#id", ".class", "tag", [data-gh="N"]) by plain comparison, others through RmlUi
    // (which parses the selector on every call).
    bool matchesSelector(Rml::Element* e, const std::string& selector)
    {
        const auto plain = [](const std::string& name) {
            return !name.empty() && name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == std::string::npos;
        };
        if (selector.size() > 1 && selector[0] == '#' && plain(selector.substr(1)))
        {
            return e->GetId() == selector.substr(1);
        }
        if (selector.size() > 1 && selector[0] == '.' && plain(selector.substr(1)))
        {
            return e->IsClassSet(selector.substr(1));
        }
        if (plain(selector))
        {
            return e->GetTagName() == selector;
        }
        if (selector.rfind("[data-gh=\"", 0) == 0 && selector.size() > 12 && selector.compare(selector.size() - 2, 2, "\"]") == 0)
        {
            const std::string number = selector.substr(10, selector.size() - 12);
            return !number.empty() && number.find_first_not_of("0123456789") == std::string::npos &&
                   e->GetAttribute<int>("data-gh", 0) == std::atoi(number.c_str());
        }
        return e->Matches(selector);
    }

    class Listener final : public Rml::EventListener
    {
    public:
        // `selector` empty: the listener sits on the element it was added to. Otherwise it sits on the document, in the capture
        // phase, and fires for events whose target is, or lies within, an element matching the selector: elements added later count.
        Listener(Doc* doc, int id, std::string selector = {}) : m_doc(doc), m_id(id), m_selector(std::move(selector)) {}
        void ProcessEvent(Rml::Event& event) override;
        const std::string& selector() const { return m_selector; }

    private:
        Doc* m_doc;
        int m_id;
        std::string m_selector;
    };

    struct Doc
    {
        int id = 0;
        std::string mod;
        fs::path root;
        fs::path themeRoot;                     // the theme mod's folder, if the document uses the theme
        Rml::Context* context = nullptr;
        Rml::ElementDocument* document = nullptr;
        std::string contextName;
        bool dp = false;
        bool screenOrigin = false;              // the document's (0, 0) is the screen's top left corner, not the layout's
        Rml::Vector2f originPx{0.0f, 0.0f};     // that corner in layout pixels (units * unit): layout position = context position + originPx
        // Where the <body>'s top left corner lies in layout pixels: the screen's corner (originPx) for a document that starts there, the layout's own
        // (0, 0) for the usual one, whose body is moved right and down by the screen's margin so that what lies left of or above the layout still
        // is inside the context (RmlUi draws no text outside it).
        Rml::Vector2f bodyPx{0.0f, 0.0f};
        Rml::ElementDocument* shiftedDocument = nullptr;    // the document that has the margin below ...
        Rml::Vector2f shiftApplied{0.0f, 0.0f};             // ... and its size
        float unit = 1.0f;
        bool hovering = false;
        std::vector<std::unique_ptr<Listener>> listeners;
        std::vector<QueuedEvent> events;
        Node model;                             // the data model's values (an object), if the document has one
        Rml::DataModelHandle modelHandle;
        bool hasModel = false;
        unsigned modelSeen = 0;
        int nextHandle = 0;                     // element handles (data-gh), see rml_query
        // What a reload needs (hot reload): how the document was loaded and the files it read.
        fs::path file;
        std::string source;                     // the markup of a string document
        bool fromString = false;
        fs::path themeFile;
        bool base = false;                      // the plain base style (no theme)
        std::map<fs::path, fs::file_time_type> deps;
        double hotChecked = 0.0;
        std::string hotError;                   // the last reload that failed (kept until a reload works)
        unsigned reloads = 0;
        std::vector<std::string> listenerEvents;    // the event name of each listener (the same index), to attach them to a reloaded document
    };

    std::map<int, std::unique_ptr<Doc>> g_docs;
    int g_nextDoc = 1;

    void Listener::ProcessEvent(Rml::Event& event)
    {
        if (m_doc->events.size() >= 512)
        {
            return;     // nobody reads them: drop the rest
        }
        QueuedEvent q;
        q.listener = m_id;
        q.type = event.GetType();
        Rml::Element* matched = nullptr;
        if (!m_selector.empty())
        {
            // The innermost element from the target up that matches the selector.
            for (Rml::Element* e = event.GetTargetElement(); e && e != m_doc->document; e = e->GetParentNode())
            {
                if (matchesSelector(e, m_selector))
                {
                    matched = e;
                    break;
                }
            }
            if (!matched)
            {
                return;
            }
        }
        if (Rml::Element* t = event.GetTargetElement())
        {
            q.target = t->GetId();
        }
        if (Rml::Element* c = matched ? matched : event.GetCurrentElement())
        {
            q.current = c->GetId();
        }
        for (const auto& [key, value] : event.GetParameters())
        {
            Rml::String text;
            if (value.GetInto(text))
            {
                q.params.emplace_back(key, std::move(text));
            }
        }
        m_doc->events.push_back(std::move(q));
    }

    // The current call's rights: its folder, released at the end of the call.
    class RootScope
    {
    public:
        explicit RootScope(const Doc& doc) : m_previous(std::move(g_root)), m_previousTheme(std::move(g_themeRoot)), m_previousDp(g_fontDp)
        {
            g_root = doc.root;
            g_themeRoot = doc.themeRoot;
            g_fontDp = doc.dp;
        }
        explicit RootScope(fs::path root, fs::path themeRoot = {}) : m_previous(std::move(g_root)), m_previousTheme(std::move(g_themeRoot)), m_previousDp(g_fontDp)
        {
            g_root = std::move(root);
            g_themeRoot = std::move(themeRoot);
        }
        ~RootScope()
        {
            g_root = std::move(m_previous);
            g_themeRoot = std::move(m_previousTheme);
            g_fontDp = m_previousDp;
        }
        RootScope(const RootScope&) = delete;
        RootScope& operator=(const RootScope&) = delete;

    private:
        fs::path m_previous, m_previousTheme;
        bool m_previousDp;
    };

    void loadFont(const fs::path& path, bool fallback)
    {
        std::error_code ec;
        if (fs::exists(path, ec))
        {
            if (!Rml::LoadFontFace(toUtf8(path), fallback))
            {
                LOG("RmlUi: cannot load the font {}", toUtf8(path));
            }
        }
    }

    // The families the host registers for the theme: "Segoe UI" (the modern default; Arial under that name if Segoe is missing),
    // "Sacred Game" (the game's own ANTQS__.TTF from the font folder next to Sacred.exe; the default face if it is missing) and
    // "Sacred Start" (CAROLING.TTF, the start screen's; else the game font). A stylesheet can always say font-family: Sacred Game.
    struct FontReport
    {
        std::string family;
        bool real = false;          // the intended file was found and loaded (not the fallback)
        std::string file;           // the file the family's regular face comes from
    };
    FontReport g_fontDefault{"Segoe UI"}, g_fontGame{"Sacred Game"}, g_fontStart{"Sacred Start"};
    std::vector<std::unique_ptr<std::string>> g_fontData;       // RmlUi needs the data until Shutdown

    std::string slurpFont(const fs::path& path)
    {
        std::error_code ec;
        if (!fs::is_regular_file(path, ec))
        {
            return {};
        }
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            return {};
        }
        std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return bytes.size() > 64 && bytes.size() < 32u * 1024 * 1024 ? bytes : std::string();
    }

    // A resource of SacredGear.dll (gear.rc), as bytes.
    std::string embeddedResource(int id)
    {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&embeddedResource), &module))
        {
            return {};
        }
        HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
        HGLOBAL data = res ? LoadResource(module, res) : nullptr;
        const char* bytes = data ? static_cast<const char*>(LockResource(data)) : nullptr;
        return bytes ? std::string(bytes, SizeofResource(module, res)) : std::string();
    }

    bool addFace(const std::string& family, const std::string& bytes, Rml::Style::FontStyle style, Rml::Style::FontWeight weight)
    {
        if (bytes.empty())
        {
            return false;
        }
        g_fontData.push_back(std::make_unique<std::string>(bytes));
        const std::string& kept = *g_fontData.back();
        return Rml::LoadFontFace(Rml::Span<const Rml::byte>(reinterpret_cast<const Rml::byte*>(kept.data()), kept.size()), family, style, weight, false);
    }

    fs::path gameFontDir()
    {
        wchar_t exe[MAX_PATH] = {};
        const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (n == 0 || n >= MAX_PATH)
        {
            return {};
        }
        return fs::path(exe).parent_path() / L"font";
    }

    void registerThemeFonts(const fs::path& system)
    {
        using Rml::Style::FontStyle;
        using Rml::Style::FontWeight;
        // Default: Segoe UI as installed; Arial registered under the same name if it is not.
        const std::string segoe = slurpFont(system / "segoeui.ttf");
        std::string regular, bold, italic;
        if (!segoe.empty())
        {
            regular = segoe;
            g_fontDefault.real = true;
            g_fontDefault.file = toUtf8(system / "segoeui.ttf");
        }
        else
        {
            regular = slurpFont(system / "arial.ttf");
            bold = slurpFont(system / "arialbd.ttf");
            italic = slurpFont(system / "ariali.ttf");
            g_fontDefault.real = false;
            g_fontDefault.file = toUtf8(system / "arial.ttf");
            if (addFace("Segoe UI", regular, FontStyle::Normal, FontWeight::Normal))
            {
                addFace("Segoe UI", bold, FontStyle::Normal, FontWeight::Bold);
                addFace("Segoe UI", italic, FontStyle::Italic, FontWeight::Normal);
            }
            else
            {
                LOG("RmlUi: neither Segoe UI nor Arial found in {}", toUtf8(system));
            }
        }
        // The icon font of Sacred Gear (a subset of Material Symbols): class "icon icon-refresh" ... in every document, see kUserAgentStyle.
        if (!addFace("Gear Icons", embeddedResource(IDR_GEAR_ICONS), FontStyle::Normal, FontWeight::Normal))
        {
            LOG("RmlUi: the icon font did not load");
        }
        const std::string defaultBold = !bold.empty() ? bold : slurpFont(system / "segoeuib.ttf");
        // Game font and start screen font from the folder next to Sacred.exe.
        const fs::path dir = gameFontDir();
        const std::string game = dir.empty() ? std::string() : slurpFont(dir / "ANTQS__.TTF");
        const std::string start = dir.empty() ? std::string() : slurpFont(dir / "CAROLING.TTF");
        if (!game.empty() && addFace("Sacred Game", game, FontStyle::Normal, FontWeight::Normal))
        {
            g_fontGame.real = true;
            g_fontGame.file = toUtf8(dir / "ANTQS__.TTF");
        }
        else
        {
            g_fontGame.real = false;
            g_fontGame.file = g_fontDefault.file;
            addFace("Sacred Game", regular, FontStyle::Normal, FontWeight::Normal);
            addFace("Sacred Game", defaultBold, FontStyle::Normal, FontWeight::Bold);
        }
        if (!start.empty() && addFace("Sacred Start", start, FontStyle::Normal, FontWeight::Normal))
        {
            g_fontStart.real = true;
            g_fontStart.file = toUtf8(dir / "CAROLING.TTF");
        }
        else
        {
            g_fontStart.real = false;
            g_fontStart.file = g_fontGame.file;
            addFace("Sacred Start", g_fontGame.real ? game : regular, FontStyle::Normal, FontWeight::Normal);
        }
        LOG("RmlUi fonts: default '{}' ({}), game '{}' ({}), start '{}' ({})", g_fontDefault.family, g_fontDefault.real ? "Segoe UI" : "Arial",
            g_fontGame.family, g_fontGame.real ? g_fontGame.file : "fallback", g_fontStart.family, g_fontStart.real ? g_fontStart.file : "fallback");
    }

    // Crisp text: a document laid out in layout units (1px = one layout unit) is stretched to the screen by the renderer, which would
    // stretch the glyphs of a face rasterized at the layout size too (blurry on every screen larger than 1024x768). This wraps the font
    // engine: a face of N px is rasterized at N * (the screen's pixels per layout unit) and every measure and vertex it gives back is
    // divided by that again, so the layout stays in layout units and the glyphs come out at the size they are shown. A document laid
    // out in screen pixels (dp) already asks for the final size: it keeps its faces as they are (g_fontDp).
    class ScaledFontEngine : public Rml::FontEngineInterface
    {
    public:
        explicit ScaledFontEngine(Rml::FontEngineInterface* inner) : m_inner(inner) {}

        bool LoadFontFace(const Rml::String& file, int index, bool fallback, Rml::Style::FontWeight weight) override
        {
            return m_inner->LoadFontFace(file, index, fallback, weight);
        }
        bool LoadFontFace(const Rml::String& file, int index, const Rml::String& family, Rml::Style::FontStyle style, Rml::Style::FontWeight weight,
            bool fallback) override
        {
            return m_inner->LoadFontFace(file, index, family, style, weight, fallback);
        }
        bool LoadFontFace(Rml::Span<const Rml::byte> data, int index, const Rml::String& family, Rml::Style::FontStyle style,
            Rml::Style::FontWeight weight, bool fallback) override
        {
            return m_inner->LoadFontFace(data, index, family, style, weight, fallback);
        }

        Rml::FontFaceHandle GetFontFaceHandle(const Rml::String& family, Rml::Style::FontStyle style, Rml::Style::FontWeight weight, int size) override
        {
            const float scale = currentScale();
            const int milli = static_cast<int>(std::lround(scale * 1000.0f));
            std::string key = family;
            key += '|';
            key += std::to_string(static_cast<int>(style)) + '|' + std::to_string(static_cast<int>(weight)) + '|' + std::to_string(size) + '|' +
                   std::to_string(milli) + (g_fontDp ? "|dp" : "");
            const auto found = m_byKey.find(key);
            if (found != m_byKey.end())
            {
                return found->second;
            }
            // The patched RmlUi of ports/rmlui (font-size-sixteenths.patch, freetype-no-hinting.patch) gives face sizes in 1/16 pixel and rasterizes at what it is
            // given: the face is rasterized at exactly size * scale, not at a rounded pixel size.
            const int raster = std::max(1, static_cast<int>(std::lround(size * scale)));
            // Documents laid out in screen pixels (g_fontDp) have their glyphs and baselines on whole pixels: light hinting (vertical grid fitting only)
            // sharpens them there (bit 28 of the size, see ports/rmlui/freetype-no-hinting.patch); SACRED_FONT_HINT=0 turns it off. The layout-unit documents
            // are scaled by the renderer, so their grid is not the screen's: no hinting.
            static const bool lightHinting = [] { const char* v = std::getenv("SACRED_FONT_HINT"); return !(v && v[0] == '0'); }();
            const int sized = (g_fontDp && lightHinting) ? (raster | (1 << 28)) : raster;
            const Rml::FontFaceHandle inner = m_inner->GetFontFaceHandle(family, style, weight, sized);
            if (!inner)
            {
                return 0;
            }
            Face face;
            face.inner = inner;
            face.scale = scale;
            face.metrics = m_inner->GetFontMetrics(inner);
            face.metrics.size = std::max(1, static_cast<int>(std::lround(size / 16.0f)));
            face.metrics.ascent /= scale;
            face.metrics.descent /= scale;
            face.metrics.line_spacing /= scale;
            face.metrics.x_height /= scale;
            face.metrics.underline_position /= scale;
            face.metrics.underline_thickness /= scale;
            m_faces.push_back(face);
            const Rml::FontFaceHandle handle = static_cast<Rml::FontFaceHandle>(m_faces.size());
            m_byKey.emplace(key, handle);
            return handle;
        }

        Rml::FontEffectsHandle PrepareFontEffects(Rml::FontFaceHandle handle, const Rml::FontEffectList& effects) override
        {
            const Face* face = faceOf(handle);
            if (!face)
            {
                return 0;
            }
            Effects fx;
            fx.inner = m_inner->PrepareFontEffects(face->inner, effects);
            m_effects.push_back(fx);
            return static_cast<Rml::FontEffectsHandle>(m_effects.size());
        }

        const Rml::FontMetrics& GetFontMetrics(Rml::FontFaceHandle handle) override
        {
            const Face* face = faceOf(handle);
            return face ? face->metrics : m_none;
        }

        int GetStringWidth(Rml::FontFaceHandle handle, Rml::StringView string, const Rml::TextShapingContext& context, Rml::Character prior) override
        {
            const Face* face = faceOf(handle);
            if (!face)
            {
                return 0;
            }
            const Rml::TextShapingContext scaled = scaledContext(context, face->scale);
            const int width = m_inner->GetStringWidth(face->inner, string, scaled, prior);
            return face->scale == 1.0f ? width : static_cast<int>(std::ceil(static_cast<float>(width) / face->scale));
        }

        int GenerateString(Rml::RenderManager& manager, Rml::FontFaceHandle handle, Rml::FontEffectsHandle effectsHandle, Rml::StringView string,
            Rml::Vector2f position, Rml::ColourbPremultiplied colour, float opacity, const Rml::TextShapingContext& context,
            Rml::TexturedMeshList& meshes) override
        {
            const Face* face = faceOf(handle);
            if (!face)
            {
                return 0;
            }
            const float scale = face->scale;
            const Rml::FontEffectsHandle effects = (effectsHandle >= 1 && effectsHandle <= m_effects.size()) ? m_effects[effectsHandle - 1].inner : 0;
            const Rml::TextShapingContext scaled = scaledContext(context, scale);
            if (scale == 1.0f)
            {
                return m_inner->GenerateString(manager, face->inner, effects, string, position, colour, opacity, scaled, meshes);
            }
            // The engine adds the line's glyphs to the meshes already in the list (one mesh per atlas texture), so the line goes
            // into a list of its own to be scaled back, and then in as new meshes.
            Rml::TexturedMeshList line;
            const int width = m_inner->GenerateString(manager, face->inner, effects, string, position * scale, colour, opacity, scaled, line);
            for (Rml::TexturedMesh& part : line)
            {
                for (Rml::Vertex& v : part.mesh.vertices)
                {
                    v.position /= scale;
                }
                meshes.push_back(std::move(part));
            }
            return static_cast<int>(std::ceil(static_cast<float>(width) / scale));
        }

        int GetVersion(Rml::FontFaceHandle handle) override
        {
            const Face* face = faceOf(handle);
            return face ? m_inner->GetVersion(face->inner) : 0;
        }

        void ReleaseFontResources() override
        {
            m_faces.clear();
            m_effects.clear();
            m_byKey.clear();
            m_inner->ReleaseFontResources();
        }

    private:
        struct Face
        {
            Rml::FontFaceHandle inner = 0;
            float scale = 1.0f;
            Rml::FontMetrics metrics{};
        };
        struct Effects
        {
            Rml::FontEffectsHandle inner = 0;
        };

        // Pixels of the screen per layout unit for a document that is laid out in layout units.
        static float currentScale()
        {
            if (g_fontDp)
            {
                return 1.0f;
            }
            const float scale = Ui::canvasScale();
            return scale > 1.0f ? std::min(scale, 8.0f) : 1.0f;
        }
        static Rml::TextShapingContext scaledContext(const Rml::TextShapingContext& context, float scale)
        {
            Rml::TextShapingContext out{context.language};
            out.text_direction = context.text_direction;
            out.font_kerning = context.font_kerning;
            out.letter_spacing = context.letter_spacing * scale;
            return out;
        }
        const Face* faceOf(Rml::FontFaceHandle handle) const
        {
            return (handle >= 1 && handle <= m_faces.size()) ? &m_faces[handle - 1] : nullptr;
        }

        Rml::FontEngineInterface* m_inner;
        std::vector<Face> m_faces;
        std::vector<Effects> m_effects;
        std::unordered_map<std::string, Rml::FontFaceHandle> m_byKey;
        Rml::FontMetrics m_none{};
    };

    // RmlUi starts with the first document; false (with the reason logged) if it cannot.
    bool ensureStarted()
    {
        if (g_started)
        {
            return true;
        }
        if (g_startFailed)
        {
            return false;
        }
        g_system = new SystemInterface();
        g_file = new FileInterface();
        g_render = new RenderInterface();
        Rml::SetSystemInterface(g_system);
        Rml::SetFileInterface(g_file);
        Rml::SetRenderInterface(g_render);
        if (!Rml::Initialise())
        {
            LOG("RmlUi: Initialise failed");
            g_startFailed = true;
            return false;
        }
        g_started = true;
        // caret-style: block (the thin vertical bar, RmlUi's own) or underscore (a bar at the foot of the line, as wide as a "_": the game's text fields).
        // Needs the patched RmlUi of ports/rmlui (caret-style-underscore.patch); a stock RmlUi ignores the property.
        Rml::StyleSheetSpecification::RegisterProperty("caret-style", "block", false).AddParser("keyword", "block, underscore");
        Rml::SetFontEngineInterface(new ScaledFontEngine(Rml::GetFontEngineInterface()));
        g_content = new GearCss::CssContentSupport();
        g_content->registerProperty();
        {
            static Rml::ElementInstancerGeneric<RingElement> ringInstancer;
            Rml::Factory::RegisterElementInstancer("gear-ring", &ringInstancer);
        }
        // The UI's own face (Segoe UI, as gear.ui text) with its styles; symbols as the fallback.
        const fs::path fonts = fontsDir();
        g_allowFonts = true;
        loadFont(fonts / "segoeui.ttf", true);
        loadFont(fonts / "segoeuib.ttf", false);
        loadFont(fonts / "segoeuii.ttf", false);
        loadFont(fonts / "segoeuiz.ttf", false);
        loadFont(fonts / "seguisym.ttf", true);
        g_allowFonts = false;
        registerThemeFonts(fonts);
        LOG("RmlUi {} started", Rml::GetVersion());
        return true;
    }

    // The context's size and density for the canvas as it is now; `dp` documents are laid out in screen pixels
    // (1dp = one layout unit), the others in layout units (1px = one layout unit).
    void fitContext(Doc& d)
    {
        const float unit = d.dp ? std::max(0.5f, Ui::canvasScale()) : 1.0f;
        // As large as the screen where that is more than the 1024 x 768 layout (a game laid out in screen pixels, a wide
        // screen): a document's elements past the context's edge lose their text.
        float right = kLayoutWidth, bottom = kLayoutHeight;
        Ui::screenExtent(right, bottom);
        // The context starts at the screen's own corner (left of and above the layout on a wide or tall screen): text outside the context is not drawn at
        // all, so a mod's text that lies out of the 1024 x 768 layout would vanish. A document that starts at that corner (the game's windows sit there)
        // measures its positions from it, `originPx` is where the corner lies in layout pixels; the usual document keeps its (0, 0) at the layout's
        // corner: its <body> is moved by the margin (the same pixels the other way), so `left: -200px` is where it was, and now has text.
        float left = 0.0f, top = 0.0f;
        Ui::screenOrigin(left, top);
        const Rml::Vector2f origin(std::floor(left * unit), std::floor(top * unit));
        const Rml::Vector2i dims(static_cast<int>(std::lround(std::ceil(right) * unit - origin.x)), static_cast<int>(std::lround(std::ceil(bottom) * unit - origin.y)));
        d.originPx = origin;
        d.bodyPx = d.screenOrigin ? origin : Rml::Vector2f(0.0f, 0.0f);
        if (unit != d.unit || d.context->GetDimensions() != dims)
        {
            d.unit = unit;
            d.context->SetDimensions(dims);
            d.context->SetDensityIndependentPixelRatio(d.dp ? unit : 1.0f);
        }
        const Rml::Vector2f shift = d.screenOrigin ? Rml::Vector2f(0.0f, 0.0f) : Rml::Vector2f(-origin.x, -origin.y);
        if (d.document && (d.shiftedDocument != d.document || shift != d.shiftApplied))
        {
            if (shift.x != 0.0f || shift.y != 0.0f || d.shiftedDocument == d.document)
            {
                d.document->SetProperty("margin-left", std::to_string(static_cast<int>(std::lround(shift.x))) + "px");
                d.document->SetProperty("margin-top", std::to_string(static_cast<int>(std::lround(shift.y))) + "px");
            }
            d.shiftedDocument = d.document;
            d.shiftApplied = shift;
        }
    }

    Doc* findDoc(int id)
    {
        const auto it = g_docs.find(id);
        return it == g_docs.end() ? nullptr : it->second.get();
    }

    Rml::Element* find(Doc& d, const std::string& selector)
    {
        // The document element is the <body>: a descendant query never finds it.
        if (selector.empty() || selector == "body")
        {
            return d.document;
        }
        return d.document->QuerySelector(selector);
    }

    // The handle of an element: the number in its data-gh attribute, given on first use. Lua addresses the element with the selector
    // [data-gh="N"], so a handle to an element that was removed simply matches nothing.
    int handleOf(Doc& d, Rml::Element* e)
    {
        int h = e->GetAttribute<int>("data-gh", 0);
        if (h == 0)
        {
            h = ++d.nextHandle;
            e->SetAttribute("data-gh", h);
        }
        return h;
    }

    bool isRange(Rml::Element* e)
    {
        return e && e->GetTagName() == "input" && e->GetAttribute<Rml::String>("type", "") == "range";
    }

    // The modifier bits Lua passes (1 Ctrl, 2 Shift, 4 Alt) as RmlUi's key modifier state.
    int modifiersArg(lua_State* L, int index)
    {
        const int bits = lua_isnumber(L, index) ? static_cast<int>(lua_tonumber(L, index)) : 0;
        return ((bits & 1) ? Rml::Input::KM_CTRL : 0) | ((bits & 2) ? Rml::Input::KM_SHIFT : 0) | ((bits & 4) ? Rml::Input::KM_ALT : 0);
    }

    // A text field: a textarea or an input of a typing kind (the elements that take Ctrl+A / C / X / V, the mouse selection and Shift + arrows).
    bool isTextField(Rml::Element* e)
    {
        if (!e)
        {
            return false;
        }
        const std::string tag = e->GetTagName();
        if (tag == "textarea")
        {
            return true;
        }
        if (tag != "input")
        {
            return false;
        }
        const std::string type = e->GetAttribute<Rml::String>("type", "text");
        return type != "checkbox" && type != "radio" && type != "submit" && type != "button" && type != "range" && type != "hidden";
    }

    constexpr const char* kNavSelector = "button, a, input, select, textarea, [data-nav]";

    // The elements the pad and the mouse can press, in document order.
    // Whether the centre of the element is outside the box of an ancestor that clips (overflow other than visible): a row of a scrolled list that
    // is out of sight. It is no pad stop then (the pad would put its cursor on something nobody can see).
    bool scrolledOut(Rml::Element* e)
    {
        const Rml::Vector2f o = e->GetAbsoluteOffset(Rml::BoxArea::Border);
        const Rml::Vector2f s = e->GetBox().GetSize(Rml::BoxArea::Border);
        const float cx = o.x + s.x * 0.5f, cy = o.y + s.y * 0.5f;
        for (Rml::Element* p = e->GetParentNode(); p; p = p->GetParentNode())
        {
            const Rml::Style::Overflow ox = p->GetComputedValues().overflow_x(), oy = p->GetComputedValues().overflow_y();
            if (ox == Rml::Style::Overflow::Visible && oy == Rml::Style::Overflow::Visible)
            {
                continue;
            }
            const Rml::Vector2f po = p->GetAbsoluteOffset(Rml::BoxArea::Padding);
            const Rml::Vector2f ps = p->GetBox().GetSize(Rml::BoxArea::Padding);
            if (cx < po.x || cx > po.x + ps.x || cy < po.y || cy > po.y + ps.y)
            {
                return true;
            }
        }
        return false;
    }

    // A box the pad's up / down walks card by card: one that scrolls (overflow-y auto or scroll, with more content than room), or any marked data-nav-scroll.
    bool isNavScrollArea(Rml::Element* p)
    {
        if (p->HasAttribute("data-nav-scroll"))
        {
            return true;
        }
        const Rml::Style::Overflow oy = p->GetComputedValues().overflow_y();
        return (oy == Rml::Style::Overflow::Auto || oy == Rml::Style::Overflow::Scroll) && p->GetScrollHeight() > p->GetClientHeight() + 0.5f;
    }

    // The nearest ancestor that is such a box.
    Rml::Element* navScrollArea(Rml::Element* e)
    {
        for (Rml::Element* p = e->GetParentNode(); p; p = p->GetParentNode())
        {
            if (isNavScrollArea(p))
            {
                return p;
            }
        }
        return nullptr;
    }

    void collectNavScrollAreas(Rml::Element* e, std::vector<Rml::Element*>& out)
    {
        if (e->IsVisible(true) && isNavScrollArea(e))
        {
            out.push_back(e);
        }
        for (int i = 0; i < e->GetNumChildren(); ++i)
        {
            collectNavScrollAreas(e->GetChild(i), out);
        }
    }

    std::vector<Rml::Element*> navElements(Doc& d)
    {
        std::vector<Rml::Element*> list;
        if (!d.document->IsVisible())
        {
            return list;
        }
        Rml::ElementList all;
        d.document->QuerySelectorAll(all, kNavSelector);
        for (Rml::Element* e : all)
        {
            if (!e->IsVisible(true) || e->GetAttribute("disabled") != nullptr || scrolledOut(e))
            {
                continue;
            }
            const Rml::Vector2f size = e->GetBox().GetSize(Rml::BoxArea::Border);
            if (size.x <= 0 || size.y <= 0)
            {
                continue;
            }
            list.push_back(e);
        }
        // Stacking order: the regions Lua registers for the stops are won by the last one at a point, so what is in front (a z-index, as a drop-down list
        // has over the rows under it) comes after what is behind; the document's order otherwise.
        const auto rank = [](Rml::Element* e) {
            float z = 0.0f;
            for (Rml::Element* p = e; p; p = p->GetParentNode())
            {
                const Rml::Style::NumberAuto zi = p->GetComputedValues().z_index();
                if (zi.type == Rml::Style::NumberAuto::Number)
                {
                    z = std::max(z, zi.value);
                }
            }
            return z;
        };
        std::stable_sort(list.begin(), list.end(), [&](Rml::Element* a, Rml::Element* b) { return rank(a) < rank(b); });
        return list;
    }

    // ---- Lua ----

    // A native that fails with a message: C++ exceptions must not cross LuaJIT's frames.
    struct Failure
    {
        std::string message;
    };

    template <int (*F)(lua_State*)>
    int guarded(lua_State* L)
    {
        std::string failure;
        try
        {
            return F(L);
        }
        catch (const Failure& f)
        {
            failure = f.message;
        }
        catch (const std::exception& e)
        {
            failure = e.what();
        }
        catch (...)
        {
            failure = "unknown error";
        }
        return luaL_error(L, "rml: %s", failure.c_str());
    }

    std::string str(lua_State* L, int arg)
    {
        size_t n = 0;
        const char* s = luaL_checklstring(L, arg, &n);
        return std::string(s, n);
    }

    std::string optStr(lua_State* L, int arg)
    {
        size_t n = 0;
        const char* s = luaL_optlstring(L, arg, "", &n);
        return std::string(s, n);
    }

    void push(lua_State* L, const std::string& s) { lua_pushlstring(L, s.data(), s.size()); }

    // The origin of the document the current native call works on (see Doc::originPx): zero for the usual document.
    Rml::Vector2f g_origin{0.0f, 0.0f};

    // An element's offset in layout pixels (the context's pixels, plus the origin of a document that starts at the screen's corner).
    Rml::Vector2f absOffset(Rml::Element* e, Rml::BoxArea area)
    {
        return e->GetAbsoluteOffset(area) + g_origin;
    }

    Doc& docArg(lua_State* L, int arg = 1)
    {
        Doc* d = findDoc(static_cast<int>(luaL_checknumber(L, arg)));
        if (!d)
        {
            throw Failure{"the document is closed"};
        }
        g_origin = d->originPx;
        return *d;
    }

    int pushFail(lua_State* L, const std::string& message)
    {
        lua_pushnil(L);
        push(L, message);
        return 2;
    }

    // RmlUi has no built-in user agent stylesheet: without one a div, p or h1 is inline and the lines run together.
    // Put in front of every document (and so of the theme and the document's own styles).
    const char* const kUserAgentStyle =
        "<style>"
        "div, p, h1, h2, h3, h4, h5, h6, ul, ol, li, dl, dt, dd, form, header, footer, section, article, nav, aside, pre, hr "
        "{ display: block; }"
        "h1, h2, h3, h4, h5, h6, strong, b { font-weight: bold; }"
        "body { font-family: \"Segoe UI\"; }"
                "em, i { font-style: italic; }"
        "template { display: block; }"
        "</style>";

    // The icon font's classes (generated: src/gear/ui/icons_css.inc); <span class="icon icon-refresh"></span>. Pre-processed like every stylesheet (::before, content).
    const char* const kIconStyle =
#include "icons_css.inc"
        ;

    // The look of a document when no theme mod is running (a mod with `theme = 'file.rcss'`): plain and readable, with the class names the themes style, so
    // that a document written for a theme still works: dark panels, gold edges, simple buttons, check boxes, sliders and scroll bars, the controller prompts.
    // (The game's own frames behind .window / .thick-frame are Gear's and do not depend on a stylesheet.) A theme replaces all of it.
    const char* const kBaseStyle = R"CSS(<style>
body { font-size: 15dp; color: #f0f0f0; }
.window { padding: 6px; }
.window > .titlebar { font-family: "Sacred Game"; padding: 4px 10px; background-color: #302418; color: #f0c860; }
.window > .body { padding: 10px 14px 12px 14px; }
.thick-frame { padding: 29px 26px 32px 26px; }
.heading { font-family: "Sacred Game"; color: #f0c860; margin: 8px 0 4px 0; }
.label { color: #9a9a9a; }
.value { color: #f0f0f0; }
.muted { color: #8c8c8c; }
.accent { color: #ffd780; }
.good { color: #7fd37f; }
.bad { color: #e06060; }
.small { font-size: 12px; }
.wrap { white-space: pre-wrap; }
.grow { flex: 1 1 auto; }
.warn { color: #ffc878; }
.gold { color: #ffd778; }
.xp { color: #9cd0ff; }
button.dis { color: #8c8c8c; border-color: #3c3c3c; opacity: 0.6; }
.row { display: flex; flex-direction: row; margin: 6px 0; }
.row.right { justify-content: flex-end; }
.row.space { justify-content: space-between; }
.font-game { font-family: "Sacred Game"; }
.font-start { font-family: "Sacred Start"; }
.font-default { font-family: "Segoe UI"; }
button { margin-right: 8px; padding: 4px 14px; line-height: 1.5; color: #f0f0f0; background-color: #14141f; border: 1px #8c7440; cursor: pointer; }
button:hover, button:focus { background-color: #24243a; border-color: #ffd780; color: #ffe030; }
button:active { background-color: #34344e; }
button.primary { border-color: #c8aa5a; color: #ffd780; }
button.danger { border-color: #a84030; }
button:disabled, button[disabled] { color: #8c8c8c; border-color: #3c3c3c; }
button.game-button { display: inline-block; height: 34px; min-width: 100px; padding: 0 22px; margin: 0 8px 0 0; line-height: 32px; text-align: center; color: #e4e4e4; font-family: "Sacred Game"; font-size: 18px; background-color: #1c1c28; border: 1px #8c7440; }
button.game-button:hover, button.game-button:focus { color: #ffe030; border-color: #ffe030; background-color: #262636; }
button.game-button:active { color: #fff2a0; background-color: #34344e; }
button.game-button:disabled, button.game-button[disabled] { color: #8a8a8a; opacity: 0.55; }
button { flex: none; white-space: nowrap; }
button.mini { margin: 0 4px 0 0; padding: 0 10px; line-height: 1.4; font-size: 13px; }
button.ibtn { margin: 0 4px 0 0; padding: 2px 8px; border: 0; background-color: transparent; color: #e8d8a8; font-size: 16px; }
button.ibtn:hover, button.ibtn:focus { background-color: transparent; color: #fff6d0; }
button.ibtn + button.ibtn { border-left: 1px #8c744099; margin-left: 4px; padding-left: 12px; }
button.mini.dis { color: #8c8c8c; border-color: #3c3c3c; opacity: 0.6; }
.dd { position: relative; display: inline-block; flex: none; z-index: 10; }
.dd.open { z-index: 100; }
.dd-list { position: absolute; left: 0; top: 100%; min-width: 150px; max-height: 300px; overflow-y: auto; z-index: 200; background-color: #0c0c14; border: 1px #ffd780; padding: 2px; }
.dd-list button.opt { display: block; width: auto; min-width: 0; height: auto; margin: 0; padding: 3px 10px; line-height: 1.4; font-size: 15px; text-align: left; white-space: nowrap; color: #e8e8e8; border: 0; background-color: transparent; }
.dd-list button.opt:hover, .dd-list button.opt:focus, .dd-list button.opt.on { background-color: #ffd78030; border: 0; color: #ffe030; }
.dd-shade { position: absolute; left: 0; top: 0; right: 0; bottom: 0; z-index: 5; }
button.game-dropdown { display: inline-block; height: 37px; min-width: 130px; margin: 0 8px 0 0; padding: 0 40px; line-height: 31px; text-align: center; white-space: nowrap; color: #e4e4e4; font-family: "Sacred Game"; font-size: 16px; background-color: transparent; border: 0; decorator: image(gear:image/281 repeat-x) content-box, image(gear:image/267 scale-none left center) border-box, image(gear:image/270 scale-none right center) border-box; }
button.game-dropdown:hover, button.game-dropdown:focus { color: #ffe030; background-color: transparent; border: 0; }
button.game-dropdown:disabled, button.game-dropdown[disabled] { color: #8a8a8a; opacity: 0.55; }
input.field, select.field { padding: 4px 8px; color: #f0f0f0; background-color: #0c0c14; border: 1px #6e5e3a; }
input.field:focus, select.field:focus { border-color: #ffd780; }
input selection, textarea selection { background-color: #c8aa5aa0; color: #ffffff; }
input.game-check { display: inline-block; width: 22px; height: 22px; margin: 0 15px; padding: 0; background-color: #0c0c14; border: 2px #8c7440; cursor: pointer; }
input.game-check:checked { background-color: #ffd780; border-color: #ffd780; }
input.game-check:hover, input.game-check:focus { border-color: #ffe030; }
input.game-check[disabled] { opacity: 0.5; }
.check-row { display: flex; flex-direction: row; align-items: center; justify-content: space-between; height: 40px; }
.check-row .text { font-family: "Sacred Game"; color: #e4e4e4; font-size: 18px; }
input.game-slider { display: inline-block; width: 300px; height: 24px; margin: 6px 0; padding: 0; border: 0; background-color: transparent; cursor: pointer; }
input.game-slider slidertrack { height: 8px; margin-top: 8px; background-color: #0c0c14; border: 1px #6e5e3a; }
input.game-slider sliderbar { width: 14px; height: 24px; background-color: #ffd780; }
input.game-slider sliderarrowdec, input.game-slider sliderarrowinc { width: 22px; height: 24px; background-color: #24243a; border: 1px #8c7440; }
input.game-slider[disabled] { opacity: 0.5; }
.slider-row { display: flex; flex-direction: row; align-items: center; justify-content: space-between; height: 44px; }
.slider-row .text { font-family: "Sacred Game"; color: #e4e4e4; font-size: 18px; }
scrollbarvertical { width: 16px; }
scrollbarvertical slidertrack { width: 16px; background-color: #0c0c14; }
scrollbarvertical sliderbar { width: 16px; min-height: 24px; background-color: #8c7440; }
scrollbarvertical sliderarrowdec, scrollbarvertical sliderarrowinc { width: 16px; height: 16px; background-color: #24243a; }
.list { margin: 4px 0; }
.list .item { padding: 6px 10px; margin-bottom: 4px; background-color: #12121c; border: 1px #4a4030; }
.list .item:hover { border-color: #c8aa5a; }
.list button.item { display: block; margin-right: 0; text-align: left; }
.list .item.selected { background-color: #4a3f2a; border-color: #ffd780; }
.item .name { font-family: "Sacred Game"; color: #ffd780; font-weight: bold; }
.item .sub { color: #9a9a9a; }
.bar { height: 10px; margin: 4px 0; background-color: #0c0c14; border: 1px #6e5e3a; }
.bar .fill { height: 10px; background-color: #a82828; }
.bar.mana .fill { background-color: #2f6fd0; }
.bar.xp .fill { background-color: #7fd37f; }
.link { display: inline-block; padding: 4px 22px; color: #e8d8a8; cursor: pointer; text-align: center; }
.link:hover, .link:focus { color: #fff6d0; font-effect: glow(2px 6px 0px 0px #ffb840e6); }
.link:active { color: #ffffff; }
.tabbar { display: flex; flex-direction: row; align-items: center; padding: 0 0 4px 0; margin: 0 0 10px 0; border-bottom: 1px #8c7440c0; }
.tabbar a.link.tab { padding: 3px 14px; font-family: "Sacred Game"; font-size: 17px; }
.tabbar a.link.tab.on { color: #ffcf40; border-bottom: 2px #ffcf40; margin-bottom: -5px; padding-bottom: 5px; }
.tabbar a.link.tab + a.link.tab { border-left: 1px #8c744099; margin-left: 6px; padding-left: 20px; }
.tabbar .tab-sep { width: 1px; height: 20px; margin: 0 6px; background-color: #8c744099; }
.padkey { display: none; width: 1.3em; height: 1.3em; margin-right: 0.5em; vertical-align: middle; }
body.pad .padkey { display: inline-block; }
body.pad .padkey.a { display: none; }
body.pad button:focus > .padkey.a, body.pad button:hover > .padkey.a, body.pad a:focus > .padkey.a, body.pad a:hover > .padkey.a { display: inline-block; }
body.pad button:focus > span.padkey.b, body.pad button:hover > span.padkey.b, body.pad button:focus > span.padkey.y, body.pad button:hover > span.padkey.y,
body.pad button:focus > span.padkey.x, body.pad button:hover > span.padkey.x, body.pad button:focus > span.padkey.lb, body.pad button:hover > span.padkey.lb,
body.pad button:focus > span.padkey.rb, body.pad button:hover > span.padkey.rb, body.pad button:focus > span.padkey.back, body.pad button:hover > span.padkey.back { decorator: image(gear:pad/a contain); }
span.padkey.a { decorator: image(gear:pad/a contain); }
span.padkey.b { decorator: image(gear:pad/b contain); }
span.padkey.x { decorator: image(gear:pad/x contain); }
span.padkey.y { decorator: image(gear:pad/y contain); }
span.padkey.lb { decorator: image(gear:pad/lb contain); }
span.padkey.rb { decorator: image(gear:pad/rb contain); }
span.padkey.lt { decorator: image(gear:pad/lt contain); }
span.padkey.rt { decorator: image(gear:pad/rt contain); }
span.padkey.back { decorator: image(gear:pad/back contain); }
span.padkey.start { decorator: image(gear:pad/start contain); }
span.padkey.l3 { decorator: image(gear:pad/l3 contain); }
span.padkey.r3 { decorator: image(gear:pad/r3 contain); }
span.padkey.up { decorator: image(gear:pad/up contain); }
span.padkey.down { decorator: image(gear:pad/down contain); }
span.padkey.left { decorator: image(gear:pad/left contain); }
span.padkey.right { decorator: image(gear:pad/right contain); }
a.arrow { display: inline-block; width: 40px; height: 28px; margin: 0 2px; vertical-align: middle; cursor: pointer; decorator: image(gear:image/236 scale-none center center); }
a.arrow.next { decorator: image(gear:image/237 scale-none center center); }
a.arrow:hover, a.arrow:focus { opacity: 0.75; }
a.arrow.off { opacity: 0.35; cursor: default; }
gear-ring { display: inline-block; width: 40px; height: 40px; }
</style>)CSS";

    // Puts the user agent style and <link> to the theme's stylesheet (if any) at the start of the document's head (a
    // head is added if there is none); `base`: the plain base style when there is no theme.
    void injectTheme(std::string& markup, const fs::path& themeFile, bool base)
    {
        std::string link = kUserAgentStyle;
        link += "<style>";
        link += kIconStyle;
        link += "</style>";
        if (themeFile.empty() && base)
        {
            link += kBaseStyle;
        }
        if (!themeFile.empty())
        {
            std::string href = toUtf8(themeFile);
            std::replace(href.begin(), href.end(), '\\', '/');
            if (href.size() > 1 && href[1] == ':')
            {
                href.insert(0, "file:///");     // see create: a bare drive path is no URL
            }
            link += "<link type=\"text/rcss\" href=\"" + href + "\"/>";
        }
        std::string lower = markup;
        for (char& c : lower)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        size_t at = lower.find("<head>");
        if (at != std::string::npos)
        {
            markup.insert(at + 6, link);
            return;
        }
        at = lower.find("<head ");
        if (at != std::string::npos && (at = lower.find('>', at)) != std::string::npos)
        {
            markup.insert(at + 1, link);
            return;
        }
        at = lower.find("<body");
        markup.insert(at == std::string::npos ? 0 : at, "<head>" + link + "</head>");
    }

    // (Re)loads the document of `d` into its context: reads the markup, puts the theme in front and records the files it pulls in (d.deps).
    void loadInto(Doc& d)
    {
        d.deps.clear();
        g_deps = &d.deps;
        {
            RootScope scope(d);
            std::string markup = d.source;
            bool inMemory = d.fromString;
            if (!d.fromString)
            {
                inMemory = readAll(d.file, markup);
            }
            if (inMemory)
            {
                injectTheme(markup, d.themeFile, d.base);
                markup = preprocessCss(markup, logicalDir(toUtf8(d.file), d.file), true);       // (after the injection: the icon classes use ::before)
            }
            // A path with a drive letter ("B:/...") is no URL to RmlUi (it takes "B" for a protocol and logs an error per
            // link); "file:///B:/..." is one, and its relative links resolve against it into plain paths again.
            std::string url = toUtf8(d.file);
            if (url.size() > 1 && url[1] == ':')
            {
                url.insert(0, "file:///");
            }
            d.document = inMemory ? d.context->LoadDocumentFromMemory(markup, url) : d.context->LoadDocument(url);
        }
        g_deps = nullptr;
    }

    // Creates the context and document; `source` is the file (or, with `fromString`, the markup).
    int create(lua_State* L, bool fromString)
    {
        const std::string dir = str(L, 1);
        const std::string source = str(L, 2);
        const bool dp = lua_toboolean(L, 3) != 0;
        const fs::path root = modRoot(dir);
        if (root.empty())
        {
            return pushFail(L, "not a mod folder");
        }
        if (g_docs.size() >= kMaxDocuments)
        {
            return pushFail(L, "too many documents");
        }
        if (!ensureStarted())
        {
            return pushFail(L, "RmlUi did not start (see SacredGear.log)");
        }
        fs::path file = root / "inline.rml";
        if (!fromString)
        {
            file = root / fromUtf8(source);
            if (!inside(root, file))
            {
                return pushFail(L, "the path leaves the mod's folder");
            }
        }
        auto d = std::make_unique<Doc>();
        d->id = g_nextDoc++;
        d->mod = dir;
        d->root = root;
        d->dp = dp;
        d->contextName = "gear" + std::to_string(d->id);
        d->context = Rml::CreateContext(d->contextName, Rml::Vector2i(kLayoutWidth, kLayoutHeight));
        if (!d->context)
        {
            return pushFail(L, "cannot create the context");
        }
        fitContext(*d);
        if (lua_istable(L, 4))
        {
            // The data model "model" (data-model="model" in the document): the table's top-level keys, and the names of the
            // callbacks of data-event-* attributes (the list in argument 5).
            toNode(L, 4, d->model, 0);
            if (d->model.type != Node::Type::Obj)
            {
                d->model = Node();
                d->model.type = Node::Type::Obj;
            }
            Rml::DataModelConstructor ctor = d->context->CreateDataModel("model");
            if (ctor)
            {
                for (auto& [key, value] : d->model.obj)
                {
                    ctor.BindCustomDataVariable(key, variableOf(value));
                }
                Doc* doc = d.get();
                if (lua_istable(L, 5))
                {
                    const int count = static_cast<int>(lua_objlen(L, 5));
                    for (int i = 1; i <= count; ++i)
                    {
                        lua_rawgeti(L, 5, i);
                        const char* name = lua_tostring(L, -1);
                        if (name)
                        {
                            ctor.BindEventCallback(name, [doc, call = std::string(name)](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& arguments) {
                                if (doc->events.size() >= 512)
                                {
                                    return;
                                }
                                QueuedEvent q;
                                q.call = call;
                                for (const Rml::Variant& a : arguments)
                                {
                                    Rml::String text;
                                    q.args.push_back(a.GetInto(text) ? text : std::string());
                                }
                                doc->events.push_back(std::move(q));
                            });
                        }
                        lua_pop(L, 1);
                    }
                }
                d->modelHandle = ctor.GetModelHandle();
                d->hasModel = true;
            }
        }
        // The theme mod's stylesheet (arguments 6 and 7: its folder and the .rcss in it) goes in front of the document's own.
        fs::path themeFile;
        if (lua_isstring(L, 6) && lua_isstring(L, 7) && (lua_toboolean(L, 8) || str(L, 6) != dir))      // 8: a built-in mod that borrows the theme's folder
        {
            const fs::path themeRoot = modRoot(str(L, 6));
            const fs::path candidate = themeRoot / fromUtf8(str(L, 7));
            if (!themeRoot.empty() && inside(themeRoot, candidate))
            {
                d->themeRoot = themeRoot;
                themeFile = candidate;
            }
        }
        d->file = file;
        d->source = source;
        d->fromString = fromString;
        d->themeFile = themeFile;
        d->base = lua_toboolean(L, fromString ? 9 : 8) != 0;
        loadInto(*d);
        if (!d->document)
        {
            Rml::RemoveContext(d->contextName);
            return pushFail(L, "cannot load the document (see SacredGear.log)");
        }
        d->document->Show();
        const int id = d->id;
        g_docs.emplace(id, std::move(d));
        lua_pushnumber(L, id);
        return 1;
    }

    // native.rml_origin(id, on): the document's (0, 0) is the screen's top left corner (where the game's own windows start), not the layout's: it
    // reaches left of the layout on a wide screen. The positions Lua sees stay in layout units.
    int rmlOrigin(lua_State* L)
    {
        Doc& d = docArg(L);
        d.screenOrigin = lua_toboolean(L, 2) != 0;
        fitContext(d);
        g_origin = d.originPx;
        return 0;
    }

    // native.rml_load(dir, path, dp) -> id | nil, message
    int rmlLoad(lua_State* L) { return create(L, false); }

    // native.rml_load_string(dir, rml, dp) -> id | nil, message (relative links resolve against the mod's folder)
    int rmlLoadString(lua_State* L) { return create(L, true); }

    // native.rml_model_set(id, key, value) -> bool: replaces the value of a top-level key of the document's data model
    // (the keys of the table given to load) and has the bindings refresh.
    int rmlModelSet(lua_State* L)
    {
        Doc& d = docArg(L);
        const std::string key = str(L, 2);
        if (!d.hasModel)
        {
            lua_pushboolean(L, 0);
            return 1;
        }
        for (auto& [name, value] : d.model.obj)
        {
            if (name == key)
            {
                toNode(L, 3, value, 0);
                d.modelHandle.DirtyVariable(key);
                lua_pushboolean(L, 1);
                return 1;
            }
        }
        lua_pushboolean(L, 0);
        return 1;
    }

    // native.rml_model_changed(id) -> table | nil: the whole data model, if the document changed a value itself (data-value) since
    // the last call.
    int rmlModelChanged(lua_State* L)
    {
        Doc& d = docArg(L);
        const unsigned edits = g_modelEdits.load();
        if (!d.hasModel || edits == d.modelSeen)
        {
            lua_pushnil(L);
            return 1;
        }
        d.modelSeen = edits;
        pushNode(L, d.model);
        return 1;
    }

    // native.rml_close(id)
    int rmlClose(lua_State* L)
    {
        const int id = static_cast<int>(luaL_checknumber(L, 1));
        const auto it = g_docs.find(id);
        if (it == g_docs.end())
        {
            return 0;
        }
        {
            Doc& d = *it->second;
            RootScope scope(d);
            d.context->UnloadDocument(d.document);
            Rml::RemoveContext(d.contextName);
            d.listeners.clear();
        }
        g_docs.erase(it);
        return 0;
    }

    // native.rml_show(id, visible)
    int rmlShow(lua_State* L)
    {
        Doc& d = docArg(L);
        if (lua_toboolean(L, 2))
        {
            d.document->Show();
        }
        else
        {
            d.document->Hide();
        }
        return 0;
    }

    // Every button of the document that has no controller prompt gets the confirm one (<span class="padkey a">, shown while the pad is on the button): a
    // document does not have to carry it, and the buttons of an unstyled or older document show one like the rest.
    void ensureButtonPrompts(Doc& d)
    {
        Rml::ElementList buttons;
        d.document->QuerySelectorAll(buttons, "button");
        for (Rml::Element* b : buttons)
        {
            bool has = false;
            for (int i = 0; i < b->GetNumChildren() && !has; ++i)
            {
                has = b->GetChild(i)->IsClassSet("padkey");
            }
            if (has || b->HasAttribute("data-no-padkey"))
            {
                continue;
            }
            Rml::ElementPtr span = d.document->CreateElement("span");
            span->SetClassNames("padkey a");
            b->InsertBefore(std::move(span), b->GetFirstChild());
        }
    }

    // ---- Hot reload (a developer's tool: g_hotReload) ----
    // The files a document read are polled; when one changes the document is loaded again into the same context (the data model, the
    // listeners and the scroll positions of boxes with an id carry over; a document that fails to load stays as it was, the error is kept in
    // Doc::hotError and logged).

    bool isImageFile(const fs::path& p)
    {
        const std::string e = toUtf8(p.extension());
        return e == ".png" || e == ".tga" || e == ".jpg" || e == ".jpeg";
    }

    bool depsChanged(const Doc& d, bool& imagesOnly)
    {
        bool changed = false;
        imagesOnly = true;
        for (const auto& [path, time] : d.deps)
        {
            std::error_code ec;
            const fs::file_time_type now = fs::last_write_time(path, ec);
            if (ec || now != time)
            {
                changed = true;
                imagesOnly = imagesOnly && isImageFile(path);
            }
        }
        return changed;
    }

    void hotReload(Doc& d, bool imagesOnly)
    {
        // scroll positions by element id
        std::vector<std::pair<std::string, float>> scrolls;
        {
            Rml::ElementList all;
            d.document->QuerySelectorAll(all, "[id]");
            for (Rml::Element* e : all)
            {
                if (e->GetScrollTop() > 0.0f)
                {
                    scrolls.emplace_back(e->GetId(), e->GetScrollTop());
                }
            }
        }
        Rml::Factory::ClearStyleSheetCache();
        Rml::Factory::ClearTemplateCache();
        if (imagesOnly)
        {
            Rml::ReleaseTextures();
        }
        Rml::ElementDocument* old = d.document;
        const bool visible = old->IsVisible();
        const auto oldDeps = d.deps;
        d.document = nullptr;
        loadInto(d);
        if (!d.document)
        {
            d.document = old;
            d.deps = oldDeps;
            for (auto& [path, time] : d.deps)
            {
                std::error_code ec;
                const auto t = fs::last_write_time(path, ec);
                if (!ec)
                {
                    time = t;       // do not try again until the file changes once more
                }
            }
            d.hotError = "cannot load the document: " + toUtf8(d.file);
            LOG("RmlUi hot reload: {} (the old one stays)", d.hotError);
            return;
        }
        d.hotError.clear();
        d.context->UnloadDocument(old);
        if (visible)
        {
            d.document->Show();
        }
        for (size_t i = 0; i < d.listeners.size() && i < d.listenerEvents.size(); ++i)
        {
            const std::string& sel = d.listeners[i]->selector();
            d.document->AddEventListener(d.listenerEvents[i], d.listeners[i].get(), !sel.empty());
        }
        d.context->Update();
        for (const auto& [id, top] : scrolls)
        {
            if (Rml::Element* e = d.document->GetElementById(id))
            {
                e->SetScrollTop(top);
            }
        }
        ++d.reloads;
        LOG("RmlUi hot reload: {}", toUtf8(d.file));
    }

    void hotPoll(Doc& d)
    {
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now - d.hotChecked < 0.25)
        {
            return;
        }
        d.hotChecked = now;
        bool imagesOnly = false;
        if (depsChanged(d, imagesOnly))
        {
            hotReload(d, imagesOnly);
        }
    }

    // native.rml_hot([on]) -> on: whether documents reload when their files change (also by the file mods\.rml-hot or the environment variable GEAR_RML_HOT)
    int rmlHot(lua_State* L)
    {
        if (lua_isboolean(L, 1))
        {
            g_hotReload = lua_toboolean(L, 1) != 0;
        }
        lua_pushboolean(L, g_hotReload ? 1 : 0);
        return 1;
    }

    // native.rml_reloads(id) -> how many times the document was reloaded, last error | nil
    int rmlReloads(lua_State* L)
    {
        Doc& d = docArg(L);
        lua_pushnumber(L, d.reloads);
        if (d.hotError.empty())
        {
            lua_pushnil(L);
        }
        else
        {
            push(L, d.hotError);
        }
        return 2;
    }

    // native.rml_update(id, mouse_x, mouse_y, hover) -> {event, ...}: lays the document out and runs its animations; the
    // mouse in layout units, `hover` whether it is over this document. An event is {listener, type, target, current,
    // params = {name = text}} for the listeners registered with rml_listen.
    int rmlUpdate(lua_State* L)
    {
        Doc& d = docArg(L);
        const double mx = luaL_checknumber(L, 2), my = luaL_checknumber(L, 3);
        const bool hover = lua_toboolean(L, 4) != 0;
        RootScope scope(d);
        if (g_hotReload)
        {
            hotPoll(d);
        }
        fitContext(d);
        if (hover)
        {
            d.context->ProcessMouseMove(static_cast<int>(mx * d.unit - d.originPx.x), static_cast<int>(my * d.unit - d.originPx.y), 0);
            d.hovering = true;
        }
        else if (d.hovering)
        {
            d.context->ProcessMouseLeave();
            d.hovering = false;
        }
        ensureButtonPrompts(d);
        if (g_content)
        {
            g_content->apply(d.context);
        }
        d.context->Update();
        if (d.events.empty())
        {
            // The common case: nothing happened. One shared empty table, nothing allocated (callers only read it).
            static int emptyRef = LUA_NOREF;
            if (emptyRef == LUA_NOREF)
            {
                lua_newtable(L);
                emptyRef = luaL_ref(L, LUA_REGISTRYINDEX);
            }
            lua_rawgeti(L, LUA_REGISTRYINDEX, emptyRef);
            return 1;
        }
        lua_createtable(L, static_cast<int>(d.events.size()), 0);
        int index = 1;
        for (const QueuedEvent& e : d.events)
        {
            lua_createtable(L, 0, 5);
            lua_pushnumber(L, e.listener);
            lua_setfield(L, -2, "listener");
            push(L, e.type);
            lua_setfield(L, -2, "type");
            push(L, e.target);
            lua_setfield(L, -2, "target");
            push(L, e.current);
            lua_setfield(L, -2, "current");
            lua_createtable(L, 0, static_cast<int>(e.params.size()));
            for (const auto& [key, value] : e.params)
            {
                push(L, value);
                lua_setfield(L, -2, key.c_str());
            }
            lua_setfield(L, -2, "params");
            if (!e.call.empty())
            {
                push(L, e.call);
                lua_setfield(L, -2, "call");
                lua_createtable(L, static_cast<int>(e.args.size()), 0);
                for (size_t i = 0; i < e.args.size(); ++i)
                {
                    push(L, e.args[i]);
                    lua_rawseti(L, -2, static_cast<int>(i + 1));
                }
                lua_setfield(L, -2, "args");
            }
            lua_rawseti(L, -2, index++);
        }
        d.events.clear();
        return 1;
    }

    // native.rml_draw(id, layer, slot) -> commands recorded
    int rmlDraw(lua_State* L)
    {
        Doc& d = docArg(L);
        const int layer = static_cast<int>(luaL_checknumber(L, 2));
        const int slot = static_cast<int>(luaL_checknumber(L, 3));
        if (layer < 0 || layer >= static_cast<int>(Ui::Layer::Count) || slot < -1 || slot > 22)
        {
            throw Failure{"bad layer or window"};
        }
        if (!d.document->IsVisible())
        {
            lua_pushnumber(L, 0);
            return 1;
        }
        g_target = Target{};
        g_target.layer = static_cast<uint8_t>(layer);
        g_target.window = static_cast<int8_t>(slot);
        g_target.unit = d.unit;
        g_target.origin = d.originPx;
        RootScope scope(d);
        if (g_render)
        {
            g_render->sweepFileTextures();
        }
        d.context->Render();
        lua_pushnumber(L, g_target.count);
        return 1;
    }

    // Fills row `index` of the table on top of the stack with the element's box (the row table is reused when there is one, so a
    // caller that hands the same table in every frame allocates nothing).
    void pushRect(lua_State* L, Rml::Element* e, float unit, int index, bool panel = false)
    {
        const Rml::Vector2f o = absOffset(e, Rml::BoxArea::Border);
        const Rml::Vector2f s = e->GetBox().GetSize(Rml::BoxArea::Border);
        lua_rawgeti(L, -1, index);
        if (!lua_istable(L, -1))
        {
            lua_pop(L, 1);
            lua_createtable(L, 0, 6);
            lua_pushvalue(L, -1);
            lua_rawseti(L, -3, index);
        }
        lua_pushnumber(L, o.x / unit);
        lua_setfield(L, -2, "x");
        lua_pushnumber(L, o.y / unit);
        lua_setfield(L, -2, "y");
        lua_pushnumber(L, s.x / unit);
        lua_setfield(L, -2, "w");
        lua_pushnumber(L, s.y / unit);
        lua_setfield(L, -2, "h");
        if (panel)
        {
            // data-panel names a window the player may move (the layout editor); data-title is what the editor shows.
            push(L, e->GetAttribute<Rml::String>("data-panel", ""));
            lua_setfield(L, -2, "panel");
            push(L, e->GetAttribute<Rml::String>("data-title", ""));
            lua_setfield(L, -2, "title");
            // The game's own frame (gear.ui.native.frame) behind the window: data-frame="tooltip", or any element with class "window".
            push(L, e->GetAttribute<Rml::String>("data-frame", e->IsClassSet("thick-frame") ? "thick" : e->IsClassSet("window") ? "tooltip" : ""));
            lua_setfield(L, -2, "frame");
            // data-fill-inset="16" or "16 20.5 15 17.5": how far the thick frame's fill starts from the box edge (left top right bottom).
            // The classes bg-fill-settings (22 24.5 22 24.5: the fill reaches the thin brown line on every side) and bg-fill-dialog (16 20 15 18, the popup) set it too; the attribute wins.
            push(L, e->GetAttribute<Rml::String>("data-fill-inset", e->IsClassSet("bg-fill-dialog") ? "16 20 15 18" : e->IsClassSet("bg-fill-settings") ? "22 24.5 22 24.5" : ""));
            lua_setfield(L, -2, "fill_inset");
            // data-dividers="142 260": offsets from the window's top where the thick frame draws divider bars.
            push(L, e->GetAttribute<Rml::String>("data-dividers", ""));
            lua_setfield(L, -2, "dividers");
            // data-resizable: the layout editor sizes the window too (a corner grip); data-min-width / data-min-height (layout units, default 120 / 60).
            lua_pushboolean(L, e->HasAttribute("data-resizable") ? 1 : 0);
            lua_setfield(L, -2, "resizable");
            lua_pushnumber(L, e->GetAttribute<float>("data-min-width", 120.0f));
            lua_setfield(L, -2, "min_width");
            lua_pushnumber(L, e->GetAttribute<float>("data-min-height", 60.0f));
            lua_setfield(L, -2, "min_height");
        }
        lua_pop(L, 1);
    }

    // The table on top of the stack after `count` rows: the rows beyond it are dropped.
    void trimRows(lua_State* L, int count)
    {
        const int have = static_cast<int>(lua_objlen(L, -1));
        for (int i = count + 1; i <= have; ++i)
        {
            lua_pushnil(L);
            lua_rawseti(L, -2, i);
        }
    }

    // A table to fill: argument `arg` if it is one (reused), else a new one. Leaves it on top of the stack.
    void rowsTable(lua_State* L, int arg)
    {
        if (lua_istable(L, arg))
        {
            lua_pushvalue(L, arg);
        }
        else
        {
            lua_newtable(L);
        }
    }

    // native.rml_rects(id) -> {{x, y, w, h, panel, title}, ...}: the boxes of the document's top-level elements (the windows), in
    // layout units; what takes the mouse. `panel` is the element's data-panel attribute ('' if none): a window the layout
    // editor may move (see rml_move).
    int rmlRects(lua_State* L)
    {
        Doc& d = docArg(L);
        rowsTable(L, 2);
        int index = 1;
        if (d.document->IsVisible())
        {
            for (int i = 0; i < d.document->GetNumChildren(); ++i)
            {
                Rml::Element* c = d.document->GetChild(i);
                const Rml::Vector2f s = c->GetBox().GetSize(Rml::BoxArea::Border);
                if (c->IsVisible() && s.x > 0 && s.y > 0)
                {
                    pushRect(L, c, d.unit, index++, true);
                }
            }
        }
        trimRows(L, index - 1);
        return 1;
    }

    // native.rml_move(id, selector, x, y) -> found: puts the (absolutely positioned) element at x, y of the layout by its left and
    // top styles.
    int rmlMove(lua_State* L)
    {
        Doc& d = docArg(L);
        Rml::Element* e = find(d, str(L, 2));
        if (e)
        {
            const double x = luaL_checknumber(L, 3), y = luaL_checknumber(L, 4);
            e->SetProperty("left", std::to_string(static_cast<int>(std::lround(x * d.unit - d.bodyPx.x))) + "px");
            e->SetProperty("top", std::to_string(static_cast<int>(std::lround(y * d.unit - d.bodyPx.y))) + "px");
        }
        lua_pushboolean(L, e != nullptr);
        return 1;
    }

    // native.rml_resize(id, selector, w, h) -> found: gives the (absolutely positioned) element the border box w x h of the layout (a resizable
    // window kept by the layout editor).
    int rmlResize(lua_State* L)
    {
        Doc& d = docArg(L);
        Rml::Element* e = find(d, str(L, 2));
        if (e)
        {
            const double w = luaL_checknumber(L, 3), h = luaL_checknumber(L, 4);
            e->SetProperty("box-sizing", "border-box");
            e->SetProperty("width", std::to_string(static_cast<int>(std::lround(w * d.unit))) + "px");
            e->SetProperty("height", std::to_string(static_cast<int>(std::lround(h * d.unit))) + "px");
        }
        lua_pushboolean(L, e != nullptr);
        return 1;
    }

    // native.rml_nav(id) -> {{x, y, w, h}, ...}: the pressable elements (buttons, links, inputs, anything with data-nav),
    // in layout units; the index is what rml_click takes.
    int rmlNav(lua_State* L)
    {
        Doc& d = docArg(L);
        rowsTable(L, 2);
        int index = 1;
        for (Rml::Element* e : navElements(d))
        {
            pushRect(L, e, d.unit, index++);
            // A range input takes the real mouse (a press jumps to the value, a drag goes on) and the pad's D-pad (rml_nudge), see Doc:draw.
            lua_rawgeti(L, -1, index - 1);
            lua_pushboolean(L, isRange(e) ? 1 : 0);
            lua_setfield(L, -2, "range");
            // A text field takes the real mouse as well (caret, drag selection, double click on a word), see Doc:draw.
            lua_pushboolean(L, isTextField(e) ? 1 : 0);
            lua_setfield(L, -2, "text");
            // data-nav-home: where the pad's cursor starts when the document opens (the topmost savegame, not the search field).
            lua_pushboolean(L, e->HasAttribute("data-nav-home") ? 1 : 0);
            lua_setfield(L, -2, "home");
            lua_pop(L, 1);
        }
        trimRows(L, index - 1);
        return 1;
    }

    // Appends the scroll bars of an element that scrolls (overflow-y / overflow-x auto or scroll, with more content than room) to `out` as {x, y, w, h}
    // (layout units): a vertical one at the right edge of its padding box, a horizontal one at the bottom.
    void collectScrollbars(Rml::Element* e, std::vector<std::array<float, 4>>& out, float unit)
    {
        const Rml::Style::Overflow oy = e->GetComputedValues().overflow_y(), ox = e->GetComputedValues().overflow_x();
        const bool vertical = (oy == Rml::Style::Overflow::Auto || oy == Rml::Style::Overflow::Scroll) && e->GetScrollHeight() > e->GetClientHeight() + 0.5f;
        const bool horizontal = (ox == Rml::Style::Overflow::Auto || ox == Rml::Style::Overflow::Scroll) && e->GetScrollWidth() > e->GetClientWidth() + 0.5f;
        if ((vertical || horizontal) && e->IsVisible(true) && !scrolledOut(e))
        {
            const Rml::Vector2f o = absOffset(e, Rml::BoxArea::Padding);
            const Rml::Vector2f s = e->GetBox().GetSize(Rml::BoxArea::Padding);
            const float bar = s.x - e->GetClientWidth();         // the room of a vertical bar
            const float barH = s.y - e->GetClientHeight();       // of a horizontal one
            if (vertical && bar > 1.0f)
            {
                out.push_back({(o.x + s.x - bar) / unit, o.y / unit, bar / unit, (s.y - (horizontal ? barH : 0.0f)) / unit});
            }
            if (horizontal && barH > 1.0f)
            {
                out.push_back({o.x / unit, (o.y + s.y - barH) / unit, (s.x - (vertical ? bar : 0.0f)) / unit, barH / unit});
            }
        }
        for (int i = 0; i < e->GetNumChildren(); ++i)
        {
            collectScrollbars(e->GetChild(i), out, unit);
        }
    }

    // native.rml_scrollbars(id) -> {{x, y, w, h}, ...}: the scroll bars of every box of the document that scrolls: Lua registers a region over each, so a press
    // on one (the knob, the groove, the arrows) reaches RmlUi as the mouse button (rml_mouse) and a drag goes on.
    int rmlScrollbars(lua_State* L)
    {
        Doc& d = docArg(L);
        rowsTable(L, 2);
        std::vector<std::array<float, 4>> bars;
        if (d.document->IsVisible())
        {
            collectScrollbars(d.document, bars, d.unit);
        }
        int index = 1;
        for (const auto& b : bars)
        {
            lua_rawgeti(L, -1, index);
            if (!lua_istable(L, -1))
            {
                lua_pop(L, 1);
                lua_createtable(L, 0, 4);
                lua_pushvalue(L, -1);
                lua_rawseti(L, -3, index);
            }
            lua_pushnumber(L, b[0]);
            lua_setfield(L, -2, "x");
            lua_pushnumber(L, b[1]);
            lua_setfield(L, -2, "y");
            lua_pushnumber(L, b[2]);
            lua_setfield(L, -2, "w");
            lua_pushnumber(L, b[3]);
            lua_setfield(L, -2, "h");
            lua_pop(L, 1);
            ++index;
        }
        trimRows(L, index - 1);
        return 1;
    }

    // native.rml_scrolls(id) -> {{x, y, w, h}, ...}: the visible boxes marked data-nav-scroll (a list the pad scrolls card by card): the pad's
    // scroll areas.
    int rmlScrolls(lua_State* L)
    {
        Doc& d = docArg(L);
        rowsTable(L, 2);
        int index = 1;
        if (d.document->IsVisible())
        {
            std::vector<Rml::Element*> areas;
            collectNavScrollAreas(d.document, areas);
            for (Rml::Element* e : areas)
            {
                const Rml::Vector2f s = e->GetBox().GetSize(Rml::BoxArea::Border);
                if (s.x > 0 && s.y > 0 && !scrolledOut(e))
                {
                    pushRect(L, e, d.unit, index);
                    // Scrolled to its top / bottom: the pad leaves the list there instead of scrolling it.
                    const float top = e->GetScrollTop();
                    const float most = std::max(0.0f, e->GetScrollHeight() - e->GetClientHeight());
                    lua_rawgeti(L, -1, index);
                    lua_pushboolean(L, top <= 0.5f ? 1 : 0);
                    lua_setfield(L, -2, "at_top");
                    lua_pushboolean(L, top >= most - 0.5f ? 1 : 0);
                    lua_setfield(L, -2, "at_bottom");
                    lua_pop(L, 1);
                    ++index;
                }
            }
        }
        trimRows(L, index - 1);
        return 1;
    }

    // native.rml_pad_scroll(id, x, y, dir) -> scrolled: the pad's up (-1) / down (+1) with its cursor at x, y (layout units) on a card of a list marked
    // data-nav-scroll, and no card there to step to in view: the list scrolls by the distance to the next card, so that the cursor (which stays where it is) is
    // over that card now.
    int rmlPadScroll(lua_State* L)
    {
        Doc& d = docArg(L);
        const float px = static_cast<float>(luaL_checknumber(L, 2)) * d.unit, py = static_cast<float>(luaL_checknumber(L, 3)) * d.unit;
        const int dir = luaL_checknumber(L, 4) > 0 ? 1 : -1;
        RootScope scope(d);
        if (!d.document->IsVisible())
        {
            lua_pushboolean(L, 0);
            return 1;
        }
        Rml::ElementList all;
        d.document->QuerySelectorAll(all, kNavSelector);
        const auto centre = [](Rml::Element* e) {
            const Rml::Vector2f o = absOffset(e, Rml::BoxArea::Border);
            return o.y + e->GetBox().GetSize(Rml::BoxArea::Border).y * 0.5f;
        };
        Rml::Element* current = nullptr;
        Rml::Element* area = nullptr;
        for (Rml::Element* e : all)
        {
            if (!e->IsVisible(true) || scrolledOut(e))
            {
                continue;
            }
            const Rml::Vector2f o = absOffset(e, Rml::BoxArea::Border);
            const Rml::Vector2f s = e->GetBox().GetSize(Rml::BoxArea::Border);
            if (px >= o.x && px <= o.x + s.x && py >= o.y && py <= o.y + s.y && navScrollArea(e))
            {
                current = e;
                area = navScrollArea(e);
                break;
            }
        }
        if (!current)
        {
            lua_pushboolean(L, 0);
            return 1;
        }
        const float here = centre(current);
        Rml::Element* best = nullptr;
        float bestDelta = 1e9f;
        for (Rml::Element* e : all)
        {
            if (e == current || !e->IsVisible(true) || e->GetAttribute("disabled") != nullptr || navScrollArea(e) != area)
            {
                continue;
            }
            const float delta = (centre(e) - here) * static_cast<float>(dir);
            if (delta > 1.0f && delta < bestDelta)
            {
                bestDelta = delta;
                best = e;
            }
        }
        if (!best)
        {
            lua_pushboolean(L, 0);
            return 1;
        }
        const float top = area->GetScrollTop();
        const float most = std::max(0.0f, area->GetScrollHeight() - area->GetClientHeight());
        area->SetScrollTop(std::max(0.0f, std::min(most, top + (centre(best) - here))));
        lua_pushboolean(L, 1);
        return 1;
    }

    // native.rml_pad_reveal(id, x, y) -> scrolled: the pad's cursor (x, y, layout units) is on a card of a list marked data-nav-scroll that is only partly in
    // view (the pad moved it to the next card, whose centre is in view): the list scrolls by the hidden part, so that the whole card is in view. The card is
    // under the cursor still (less than half of it was hidden).
    int rmlPadReveal(lua_State* L)
    {
        Doc& d = docArg(L);
        const float px = static_cast<float>(luaL_checknumber(L, 2)) * d.unit, py = static_cast<float>(luaL_checknumber(L, 3)) * d.unit;
        RootScope scope(d);
        lua_pushboolean(L, 0);
        if (!d.document->IsVisible())
        {
            return 1;
        }
        Rml::ElementList all;
        d.document->QuerySelectorAll(all, kNavSelector);
        for (Rml::Element* e : all)
        {
            Rml::Element* area = navScrollArea(e);
            if (!area || !e->IsVisible(true))
            {
                continue;
            }
            const Rml::Vector2f o = absOffset(e, Rml::BoxArea::Border);
            const Rml::Vector2f s = e->GetBox().GetSize(Rml::BoxArea::Border);
            if (px < o.x || px > o.x + s.x || py < o.y || py > o.y + s.y)
            {
                continue;
            }
            const Rml::Vector2f ao = absOffset(area, Rml::BoxArea::Padding);
            const Rml::Vector2f as = area->GetBox().GetSize(Rml::BoxArea::Padding);
            float move = 0.0f;
            if (o.y < ao.y)
            {
                move = o.y - ao.y;
            }
            else if (o.y + s.y > ao.y + as.y)
            {
                move = o.y + s.y - (ao.y + as.y);
            }
            if (std::fabs(move) < 0.5f || std::fabs(move) > s.y * 0.6f)
            {
                return 1;
            }
            const float top = area->GetScrollTop();
            const float most = std::max(0.0f, area->GetScrollHeight() - area->GetClientHeight());
            area->SetScrollTop(std::max(0.0f, std::min(most, top + move)));
            lua_pop(L, 1);
            lua_pushboolean(L, 1);
            return 1;
        }
        return 1;
    }

    // native.rml_mouse(id, "down" | "up" [, x, y [, modifiers]]): the left button for the mouse position the last update gave (a press on a range input, which
    // the click at the element's centre of rml_click would not serve).
    int rmlMouse(lua_State* L)
    {
        Doc& d = docArg(L);
        const std::string kind = str(L, 2);
        RootScope scope(d);
        if (lua_isnumber(L, 3) && lua_isnumber(L, 4))
        {
            d.context->ProcessMouseMove(static_cast<int>(lua_tonumber(L, 3) * d.unit - d.originPx.x), static_cast<int>(lua_tonumber(L, 4) * d.unit - d.originPx.y), 0);
            d.hovering = true;
        }
        const int mods = modifiersArg(L, 5);
        if (kind == "down")
        {
            d.context->ProcessMouseButtonDown(0, mods);
        }
        else
        {
            d.context->ProcessMouseButtonUp(0, mods);
        }
        return 0;
    }

    // native.rml_nudge(id, index, steps): the D-pad on the index-th element of rml_nav, a range input: its value moves by `steps` jumps of
    // about a fiftieth of its range (at least one step), as the arrow keys of the slider would.
    int rmlNudge(lua_State* L)
    {
        Doc& d = docArg(L);
        const size_t index = static_cast<size_t>(luaL_checknumber(L, 2));
        const int steps = static_cast<int>(luaL_checknumber(L, 3));
        RootScope scope(d);
        const std::vector<Rml::Element*> list = navElements(d);
        if (index < 1 || index > list.size() || !isRange(list[index - 1]) || steps == 0)
        {
            return 0;
        }
        Rml::Element* e = list[index - 1];
        const float lo = e->GetAttribute<float>("min", 0.0f), hi = e->GetAttribute<float>("max", 100.0f);
        const float step = std::max(e->GetAttribute<float>("step", 1.0f), 1e-6f);
        const int jump = std::max(1, static_cast<int>(std::lround((hi - lo) / 50.0f / step)));
        e->Focus();
        const Rml::Input::KeyIdentifier key = steps > 0 ? Rml::Input::KI_RIGHT : Rml::Input::KI_LEFT;
        for (int i = 0; i < std::abs(steps) * jump; ++i)
        {
            d.context->ProcessKeyDown(key, 0);
            d.context->ProcessKeyUp(key, 0);
        }
        return 0;
    }

    // native.rml_click(id, index): a left click on the index-th element of rml_nav (the mouse, or the pad's A).
    int rmlClick(lua_State* L)
    {
        Doc& d = docArg(L);
        const size_t index = static_cast<size_t>(luaL_checknumber(L, 2));
        RootScope scope(d);
        const std::vector<Rml::Element*> list = navElements(d);
        if (index < 1 || index > list.size())
        {
            return 0;
        }
        Rml::Element* e = list[index - 1];
        const Rml::Vector2f o = e->GetAbsoluteOffset(Rml::BoxArea::Border);
        const Rml::Vector2f s = e->GetBox().GetSize(Rml::BoxArea::Border);
        d.context->ProcessMouseMove(static_cast<int>(o.x + s.x * 0.5f), static_cast<int>(o.y + s.y * 0.5f), 0);
        d.context->ProcessMouseButtonDown(0, 0);
        d.context->ProcessMouseButtonUp(0, 0);
        return 0;
    }

    // native.rml_html(id, selector [, rml]) -> the inner markup of the first match (set it with a second string)
    int rmlHtml(lua_State* L)
    {
        Doc& d = docArg(L);
        Rml::Element* e = find(d, str(L, 2));
        if (!e)
        {
            lua_pushnil(L);
            return 1;
        }
        if (lua_isstring(L, 3))
        {
            RootScope scope(d);
            e->SetInnerRML(str(L, 3));
        }
        push(L, e->GetInnerRML());
        return 1;
    }

    // native.rml_attr(id, selector, name [, value | false]) -> the attribute's text | nil (a value sets it, false removes it)
    int rmlAttr(lua_State* L)
    {
        Doc& d = docArg(L);
        Rml::Element* e = find(d, str(L, 2));
        const std::string name = str(L, 3);
        if (!e)
        {
            lua_pushnil(L);
            return 1;
        }
        if (lua_type(L, 4) == LUA_TBOOLEAN && !lua_toboolean(L, 4))
        {
            e->RemoveAttribute(name);
        }
        else if (lua_type(L, 4) == LUA_TBOOLEAN)
        {
            e->SetAttribute(name, Rml::String());      // a flag attribute (checked, disabled)
        }
        else if (lua_isstring(L, 4))
        {
            e->SetAttribute(name, str(L, 4));
        }
        const Rml::Variant* v = e->GetAttribute(name);
        Rml::String text;
        if (v && v->GetInto(text))
        {
            push(L, text);
        }
        else
        {
            lua_pushnil(L);
        }
        return 1;
    }

    // native.rml_class(id, selector, class, on) -> found
    int rmlClass(lua_State* L)
    {
        Doc& d = docArg(L);
        Rml::Element* e = find(d, str(L, 2));
        if (e)
        {
            e->SetClass(str(L, 3), lua_toboolean(L, 4) != 0);
        }
        lua_pushboolean(L, e != nullptr);
        return 1;
    }

    // native.rml_style(id, selector, property, value | false) -> found: sets an inline style, false takes it off
    int rmlStyle(lua_State* L)
    {
        Doc& d = docArg(L);
        Rml::Element* e = find(d, str(L, 2));
        if (e)
        {
            const std::string property = str(L, 3);
            if (lua_type(L, 4) == LUA_TBOOLEAN || lua_isnil(L, 4))
            {
                e->RemoveProperty(property);
            }
            else
            {
                e->SetProperty(property, str(L, 4));
            }
        }
        lua_pushboolean(L, e != nullptr);
        return 1;
    }

    // native.rml_value(id, selector [, text]) -> the value of the form control | nil (a string sets it)
    int rmlValue(lua_State* L)
    {
        Doc& d = docArg(L);
        auto* e = dynamic_cast<Rml::ElementFormControl*>(find(d, str(L, 2)));
        if (!e)
        {
            lua_pushnil(L);
            return 1;
        }
        if (lua_isstring(L, 3))
        {
            e->SetValue(str(L, 3));
        }
        push(L, e->GetValue());
        return 1;
    }

    // native.rml_query(id, selector [, all]) -> handle | nil, or with `all` a list of handles (at most 500)
    int rmlQuery(lua_State* L)
    {
        Doc& d = docArg(L);
        const std::string selector = str(L, 2);
        if (lua_toboolean(L, 3))
        {
            Rml::ElementList list;
            d.document->QuerySelectorAll(list, selector);
            lua_createtable(L, static_cast<int>(list.size()), 0);
            int i = 0;
            for (Rml::Element* e : list)
            {
                if (++i > 500)
                {
                    break;
                }
                lua_pushnumber(L, handleOf(d, e));
                lua_rawseti(L, -2, i);
            }
            return 1;
        }
        Rml::Element* e = find(d, selector);
        if (e)
        {
            lua_pushnumber(L, handleOf(d, e));
        }
        else
        {
            lua_pushnil(L);
        }
        return 1;
    }

    // native.rml_append(id, parent selector, tag [, {attribute = text}]) -> handle | nil: a new empty element as the last child
    int rmlAppend(lua_State* L)
    {
        Doc& d = docArg(L);
        Rml::Element* parent = find(d, str(L, 2));
        if (!parent || !d.document || parent->GetNumChildren(true) >= 2000)
        {
            lua_pushnil(L);
            return 1;
        }
        RootScope scope(d);
        Rml::ElementPtr child = d.document->CreateElement(str(L, 3));
        if (!child)
        {
            lua_pushnil(L);
            return 1;
        }
        if (lua_istable(L, 4))
        {
            lua_pushnil(L);
            while (lua_next(L, 4))
            {
                if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) != LUA_TTABLE)
                {
                    child->SetAttribute(lua_tostring(L, -2), lua_isstring(L, -1) ? std::string(lua_tostring(L, -1)) : std::string());
                }
                lua_pop(L, 1);
            }
        }
        Rml::Element* raw = parent->AppendChild(std::move(child));
        lua_pushnumber(L, raw ? handleOf(d, raw) : 0);
        return 1;
    }

    // native.rml_remove(id, selector) -> whether an element was removed (never the document itself)
    int rmlRemove(lua_State* L)
    {
        Doc& d = docArg(L);
        Rml::Element* e = find(d, str(L, 2));
        Rml::Element* parent = e ? e->GetParentNode() : nullptr;
        if (parent && e != d.document)
        {
            parent->RemoveChild(e);
            lua_pushboolean(L, 1);
        }
        else
        {
            lua_pushboolean(L, 0);
        }
        return 1;
    }

    // native.rml_relatives(id, selector, "parent" | "children") -> handle | nil, or a list of handles
    int rmlRelatives(lua_State* L)
    {
        Doc& d = docArg(L);
        Rml::Element* e = find(d, str(L, 2));
        const std::string what = str(L, 3);
        if (what == "parent")
        {
            Rml::Element* p = e ? e->GetParentNode() : nullptr;
            if (p && p != d.document)
            {
                lua_pushnumber(L, handleOf(d, p));
            }
            else
            {
                lua_pushnil(L);
            }
            return 1;
        }
        lua_createtable(L, 0, 0);
        if (e)
        {
            const int n = std::min(e->GetNumChildren(), 500);
            for (int i = 0; i < n; ++i)
            {
                lua_pushnumber(L, handleOf(d, e->GetChild(i)));
                lua_rawseti(L, -2, i + 1);
            }
        }
        return 1;
    }

    // native.rml_rect(id, selector) -> {x, y, w, h} in layout units | nil
    int rmlRect(lua_State* L)
    {
        Doc& d = docArg(L);
        Rml::Element* e = find(d, str(L, 2));
        if (!e)
        {
            lua_pushnil(L);
            return 1;
        }
        lua_createtable(L, 1, 0);
        pushRect(L, e, d.unit, 1);
        lua_rawgeti(L, -1, 1);
        lua_remove(L, -2);
        return 1;
    }

    // native.rml_scroll(id, selector [, top]) -> {top, height, client} | nil: the scroll position of the element (an overflow: auto / scroll
    // box) in layout units, after setting it to `top` when given (clamped by RmlUi to the content); `height` is the content's height,
    // `client` the visible height.
    int rmlScroll(lua_State* L)
    {
        Doc& d = docArg(L);
        Rml::Element* e = find(d, str(L, 2));
        if (!e)
        {
            lua_pushnil(L);
            return 1;
        }
        const float unit = d.unit > 0 ? d.unit : 1.0f;
        if (lua_isnumber(L, 3))
        {
            e->SetScrollTop(static_cast<float>(lua_tonumber(L, 3)) * unit);
        }
        lua_createtable(L, 0, 3);
        lua_pushnumber(L, e->GetScrollTop() / unit);
        lua_setfield(L, -2, "top");
        lua_pushnumber(L, e->GetScrollHeight() / unit);
        lua_setfield(L, -2, "height");
        lua_pushnumber(L, e->GetClientHeight() / unit);
        lua_setfield(L, -2, "client");
        return 1;
    }

    // native.rml_focus(id, selector [, on]) -> found, focused: gives the element the focus (a text field then takes the keys), or takes
    // it away with on == false.
    int rmlFocus(lua_State* L)
    {
        Doc& d = docArg(L);
        Rml::Element* e = find(d, str(L, 2));
        if (e)
        {
            if (lua_isnone(L, 3) || lua_toboolean(L, 3))
            {
                e->Focus();
            }
            else
            {
                e->Blur();
            }
        }
        lua_pushboolean(L, e != nullptr);
        lua_pushboolean(L, e && e->GetOwnerDocument() && e->GetOwnerDocument()->GetContext() &&
                               e->GetOwnerDocument()->GetContext()->GetFocusElement() == e);
        return 2;
    }

    // native.rml_blur(id): the document's focus goes away (the focused element, if any, is blurred): for a document that is not drawn any more
    int rmlBlur(lua_State* L)
    {
        Doc& d = docArg(L);
        if (Rml::Element* f = d.context->GetFocusElement())
        {
            RootScope scope(d);
            f->Blur();
        }
        return 0;
    }

    // native.rml_tag(id, selector) -> tag name | nil
    int rmlTag(lua_State* L)
    {
        Doc& d = docArg(L);
        Rml::Element* e = find(d, str(L, 2));
        if (e)
        {
            push(L, e->GetTagName());
        }
        else
        {
            lua_pushnil(L);
        }
        return 1;
    }

    // native.rml_listen(id, selector, event) -> listener id, matching elements now | nil: events of that type on the matching
    // elements (also ones added later) come back from rml_update with the listener id.
    int rmlListen(lua_State* L)
    {
        Doc& d = docArg(L);
        const std::string selector = str(L, 2);
        const std::string event = str(L, 3);
        if (d.listeners.size() >= 1024)
        {
            lua_pushnil(L);
            return 1;
        }
        const int id = static_cast<int>(d.listeners.size()) + 1;
        if (selector.empty())
        {
            d.listeners.push_back(std::make_unique<Listener>(&d, id));
            d.listenerEvents.push_back(event);
            d.document->AddEventListener(event, d.listeners.back().get());
            lua_pushnumber(L, id);
            lua_pushnumber(L, 1);
            return 2;
        }
        Rml::ElementList list;
        d.document->QuerySelectorAll(list, selector);
        d.listeners.push_back(std::make_unique<Listener>(&d, id, selector));
        d.listenerEvents.push_back(event);
        d.document->AddEventListener(event, d.listeners.back().get(), true);
        lua_pushnumber(L, id);
        lua_pushnumber(L, static_cast<lua_Number>(list.size()));
        return 2;
    }

    // native.rml_wants_text(id) -> whether a text field has the focus
    int rmlWantsText(lua_State* L)
    {
        Doc& d = docArg(L);
        const bool wants = isTextField(d.context->GetFocusElement());
        lua_pushboolean(L, wants);
        return 1;
    }

    // native.rml_text(id, utf8): characters typed into the focused field
    int rmlText(lua_State* L)
    {
        Doc& d = docArg(L);
        d.context->ProcessTextInput(str(L, 2));
        return 0;
    }

    Rml::Input::KeyIdentifier keyFor(int vk)
    {
        using K = Rml::Input::KeyIdentifier;
        switch (vk)
        {
        case 0x08: return K::KI_BACK;
        case 0x09: return K::KI_TAB;
        case 0x0D: return K::KI_RETURN;
        case 0x1B: return K::KI_ESCAPE;
        case 0x21: return K::KI_PRIOR;
        case 0x22: return K::KI_NEXT;
        case 0x23: return K::KI_END;
        case 0x24: return K::KI_HOME;
        case 0x25: return K::KI_LEFT;
        case 0x26: return K::KI_UP;
        case 0x27: return K::KI_RIGHT;
        case 0x28: return K::KI_DOWN;
        case 0x2E: return K::KI_DELETE;
        // the clipboard and select-all chords (Lua passes them with Ctrl held)
        case 0x41: return K::KI_A;
        case 0x43: return K::KI_C;
        case 0x56: return K::KI_V;
        case 0x58: return K::KI_X;
        default: return K::KI_UNKNOWN;
        }
    }

    // native.rml_key(id, vk, down [, modifiers]) -> whether the key is one RmlUi takes (Backspace, Delete, Enter, Tab, Escape, arrows,
    // Home, End, Page Up / Down, A / C / V / X); modifiers: 1 Ctrl, 2 Shift, 4 Alt (Shift + arrows select, Ctrl + A / C / X / V)
    int rmlKey(lua_State* L)
    {
        Doc& d = docArg(L);
        const Rml::Input::KeyIdentifier key = keyFor(static_cast<int>(luaL_checknumber(L, 2)));
        if (key == Rml::Input::KI_UNKNOWN)
        {
            lua_pushboolean(L, 0);
            return 1;
        }
        const int mods = modifiersArg(L, 4);
        if (lua_toboolean(L, 3))
        {
            d.context->ProcessKeyDown(key, mods);
        }
        else
        {
            d.context->ProcessKeyUp(key, mods);
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    // native.rml_wheel(id, notches): the mouse wheel over the document (+ up, as the game reports it)
    int rmlWheel(lua_State* L)
    {
        Doc& d = docArg(L);
        d.context->ProcessMouseWheel(Rml::Vector2f(0.0f, -static_cast<float>(luaL_checknumber(L, 2))), 0);
        return 0;
    }

    // native.rml_fonts() -> {default = {family, real, file}, game = {...}, start = {...}}: the families registered for the theme and whether
    // each comes from its real file or is the fallback.
    int rmlFonts(lua_State* L)
    {
        if (!ensureStarted())
        {
            lua_pushnil(L);
            return 1;
        }
        lua_createtable(L, 0, 3);
        const auto put = [&](const char* key, const FontReport& f) {
            lua_createtable(L, 0, 3);
            push(L, f.family);
            lua_setfield(L, -2, "family");
            lua_pushboolean(L, f.real ? 1 : 0);
            lua_setfield(L, -2, "real");
            push(L, f.file);
            lua_setfield(L, -2, "file");
            lua_setfield(L, -2, key);
        };
        put("default", g_fontDefault);
        put("game", g_fontGame);
        put("start", g_fontStart);
        return 1;
    }

    // native.rml_exports(id, dir, patterns | nil): the files of the mod (folder `dir`) that other mods' documents reach as @id/path while it runs
    int rmlExports(lua_State* L)
    {
        const std::string id = str(L, 1);
        if (id.empty())
        {
            return 0;
        }
        if (!lua_istable(L, 3))
        {
            g_exports.erase(id);
            return 0;
        }
        Export e;
        e.dir = str(L, 2);
        const int count = static_cast<int>(lua_objlen(L, 3));
        for (int i = 1; i <= count && i <= 64; ++i)
        {
            lua_rawgeti(L, 3, i);
            if (const char* t = lua_tostring(L, -1))
            {
                e.patterns.emplace_back(t);
            }
            lua_pop(L, 1);
        }
        g_exports[id] = std::move(e);
        return 0;
    }

    // native.vfs_configure(dir, layers | nil): the layers of the mod's packs from mod.lua ({file, mount} in order, a later one wins); nil: the folder's *.gpak.
    int vfsConfigure(lua_State* L)
    {
        const fs::path root = modRoot(str(L, 1));
        if (root.empty())
        {
            return 0;
        }
        if (!lua_istable(L, 2))
        {
            Gear::Vfs::unmount(root);
            return 0;
        }
        std::vector<Gear::Vfs::Layer> layers;
        const int count = static_cast<int>(lua_objlen(L, 2));
        for (int i = 1; i <= count && i <= 32; ++i)
        {
            lua_rawgeti(L, 2, i);
            lua_getfield(L, -1, "file");
            lua_getfield(L, -2, "mount");
            const char* file = lua_tostring(L, -2);
            const char* mount = lua_tostring(L, -1);
            if (file)
            {
                const fs::path path = root / fromUtf8(file);
                if (inside(root, path))
                {
                    layers.push_back({path, mount ? mount : "/"});
                }
                else
                {
                    LOG("vfs: {}: the pack {} is not inside the mod's folder", str(L, 1), file);
                }
            }
            lua_pop(L, 3);
        }
        Gear::Vfs::configure(root, std::move(layers));
        return 0;
    }

    // native.vfs_unmount(dir): the mod stopped.
    int vfsUnmount(lua_State* L)
    {
        const fs::path root = modRoot(str(L, 1));
        if (!root.empty())
        {
            Gear::Vfs::unmount(root);
        }
        return 0;
    }

    // native.vfs_read(dir, path) -> string | nil: a file of the mod's folder, from the disk or else from its layers.
    int vfsRead(lua_State* L)
    {
        const fs::path root = modRoot(str(L, 1));
        const std::string rel = str(L, 2);
        const fs::path file = root.empty() || rel.empty() ? fs::path() : root / fromUtf8(rel);
        std::string bytes;
        if (!file.empty() && inside(root, file) && readAll(file, bytes, true))
        {
            lua_pushlstring(L, bytes.data(), bytes.size());
            return 1;
        }
        lua_pushnil(L);
        return 1;
    }

    // native.vfs_exists(dir, path) -> bool
    int vfsExists(lua_State* L)
    {
        const fs::path root = modRoot(str(L, 1));
        const std::string rel = str(L, 2);
        const fs::path file = root.empty() || rel.empty() ? fs::path() : root / fromUtf8(rel);
        std::error_code ec;
        lua_pushboolean(L, !file.empty() && inside(root, file) && (fs::is_regular_file(file, ec) || Gear::Vfs::contains(root, file)));
        return 1;
    }

    // native.vfs_list(dir, prefix) -> {path, ...}: the files below `prefix` that the layers provide (not the loose files on disk), in order.
    int vfsList(lua_State* L)
    {
        const fs::path root = modRoot(str(L, 1));
        lua_newtable(L);
        if (root.empty())
        {
            return 1;
        }
        int i = 0;
        for (const std::string& path : Gear::Vfs::list(root, str(L, 2)))
        {
            lua_pushlstring(L, path.data(), path.size());
            lua_rawseti(L, -2, ++i);
            if (i >= 100000)
            {
                break;
            }
        }
        return 1;
    }

    // native.texture_picture(id) -> the picture id (for gear.ui.image, Creature:set_texture) of a shared texture | nil
    int texturePicture(lua_State* L)
    {
        const auto it = g_shared.find(static_cast<uint32_t>(luaL_checkinteger(L, 1)));
        if (it == g_shared.end())
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushinteger(L, it->second.picture);
        return 1;
    }

    // native.texture_load(dir, path) -> id, width, height | nil, message: an image file of the mod (or "@mod/path") as a shared texture, one more reference.
    int textureLoad(lua_State* L)
    {
        const std::string dir = str(L, 1), rel = str(L, 2);
        fs::path file;
        bool trusted = false;
        if (!rel.empty() && rel[0] == '@')
        {
            file = resolveShared(rel, &trusted);
        }
        else
        {
            const fs::path root = modRoot(dir);
            file = root.empty() || rel.empty() ? fs::path() : root / fromUtf8(rel);
            if (!file.empty() && !inside(root, file))
            {
                file.clear();
            }
        }
        if (file.empty())
        {
            lua_pushnil(L);
            lua_pushstring(L, "no such image in the mod's folder");
            return 2;
        }
        std::error_code ec;
        const std::string key = lowered(toUtf8(fs::weakly_canonical(file, ec)));
        const auto known = g_sharedByKey.find(key);
        if (known != g_sharedByKey.end())
        {
            SharedTexture& t = g_shared[known->second];
            ++t.modRefs;
            lua_pushinteger(L, known->second);
            lua_pushinteger(L, t.width);
            lua_pushinteger(L, t.height);
            return 3;
        }
        std::string bytes;
        if (!readAll(file, bytes, true))
        {
            lua_pushnil(L);
            lua_pushstring(L, "cannot read the image");
            return 2;
        }
        SharedTexture t;
        std::string error;
        t.picture = pictureFromImageBytes(bytes, t.width, t.height, error);
        if (!t.picture)
        {
            lua_pushnil(L);
            lua_pushstring(L, error.c_str());
            return 2;
        }
        t.modRefs = 1;
        t.key = key;
        const uint32_t id = ++g_nextShared;
        g_shared.emplace(id, t);
        g_sharedByKey.emplace(key, id);
        lua_pushinteger(L, id);
        lua_pushinteger(L, t.width);
        lua_pushinteger(L, t.height);
        return 3;
    }

    // native.texture_release(id): one reference less; the last one purges the source from RmlUi's cache and frees the picture.
    int textureRelease(lua_State* L)
    {
        const uint32_t id = static_cast<uint32_t>(luaL_checkinteger(L, 1));
        const auto it = g_shared.find(id);
        if (it == g_shared.end() || it->second.modRefs <= 0)
        {
            return 0;
        }
        if (--it->second.modRefs == 0)
        {
            Rml::ReleaseTexture("gear:tex/" + std::to_string(id));      // RmlUi's own reference (its release callback drops rmlRefs)
            destroyIfUnused(id);
        }
        return 0;
    }

    // native.rml_font(dir, path) -> ok: a font file of the mod (TTF / OTF) for the documents' font-family
    int rmlFont(lua_State* L)
    {
        const std::string dir = str(L, 1);
        const fs::path root = modRoot(dir);
        if (root.empty() || !ensureStarted())
        {
            lua_pushboolean(L, 0);
            return 1;
        }
        fs::path file = root / fromUtf8(str(L, 2));
        bool ok = false;
        bool sharedFont = false;
        if (str(L, 2).compare(0, 1, "@") == 0)
        {
            file = resolveShared(str(L, 2), &sharedFont);
        }
        if (sharedFont ? !file.empty() : inside(root, file))
        {
            RootScope scope(root, sharedFont ? file.parent_path() : fs::path());     // a shared font is readable from its own folder while it loads
            ok = Rml::LoadFontFace(toUtf8(file), lua_toboolean(L, 3) != 0);
        }
        lua_pushboolean(L, ok);
        return 1;
    }
}

void RmlHost::configure(const std::wstring& modsDir)
{
    g_modsDir = modsDir;
    {
        // Hot reload of documents: a developer's switch (an empty file mods\.rml-hot, or the environment variable GEAR_RML_HOT); players never have it.
        std::error_code ec;
        g_hotReload = fs::exists(fs::path(modsDir) / L".rml-hot", ec) || (_wgetenv(L"GEAR_RML_HOT") != nullptr);
    }
}

void RmlHost::registerNatives(lua_State* L)
{
    static const luaL_Reg kNatives[] = {
        {"rml_load", guarded<rmlLoad>},
        {"rml_load_string", guarded<rmlLoadString>},
        {"rml_origin", guarded<rmlOrigin>},
        {"rml_close", guarded<rmlClose>},
        {"rml_show", guarded<rmlShow>},
        {"rml_update", guarded<rmlUpdate>},
        {"rml_hot", guarded<rmlHot>},
        {"rml_reloads", guarded<rmlReloads>},
        {"rml_draw", guarded<rmlDraw>},
        {"rml_rects", guarded<rmlRects>},
        {"rml_move", guarded<rmlMove>},
        {"rml_resize", guarded<rmlResize>},
        {"rml_query", guarded<rmlQuery>},
        {"rml_append", guarded<rmlAppend>},
        {"rml_remove", guarded<rmlRemove>},
        {"rml_relatives", guarded<rmlRelatives>},
        {"rml_rect", guarded<rmlRect>},
        {"rml_tag", guarded<rmlTag>},
        {"rml_focus", guarded<rmlFocus>},
        {"rml_blur", guarded<rmlBlur>},
        {"rml_scroll", guarded<rmlScroll>},
        {"rml_model_set", guarded<rmlModelSet>},
        {"rml_model_changed", guarded<rmlModelChanged>},
        {"rml_nav", guarded<rmlNav>},
        {"rml_scrolls", guarded<rmlScrolls>},
        {"rml_scrollbars", guarded<rmlScrollbars>},
        {"rml_pad_scroll", guarded<rmlPadScroll>},
        {"rml_pad_reveal", guarded<rmlPadReveal>},
        {"rml_click", guarded<rmlClick>},
        {"rml_mouse", guarded<rmlMouse>},
        {"rml_nudge", guarded<rmlNudge>},
        {"rml_html", guarded<rmlHtml>},
        {"rml_attr", guarded<rmlAttr>},
        {"rml_class", guarded<rmlClass>},
        {"rml_style", guarded<rmlStyle>},
        {"rml_value", guarded<rmlValue>},
        {"rml_listen", guarded<rmlListen>},
        {"rml_wants_text", guarded<rmlWantsText>},
        {"rml_text", guarded<rmlText>},
        {"rml_key", guarded<rmlKey>},
        {"rml_wheel", guarded<rmlWheel>},
        {"rml_font", guarded<rmlFont>},
        {"vfs_configure", guarded<vfsConfigure>},
        {"vfs_unmount", guarded<vfsUnmount>},
        {"vfs_read", guarded<vfsRead>},
        {"vfs_exists", guarded<vfsExists>},
        {"vfs_list", guarded<vfsList>},
        {"texture_picture", guarded<texturePicture>},
        {"texture_load", guarded<textureLoad>},
        {"texture_release", guarded<textureRelease>},
        {"rml_exports", guarded<rmlExports>},
        {"rml_fonts", guarded<rmlFonts>},
        {nullptr, nullptr},
    };
    for (const luaL_Reg* r = kNatives; r->name; ++r)
    {
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }
}
