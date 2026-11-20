#include "rml.h"
#include "log.h"
#include "padnav.h"
#include "ui.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/DataTypes.h>
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
#include <memory>
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
    bool g_allowFonts = false;  // the Windows font folder is readable too

    fs::path fromUtf8(const std::string& s)
    {
        return fs::path(std::u8string(s.begin(), s.end()));
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

    bool readable(const fs::path& path)
    {
        if (!g_root.empty() && inside(g_root, path))
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

    bool readAll(const fs::path& path, std::string& out)
    {
        if (!readable(path))
        {
            return false;
        }
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            return false;
        }
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

        bool LogMessage(Rml::Log::Type type, const Rml::String& message) override
        {
            const char* kind = type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT ? "error"
                               : type == Rml::Log::LT_WARNING                            ? "warning"
                                                                                         : "info";
            LOG("RmlUi {}: {}", kind, message);
            return true;
        }
    };

    class FileInterface final : public Rml::FileInterface
    {
    public:
        Rml::FileHandle Open(const Rml::String& path) override
        {
            const fs::path p = fromUtf8(path);
            if (!readable(p))
            {
                LOG("RmlUi: a document may not read {}", path);
                return 0;
            }
            auto file = std::make_unique<std::ifstream>(p, std::ios::binary);
            if (!*file)
            {
                return 0;
            }
            return reinterpret_cast<Rml::FileHandle>(file.release());
        }

        void Close(Rml::FileHandle file) override { delete stream(file); }

        size_t Read(void* buffer, size_t size, Rml::FileHandle file) override
        {
            std::ifstream* in = stream(file);
            in->read(static_cast<char*>(buffer), static_cast<std::streamsize>(size));
            return static_cast<size_t>(in->gcount());
        }

        bool Seek(Rml::FileHandle file, long offset, int origin) override
        {
            std::ifstream* in = stream(file);
            in->clear();
            const std::ios::seekdir dir = origin == 1 ? std::ios::cur : origin == 2 ? std::ios::end : std::ios::beg;
            in->seekg(offset, dir);
            return !in->fail();
        }

        size_t Tell(Rml::FileHandle file) override
        {
            const std::streamoff p = stream(file)->tellg();
            return p < 0 ? 0 : static_cast<size_t>(p);
        }

    private:
        static std::ifstream* stream(Rml::FileHandle file) { return reinterpret_cast<std::ifstream*>(file); }
    };

    // Where the render interface puts what the current draw records (set by rml_draw).
    struct Target
    {
        uint8_t layer = 1;
        int8_t window = -1;
        float unit = 1.0f;          // RmlUi pixels per layout unit
        bool scissor = false;
        float clip[4] = {};         // layout units
        int count = 0;
        std::shared_ptr<const std::array<float, 16>> matrix;    // the element's transform (SetTransform), null: none
    };
    Target g_target;

    struct TextureInfo
    {
        uint32_t id = 0;            // a game image id (picture == false) or a picture id
        uint8_t flip = 0;
        bool picture = false;
    };

    // "gear:image/54" (the game's UI image 54), "gear:texture/GUI_MOVE_RUNDUM.TGA" (a texture of the game's
    // Texture.pak); a "?flip=h", "?flip=v" or "?flip=hv" mirrors it.
    bool parseGearTexture(const std::string& source, TextureInfo& info)
    {
        std::string s = source.substr(5);
        info.flip = 0;
        const size_t q = s.find('?');
        if (q != std::string::npos)
        {
            const std::string opt = s.substr(q + 1);
            s.resize(q);
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
        if (s.rfind("image/", 0) == 0)
        {
            const std::string number = s.substr(6);
            if (number.empty() || number.size() > 9 || number.find_first_not_of("0123456789") != std::string::npos)
            {
                return false;
            }
            info.id = static_cast<uint32_t>(std::stoul(number));
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
            return Ui::meshCreate(std::move(vs), std::move(is));
        }

        void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override
        {
            if (g_target.count >= kMaxMeshesPerDraw)
            {
                return;
            }
            Ui::Command c;
            c.kind = Ui::Kind::Mesh;
            c.layer = static_cast<Ui::Layer>(g_target.layer);
            c.window = g_target.window;
            c.region = static_cast<uint32_t>(geometry);
            c.x = translation.x;
            c.y = translation.y;
            c.w = g_target.unit;
            c.matrix = g_target.matrix;
            if (texture)
            {
                const auto it = m_textures.find(texture);
                if (it == m_textures.end())
                {
                    return;
                }
                c.image = it->second.id;
                c.flip = it->second.flip;
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

        void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override { Ui::meshRelease(static_cast<uint32_t>(geometry)); }

        Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override
        {
            TextureInfo info;
            if (source.rfind("gear:", 0) == 0)
            {
                Ui::ImageInfo image;
                if (parseGearTexture(source, info) && info.picture)
                {
                    dimensions = {64, 64};      // a pad button prompt
                    return add(info);
                }
                if (!info.picture && (info.id == 0 || !Ui::imageInfo(info.id, image) || image.width <= 0 || image.height <= 0))
                {
                    LOG("RmlUi: no game image for {}", source);
                    return 0;
                }
                dimensions = {image.width, image.height};
                return add(info);
            }
            std::string bytes;
            if (!readAll(fromUtf8(source), bytes))
            {
                LOG("RmlUi: cannot read the image {}", source);
                return 0;
            }
            int w = 0, h = 0, channels = 0;
            stbi_uc* data = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()),
                                                  &w, &h, &channels, 4);
            if (!data)
            {
                LOG("RmlUi: cannot decode the image {}", source);
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
            info.id = Ui::pictureCreate32(w, h, std::move(pixels), true);
            if (!info.id)
            {
                return 0;
            }
            info.picture = true;
            dimensions = {w, h};
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
            if (it->second.picture)
            {
                Ui::pictureRelease(it->second.id);
            }
            m_textures.erase(it);
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
            g_target.matrix = std::move(m);
        }

        void EnableScissorRegion(bool enable) override { g_target.scissor = enable; }

        void SetScissorRegion(Rml::Rectanglei region) override
        {
            const float unit = g_target.unit > 0 ? g_target.unit : 1.0f;
            g_target.clip[0] = static_cast<float>(region.Left()) / unit;
            g_target.clip[1] = static_cast<float>(region.Top()) / unit;
            g_target.clip[2] = static_cast<float>(region.Width()) / unit;
            g_target.clip[3] = static_cast<float>(region.Height()) / unit;
        }

    private:
        Rml::TextureHandle add(const TextureInfo& info)
        {
            const Rml::TextureHandle handle = ++m_next;
            m_textures.emplace(handle, info);
            return handle;
        }

        std::unordered_map<Rml::TextureHandle, TextureInfo> m_textures;
        Rml::TextureHandle m_next = 0;
    };

    // Never destroyed: RmlUi's own statics outlive ours when the DLL unloads with the process.
    SystemInterface* g_system = nullptr;
    FileInterface* g_file = nullptr;
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

    class Listener final : public Rml::EventListener
    {
    public:
        Listener(Doc* doc, int id) : m_doc(doc), m_id(id) {}
        void ProcessEvent(Rml::Event& event) override;

    private:
        Doc* m_doc;
        int m_id;
    };

    struct Doc
    {
        int id = 0;
        std::string mod;
        fs::path root;
        Rml::Context* context = nullptr;
        Rml::ElementDocument* document = nullptr;
        std::string contextName;
        bool dp = false;
        float unit = 1.0f;
        bool hovering = false;
        std::vector<std::unique_ptr<Listener>> listeners;
        std::vector<QueuedEvent> events;
        Node model;                             // the data model's values (an object), if the document has one
        Rml::DataModelHandle modelHandle;
        bool hasModel = false;
        unsigned modelSeen = 0;
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
        if (Rml::Element* t = event.GetTargetElement())
        {
            q.target = t->GetId();
        }
        if (Rml::Element* c = event.GetCurrentElement())
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
        explicit RootScope(const Doc& doc) : m_previous(std::move(g_root)) { g_root = doc.root; }
        explicit RootScope(fs::path root) : m_previous(std::move(g_root)) { g_root = std::move(root); }
        ~RootScope() { g_root = std::move(m_previous); }
        RootScope(const RootScope&) = delete;
        RootScope& operator=(const RootScope&) = delete;

    private:
        fs::path m_previous;
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
        // The UI's own face (Segoe UI, as gear.ui text) with its styles; symbols as the fallback.
        const fs::path fonts = fontsDir();
        g_allowFonts = true;
        loadFont(fonts / "segoeui.ttf", true);
        loadFont(fonts / "segoeuib.ttf", false);
        loadFont(fonts / "segoeuii.ttf", false);
        loadFont(fonts / "segoeuiz.ttf", false);
        loadFont(fonts / "seguisym.ttf", true);
        g_allowFonts = false;
        LOG("RmlUi {} started", Rml::GetVersion());
        return true;
    }

    // The context's size and density for the canvas as it is now; `dp` documents are laid out in screen pixels
    // (1dp = one layout unit), the others in layout units (1px = one layout unit).
    void fitContext(Doc& d)
    {
        const float unit = d.dp ? std::max(0.5f, Ui::canvasScale()) : 1.0f;
        const Rml::Vector2i dims(static_cast<int>(std::lround(kLayoutWidth * unit)), static_cast<int>(std::lround(kLayoutHeight * unit)));
        if (unit != d.unit || d.context->GetDimensions() != dims)
        {
            d.unit = unit;
            d.context->SetDimensions(dims);
            d.context->SetDensityIndependentPixelRatio(d.dp ? unit : 1.0f);
        }
    }

    Doc* findDoc(int id)
    {
        const auto it = g_docs.find(id);
        return it == g_docs.end() ? nullptr : it->second.get();
    }

    Rml::Element* find(Doc& d, const std::string& selector)
    {
        if (selector.empty())
        {
            return d.document;
        }
        return d.document->QuerySelector(selector);
    }

    constexpr const char* kNavSelector = "button, a, input, select, textarea, [data-nav]";

    // The elements the pad and the mouse can press, in document order.
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
            if (!e->IsVisible(true) || e->GetAttribute("disabled") != nullptr)
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

    Doc& docArg(lua_State* L, int arg = 1)
    {
        Doc* d = findDoc(static_cast<int>(luaL_checknumber(L, arg)));
        if (!d)
        {
            throw Failure{"the document is closed"};
        }
        return *d;
    }

    int pushFail(lua_State* L, const std::string& message)
    {
        lua_pushnil(L);
        push(L, message);
        return 2;
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
        {
            RootScope scope(*d);
            d->document = fromString ? d->context->LoadDocumentFromMemory(source, toUtf8(file))
                                     : d->context->LoadDocument(toUtf8(file));
        }
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

    // native.rml_update(id, mouse_x, mouse_y, hover) -> {event, ...}: lays the document out and runs its animations; the
    // mouse in layout units, `hover` whether it is over this document. An event is {listener, type, target, current,
    // params = {name = text}} for the listeners registered with rml_listen.
    int rmlUpdate(lua_State* L)
    {
        Doc& d = docArg(L);
        const double mx = luaL_checknumber(L, 2), my = luaL_checknumber(L, 3);
        const bool hover = lua_toboolean(L, 4) != 0;
        RootScope scope(d);
        fitContext(d);
        if (hover)
        {
            d.context->ProcessMouseMove(static_cast<int>(mx * d.unit), static_cast<int>(my * d.unit), 0);
            d.hovering = true;
        }
        else if (d.hovering)
        {
            d.context->ProcessMouseLeave();
            d.hovering = false;
        }
        d.context->Update();
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
        RootScope scope(d);
        d.context->Render();
        lua_pushnumber(L, g_target.count);
        return 1;
    }

    void pushRect(lua_State* L, Rml::Element* e, float unit, int index, bool panel = false)
    {
        const Rml::Vector2f o = e->GetAbsoluteOffset(Rml::BoxArea::Border);
        const Rml::Vector2f s = e->GetBox().GetSize(Rml::BoxArea::Border);
        lua_createtable(L, 0, 4);
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
        }
        lua_rawseti(L, -2, index);
    }

    // native.rml_rects(id) -> {{x, y, w, h, panel, title}, ...}: the boxes of the document's top-level elements (the windows), in
    // layout units; what takes the mouse. `panel` is the element's data-panel attribute ('' if none): a window the layout
    // editor may move (see rml_move).
    int rmlRects(lua_State* L)
    {
        Doc& d = docArg(L);
        lua_newtable(L);
        if (!d.document->IsVisible())
        {
            return 1;
        }
        int index = 1;
        for (int i = 0; i < d.document->GetNumChildren(); ++i)
        {
            Rml::Element* c = d.document->GetChild(i);
            const Rml::Vector2f s = c->GetBox().GetSize(Rml::BoxArea::Border);
            if (c->IsVisible() && s.x > 0 && s.y > 0)
            {
                pushRect(L, c, d.unit, index++, true);
            }
        }
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
            e->SetProperty("left", std::to_string(static_cast<int>(std::lround(x * d.unit))) + "px");
            e->SetProperty("top", std::to_string(static_cast<int>(std::lround(y * d.unit))) + "px");
        }
        lua_pushboolean(L, e != nullptr);
        return 1;
    }

    // native.rml_nav(id) -> {{x, y, w, h}, ...}: the pressable elements (buttons, links, inputs, anything with data-nav),
    // in layout units; the index is what rml_click takes.
    int rmlNav(lua_State* L)
    {
        Doc& d = docArg(L);
        lua_newtable(L);
        int index = 1;
        for (Rml::Element* e : navElements(d))
        {
            pushRect(L, e, d.unit, index++);
        }
        return 1;
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

    // native.rml_listen(id, selector, event) -> listener id, elements | nil: events of that type on the matching
    // elements come back from rml_update with the listener id.
    int rmlListen(lua_State* L)
    {
        Doc& d = docArg(L);
        const std::string selector = str(L, 2);
        const std::string event = str(L, 3);
        Rml::ElementList list;
        if (selector.empty())
        {
            list.push_back(d.document);
        }
        else
        {
            d.document->QuerySelectorAll(list, selector);
        }
        if (list.empty() || d.listeners.size() >= 1024)
        {
            lua_pushnil(L);
            return 1;
        }
        const int id = static_cast<int>(d.listeners.size()) + 1;
        d.listeners.push_back(std::make_unique<Listener>(&d, id));
        for (Rml::Element* e : list)
        {
            e->AddEventListener(event, d.listeners.back().get());
        }
        lua_pushnumber(L, id);
        lua_pushnumber(L, static_cast<lua_Number>(list.size()));
        return 2;
    }

    // native.rml_wants_text(id) -> whether a text field has the focus
    int rmlWantsText(lua_State* L)
    {
        Doc& d = docArg(L);
        Rml::Element* f = d.context->GetFocusElement();
        bool wants = false;
        if (f)
        {
            const std::string tag = f->GetTagName();
            if (tag == "textarea")
            {
                wants = true;
            }
            else if (tag == "input")
            {
                const std::string type = f->GetAttribute<Rml::String>("type", "text");
                wants = type != "checkbox" && type != "radio" && type != "submit" && type != "button" && type != "range" &&
                        type != "hidden";
            }
        }
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
        default: return K::KI_UNKNOWN;
        }
    }

    // native.rml_key(id, vk, down) -> whether the key is one RmlUi takes (Backspace, Delete, Enter, Tab, Escape, arrows,
    // Home, End, Page Up / Down)
    int rmlKey(lua_State* L)
    {
        Doc& d = docArg(L);
        const Rml::Input::KeyIdentifier key = keyFor(static_cast<int>(luaL_checknumber(L, 2)));
        if (key == Rml::Input::KI_UNKNOWN)
        {
            lua_pushboolean(L, 0);
            return 1;
        }
        if (lua_toboolean(L, 3))
        {
            d.context->ProcessKeyDown(key, 0);
        }
        else
        {
            d.context->ProcessKeyUp(key, 0);
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
        const fs::path file = root / fromUtf8(str(L, 2));
        bool ok = false;
        if (inside(root, file))
        {
            RootScope scope(root);
            ok = Rml::LoadFontFace(toUtf8(file), lua_toboolean(L, 3) != 0);
        }
        lua_pushboolean(L, ok);
        return 1;
    }
}

void RmlHost::configure(const std::wstring& modsDir)
{
    g_modsDir = modsDir;
}

void RmlHost::registerNatives(lua_State* L)
{
    static const luaL_Reg kNatives[] = {
        {"rml_load", guarded<rmlLoad>},
        {"rml_load_string", guarded<rmlLoadString>},
        {"rml_close", guarded<rmlClose>},
        {"rml_show", guarded<rmlShow>},
        {"rml_update", guarded<rmlUpdate>},
        {"rml_draw", guarded<rmlDraw>},
        {"rml_rects", guarded<rmlRects>},
        {"rml_move", guarded<rmlMove>},
        {"rml_model_set", guarded<rmlModelSet>},
        {"rml_model_changed", guarded<rmlModelChanged>},
        {"rml_nav", guarded<rmlNav>},
        {"rml_click", guarded<rmlClick>},
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
        {nullptr, nullptr},
    };
    for (const luaL_Reg* r = kNatives; r->name; ++r)
    {
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }
}
