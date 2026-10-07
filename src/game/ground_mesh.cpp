#include "game/ground_mesh.h"
#include "game/d3d_stats.h"
#include "game/device_proxy.h"
#include "game/resolution.h"
#include "game/sacred_addr.h"
#include "ddraw9/ground.h"
#include "config.h"
#include "log.h"
#include "patch.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

namespace
{
    using namespace Sacred;
    namespace Ground = DDraw9::Ground;

    // thiscall targets are hooked and called as fastcall with an unused EDX parameter.
    using TileRowFn = void(__fastcall*)(void* self, void* edx, void* device, void* rowPos, int detail);
    using DeviceFn = void(__fastcall*)(void* self, void* edx, void* device);
    using AddFn = void(__fastcall*)(void* self, void* edx, void* device, const float* quad);
    using SetTextureFn = void(__fastcall*)(void* self, void* edx, void* device, uint32_t handle);
    using RecordFn = uint8_t*(__fastcall*)(void* owner, void* edx, uint32_t id);
    using TextureGetFn = uint8_t*(__fastcall*)(void* manager, void* edx, uint32_t handle, int flags);
    using FlagsInstanceFn = void*(__cdecl*)();
    using FlagsSetFn = uint32_t(__fastcall*)(void* flags, void* edx, uint32_t flag, uint32_t on);
    using WorldStateFn = uint8_t*(__cdecl*)();

    TileRowFn g_origTileRow = nullptr;
    DeviceFn g_origLayers = nullptr;
    AddFn g_origAdd = nullptr;
    SetTextureFn g_origSetTexture = nullptr;

    template <class T>
    T& member(void* obj, uintptr_t offset)
    {
        return *reinterpret_cast<T*>(static_cast<uint8_t*>(obj) + offset);
    }

    template <class T>
    T field(const uint8_t* p, uintptr_t offset)
    {
        T v;
        std::memcpy(&v, p + offset, sizeof(T));
        return v;
    }

    struct RowPos
    {
        float x, y;
        int16_t tile;       // row * 64 + column within the sector
        int16_t sector;     // 3x3 grid: row * 3 + column
    };

    constexpr int kTilesPerSector = 64 * 64;
    constexpr int kLoaded = 192;            // loaded tiles per side (3x3 sectors)
    constexpr uint32_t kRenderFlagLayers = 0x2000;
    constexpr float kGroundZ = 0.9f;        // the game's z for ground quads (renderTileRow)
    // Corners in the game's vertex order: left, top, bottom, right. Offsets around the tile position (renderTileRow,
    // set by cWorldView_updateViewMetrics divided by the zoom), and which of the tile's height/light bytes they take.
    constexpr float kCornerX[4] = {-48.2f, 0.0f, 0.0f, 48.2f};
    constexpr float kCornerY[4] = {0.0f, -24.2f, 24.2f, 0.0f};
    constexpr int kCornerByte[4] = {0, 1, 3, 2};
    constexpr size_t kUvFloats = WorldView::tileUvCells * 8;

    // Atlas pages of ground textures: 256x256 sheets with a one-texel gutter (wrap addressing), on a fixed grid.
    constexpr int kAtlasSize = 4096;
    constexpr int kAtlasCell = 258;
    constexpr int kAtlasCellsPerRow = kAtlasSize / kAtlasCell;
    constexpr int kAtlasCells = kAtlasCellsPerRow * kAtlasCellsPerRow;
    constexpr int kMaxPages = 4;
    constexpr int kMaxSectorCaches = 16;    // the 9 loaded sectors and some the camera left recently

    struct AtlasPage
    {
        IDirectDrawSurface7* surface = nullptr;
        DDPIXELFORMAT format = {};
        int used = 0;
    };

    // A ground texture's place: page -1 = no texture (the game draws such tiles untextured as well).
    struct TextureSlot
    {
        int page = -1;
        float scaleU = 1, scaleV = 1, offsetU = 0, offsetV = 0;
    };

    // The fields a tile's quads are made of.
    struct TileKey
    {
        int32_t def = 0;
        uint32_t layers = 0;
        uint32_t heights = 0;
        uint32_t light = 0;
        bool operator==(const TileKey&) const = default;
    };

    // Quads of one pass (0 base tiles, 1.. layer passes) and atlas pages, in the order the tiles were built.
    struct Group
    {
        uint16_t pass = 0;
        int8_t page0 = -1, page1 = -1;
        std::vector<Ground::Vertex> vertices;
        Ground::Mesh* mesh = nullptr;
        uint32_t uploaded = 0;  // quads in the mesh
    };

    struct SectorCache
    {
        uint8_t* sector = nullptr;
        uint8_t* tiles = nullptr;
        uint32_t frame = 0;     // last frame it was used
        std::vector<Group> groups;
        std::vector<TileKey> keys = std::vector<TileKey>(kTilesPerSector);
        std::vector<uint8_t> built = std::vector<uint8_t>(kTilesPerSector);
        float minX = 0, minY = 0, maxX = 0, maxY = 0;
        bool any = false;
    };

    enum class Build
    {
        Ok,
        AtlasFull,      // start over: the textures in use are copied again into emptied pages
        Unsupported,    // the cache can't stand in for the game here: off for the session
    };

    struct State
    {
        bool enabled = false;       // installed and not given up
        IDirect3DDevice7* real = nullptr;
        std::vector<std::unique_ptr<SectorCache>> caches;
        std::unordered_map<uint32_t, TextureSlot> textures;
        AtlasPage pages[kMaxPages];
        IDirectDraw7* ddraw = nullptr;
        // What every cached quad depends on besides its tile.
        uint8_t* mapData = nullptr;
        uint8_t* defs = nullptr;
        float uvs[kUvFloats] = {};
        bool uvsChecked = false;
        bool startOver = false;     // atlas full: empty everything at the next frame

        // This frame.
        void* view = nullptr;
        bool active = false;        // the game's ground draws are skipped
        bool groundRows = false;    // a row with ground was walked
        bool haveOffset = false;
        float offsetX = 0, offsetY = 0;     // view position of loaded tile (0, 0)
        bool drawn = false;
        uint32_t frame = 0;
        SectorCache* slots[9] = {};

        // Statistics ([Debug] D3DStats).
        uint32_t tilesBuilt = 0, sectorResets = 0, draws = 0, frames = 0, offsetMismatches = 0;
        DWORD nextLog = 0;
    };
    State g;
    bool g_inRow = false;       // inside the game's renderTileRow while the cache draws the ground

    void releaseCache(SectorCache& c)
    {
        for (Group& group : c.groups)
        {
            Ground::releaseMesh(group.mesh);
        }
        c.groups.clear();
        std::fill(c.built.begin(), c.built.end(), uint8_t(0));
        c.any = false;
    }

    // Forgets every cached tile and texture copy (the pages stay, emptied).
    void resetAll()
    {
        for (auto& c : g.caches)
        {
            releaseCache(*c);
        }
        g.caches.clear();
        g.textures.clear();
        for (AtlasPage& page : g.pages)
        {
            page.used = 0;
        }
        std::fill(std::begin(g.slots), std::end(g.slots), nullptr);
    }

    void disable(const char* why)
    {
        if (!g.enabled)
        {
            return;
        }
        LOG("Ground mesh: off for this session, {}", why);
        g.enabled = false;
        g.active = false;
        resetAll();
        for (AtlasPage& page : g.pages)
        {
            if (page.surface)
            {
                page.surface->Release();
                page.surface = nullptr;
            }
        }
        if (g.ddraw)
        {
            g.ddraw->Release();
            g.ddraw = nullptr;
        }
    }

    bool sameFormat(const DDPIXELFORMAT& a, const DDPIXELFORMAT& b)
    {
        return a.dwFlags == b.dwFlags && a.dwRGBBitCount == b.dwRGBBitCount && a.dwRBitMask == b.dwRBitMask &&
            a.dwGBitMask == b.dwGBitMask && a.dwBBitMask == b.dwBBitMask && a.dwRGBAlphaBitMask == b.dwRGBAlphaBitMask;
    }

    AtlasPage* createPage(IDirectDrawSurface7* texture, const DDPIXELFORMAT& format)
    {
        AtlasPage* free = nullptr;
        for (AtlasPage& page : g.pages)
        {
            if (!page.surface)
            {
                free = &page;
                break;
            }
        }
        if (!free)
        {
            return nullptr;
        }
        if (!g.ddraw)
        {
            void* dd = nullptr;
            if (FAILED(texture->GetDDInterface(&dd)) || !dd)
            {
                return nullptr;
            }
            g.ddraw = static_cast<IDirectDraw7*>(dd);   // holds the reference GetDDInterface added
        }
        DDSURFACEDESC2 desc = {};
        desc.dwSize = sizeof(desc);
        desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        desc.dwWidth = kAtlasSize;
        desc.dwHeight = kAtlasSize;
        desc.ddpfPixelFormat = format;
        desc.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY;
        IDirectDrawSurface7* surface = nullptr;
        const HRESULT hr = g.ddraw->CreateSurface(&desc, &surface, nullptr);
        if (FAILED(hr) || !surface)
        {
            LOG("Ground mesh: creating a {}x{} {} bpp atlas page failed ({:08x})", kAtlasSize, kAtlasSize,
                format.dwRGBBitCount, static_cast<uint32_t>(hr));
            return nullptr;
        }
        free->surface = surface;
        free->format = format;
        free->used = 0;
        LOG("Ground mesh: atlas page {} ({}x{}, {} bpp); {}", free - g.pages + 1, kAtlasSize, kAtlasSize,
            format.dwRGBBitCount, D3DStats::memorySummary());
        return free;
    }

    // Copies `texture` into a free cell of `page` with a wrap gutter (as the batcher's atlas does).
    bool upload(AtlasPage& page, int cell, IDirectDrawSurface7* texture, LONG w, LONG h)
    {
        const LONG x = (cell % kAtlasCellsPerRow) * kAtlasCell + 1;
        const LONG y = (cell / kAtlasCellsPerRow) * kAtlasCell + 1;
        struct Copy
        {
            LONG dx, dy, sx, sy, cw, ch;
        };
        const Copy copies[] = {
            {x - 1, y, w - 1, 0, 1, h}, {x + w, y, 0, 0, 1, h},
            {x, y - 1, 0, h - 1, w, 1}, {x, y + h, 0, 0, w, 1},
            {x - 1, y - 1, w - 1, h - 1, 1, 1}, {x + w, y - 1, 0, h - 1, 1, 1},
            {x - 1, y + h, w - 1, 0, 1, 1}, {x + w, y + h, 0, 0, 1, 1},
            {x, y, 0, 0, w, h},
        };
        for (const Copy& c : copies)
        {
            RECT dst = {c.dx, c.dy, c.dx + c.cw, c.dy + c.ch};
            RECT src = {c.sx, c.sy, c.sx + c.cw, c.sy + c.ch};
            const HRESULT hr = page.surface->Blt(&dst, texture, &src, DDBLT_WAIT, nullptr);
            if (FAILED(hr))
            {
                LOG("Ground mesh: copying a {}x{} texture into the atlas failed ({:08x})", w, h, static_cast<uint32_t>(hr));
                return false;
            }
        }
        D3DStats::count(D3DStats::CAtlasUpload);
        return true;
    }

    Build textureSlot(uint32_t handle, const TextureSlot*& out)
    {
        auto it = g.textures.find(handle);
        if (it != g.textures.end())
        {
            out = &it->second;
            return Build::Ok;
        }
        // As the game's flush looks it up: loads the texture if it isn't.
        void* manager = *reinterpret_cast<void**>(Addr::g_pTextureManager);
        uint8_t* texture = manager ? reinterpret_cast<TextureGetFn>(Addr::cTextureManager_get)(manager, nullptr, handle, 0)
                                   : nullptr;
        auto* surface = texture ? member<IDirectDrawSurface7*>(texture, 0x14) : nullptr;
        TextureSlot slot;
        if (surface)
        {
            DDSURFACEDESC2 desc = {};
            desc.dwSize = sizeof(desc);
            if (FAILED(surface->GetSurfaceDesc(&desc)))
            {
                return Build::Unsupported;
            }
            const LONG w = static_cast<LONG>(desc.dwWidth), h = static_cast<LONG>(desc.dwHeight);
            if (w <= 0 || h <= 0 || w > kAtlasCell - 2 || h > kAtlasCell - 2)
            {
                LOG("Ground mesh: a ground texture is {}x{}, larger than the atlas cells", w, h);
                return Build::Unsupported;
            }
            AtlasPage* page = nullptr;
            for (AtlasPage& p : g.pages)
            {
                if (p.surface && p.used < kAtlasCells && sameFormat(p.format, desc.ddpfPixelFormat))
                {
                    page = &p;
                    break;
                }
            }
            if (!page)
            {
                page = createPage(surface, desc.ddpfPixelFormat);
            }
            if (!page)
            {
                return Build::AtlasFull;
            }
            const int cell = page->used++;
            if (!upload(*page, cell, surface, w, h))
            {
                return Build::Unsupported;
            }
            const float x = static_cast<float>((cell % kAtlasCellsPerRow) * kAtlasCell + 1);
            const float y = static_cast<float>((cell / kAtlasCellsPerRow) * kAtlasCell + 1);
            slot.page = static_cast<int>(page - g.pages);
            slot.scaleU = static_cast<float>(w) / kAtlasSize;
            slot.scaleV = static_cast<float>(h) / kAtlasSize;
            slot.offsetU = x / kAtlasSize;
            slot.offsetV = y / kAtlasSize;
        }
        out = &g.textures.emplace(handle, slot).first->second;
        return Build::Ok;
    }

    TileKey readKey(const uint8_t* tile)
    {
        return {field<int32_t>(tile, Tile::def), field<uint32_t>(tile, Tile::layers), field<uint32_t>(tile, Tile::heights),
            field<uint32_t>(tile, Tile::light)};
    }

    Group& group(SectorCache& c, uint16_t pass, int page0, int page1)
    {
        for (Group& gr : c.groups)
        {
            if (gr.pass == pass && gr.page0 == page0 && gr.page1 == page1)
            {
                return gr;
            }
        }
        Group& gr = c.groups.emplace_back();
        gr.pass = pass;
        gr.page0 = static_cast<int8_t>(page0);
        gr.page1 = static_cast<int8_t>(page1);
        return gr;
    }

    // The tile definition's texture and cell, through the atlas.
    struct Corners
    {
        float u[4], v[4];
        int page;
    };

    Build corners(uint32_t defIndex, uint32_t cell, Corners& out)
    {
        const uint8_t* def = g.defs + size_t(defIndex) * TileDef::size;
        const TextureSlot* slot = nullptr;
        const Build b = textureSlot(field<uint32_t>(def, TileDef::texture), slot);
        if (b != Build::Ok)
        {
            return b;
        }
        out.page = slot->page;
        // Cells past the table read on into the view, as the game's do; the table holds 18.
        cell = std::min<uint32_t>(cell, WorldView::tileUvCells - 1);
        for (int i = 0; i < 4; ++i)
        {
            out.u[i] = g.uvs[cell * 8 + i * 2] * slot->scaleU + slot->offsetU;
            out.v[i] = g.uvs[cell * 8 + i * 2 + 1] * slot->scaleV + slot->offsetV;
        }
        return Build::Ok;
    }

    void appendQuad(SectorCache& c, Group& gr, const float (&x)[4], const float (&y)[4], const DWORD (&diffuse)[4],
        const Corners& stage0, const Corners* stage1)
    {
        for (int i = 0; i < 4; ++i)
        {
            Ground::Vertex v;
            v.x = x[i];
            v.y = y[i];
            v.diffuse = diffuse[i];
            v.u0 = stage0.u[i];
            v.v0 = stage0.v[i];
            v.u1 = stage1 ? stage1->u[i] : 0.0f;
            v.v1 = stage1 ? stage1->v[i] : 0.0f;
            gr.vertices.push_back(v);
        }
        for (int i = 0; i < 4; ++i)
        {
            if (!c.any)
            {
                c.minX = c.maxX = x[i];
                c.minY = c.maxY = y[i];
                c.any = true;
            }
            c.minX = std::min(c.minX, x[i]);
            c.maxX = std::max(c.maxX, x[i]);
            c.minY = std::min(c.minY, y[i]);
            c.maxY = std::max(c.maxY, y[i]);
        }
    }

    // The base quad and the blend layer quads of tile `t`, as renderTileRow and drawTileLayers make them.
    Build buildTile(SectorCache& c, int t, const uint8_t* tile, const TileKey& key)
    {
        const int row = t / 64, col = t % 64;
        const float px = 48.0f * static_cast<float>(col - row);
        const float py = 24.0f * static_cast<float>(col + row);
        const auto* heights = tile + Tile::heights;
        const auto* light = tile + Tile::light;
        float x[4], y[4];
        DWORD diffuse[4];
        for (int i = 0; i < 4; ++i)
        {
            const int b = kCornerByte[i];
            x[i] = px + kCornerX[i];
            y[i] = py + kCornerY[i] - static_cast<float>(static_cast<int8_t>(heights[b]));
            diffuse[i] = 0xFF000000u | 0x010101u * light[b];
        }

        const uint8_t* def = g.defs + size_t(static_cast<uint32_t>(key.def)) * TileDef::size;
        Corners base;
        Build b = corners(static_cast<uint32_t>(key.def), field<uint16_t>(def, TileDef::uvCell), base);
        if (b != Build::Ok)
        {
            return b;
        }
        appendQuad(c, group(c, 0, base.page, -1), x, y, diffuse, base, nullptr);

        // Pass 0 (and every even one) draws two-texture records, the odd ones one-texture records; a chain moves on
        // to the next pass at each change of kind.
        auto record = reinterpret_cast<RecordFn>(Addr::layerRecordCache);
        void* mapData = g.mapData;
        uint32_t id = key.layers;
        int pass = 0;
        for (int n = 0; id && n < 256; ++n)
        {
            const uint8_t* r = record(mapData, nullptr, id);
            if (!r)
            {
                break;
            }
            const uint32_t defs = field<uint32_t>(r, LayerRecord::defs);
            const uint32_t next = field<uint32_t>(r, LayerRecord::next);
            const uint32_t a = defs & 0x1FFFF, second = defs >> 17;
            const bool twoTextures = second != 0;
            if (twoTextures != (pass % 2 == 0))
            {
                ++pass;
            }
            Corners c0, c1;
            b = corners(a, a % WorldView::tileUvCells, c0);
            if (b == Build::Ok && twoTextures)
            {
                b = corners(second, second % WorldView::tileUvCells, c1);
            }
            if (b != Build::Ok)
            {
                return b;
            }
            appendQuad(c, group(c, static_cast<uint16_t>(1 + pass), c0.page, twoTextures ? c1.page : -1), x, y, diffuse,
                c0, twoTextures ? &c1 : nullptr);
            id = next;
        }
        c.keys[t] = key;
        c.built[t] = 1;
        ++g.tilesBuilt;
        return Build::Ok;
    }

    SectorCache* cacheFor(uint8_t* sector, uint8_t* tiles)
    {
        for (auto& c : g.caches)
        {
            if (c->sector == sector && c->tiles == tiles)
            {
                return c.get();
            }
        }
        if (g.caches.size() >= kMaxSectorCaches)
        {
            // The least recently used one that isn't in use this frame.
            auto oldest = g.caches.end();
            for (auto it = g.caches.begin(); it != g.caches.end(); ++it)
            {
                if ((*it)->frame != g.frame && (oldest == g.caches.end() || (*it)->frame < (*oldest)->frame))
                {
                    oldest = it;
                }
            }
            if (oldest == g.caches.end())
            {
                return nullptr;
            }
            releaseCache(**oldest);
            g.caches.erase(oldest);
        }
        auto& c = g.caches.emplace_back(std::make_unique<SectorCache>());
        c->sector = sector;
        c->tiles = tiles;
        return c.get();
    }

    // Tile `t` of loaded sector `slot` in the cache, built if it isn't or if it changed.
    bool visitTile(int slot, int t)
    {
        auto* sector = member<uint8_t*>(g.view, WorldView::sectors + slot * 4);
        auto* tiles = sector ? member<uint8_t*>(sector, Sacred::Sector::tiles) : nullptr;
        if (!tiles)
        {
            return true;
        }
        SectorCache* c = g.slots[slot];
        if (!c || c->sector != sector || c->tiles != tiles)
        {
            c = cacheFor(sector, tiles);
            if (!c)
            {
                return true;
            }
            g.slots[slot] = c;
        }
        c->frame = g.frame;
        const uint8_t* tile = tiles + size_t(t) * Tile::size;
        const TileKey key = readKey(tile);
        if (c->built[t])
        {
            if (c->keys[t] == key)
            {
                return true;
            }
            // Quads can't be taken out of the buffers: the sector starts over (its tiles are built again as the walk
            // visits them).
            releaseCache(*c);
            ++g.sectorResets;
        }
        switch (buildTile(*c, t, tile, key))
        {
        case Build::Ok:
            return true;
        case Build::AtlasFull:
            LOG("Ground mesh: atlas pages full ({} textures), starting over", g.textures.size());
            g.startOver = true;
            return false;
        case Build::Unsupported:
            disable("a ground texture can't be copied into its atlas");
            return false;
        }
        return false;
    }

    // Builds what is new in the row the game is about to walk, and takes the view offset from it.
    void visitRow(void* self, const RowPos& pos)
    {
        if (g.startOver || pos.sector < 0 || pos.sector > 8 || pos.tile < 0 || pos.tile > 0xFFF)
        {
            return;
        }
        const int r0 = pos.sector / 3 * 64 + pos.tile / 64;
        const int c0 = pos.sector % 3 * 64 + pos.tile % 64;
        const float ox = pos.x - 48.0f * static_cast<float>(c0 - r0);
        const float oy = pos.y - 24.0f * static_cast<float>(c0 + r0);
        if (!g.haveOffset)
        {
            g.haveOffset = true;
            g.offsetX = ox;
            g.offsetY = oy;
        }
        else if ((ox != g.offsetX || oy != g.offsetY) && g.offsetMismatches++ < 10)
        {
            LOG("Ground mesh: row at ({}, {}) sector {} tile {} gives view offset ({}, {}), the frame's first row ({}, {})",
                pos.x, pos.y, pos.sector, pos.tile, ox, oy, g.offsetX, g.offsetY);
        }
        const int length = member<int32_t>(self, WorldView::rowLength);
        // Tile j of the row is (r0 - j, c0 + j) in the loaded tiles.
        for (int j = 0; j < length && g.active; ++j)
        {
            const int r = r0 - j, c = c0 + j;
            if (r < 0 || c >= kLoaded)
            {
                break;
            }
            if (!visitTile(r / 64 * 3 + c / 64, r % 64 * 64 + c % 64))
            {
                // This frame's ground is incomplete: the rows still skip the game's ground draws, the next frame
                // starts over (or the game draws it again).
                return;
            }
        }
    }

    bool drawCallback(IDirect3DDevice7* real, void* context)
    {
        return Ground::draw(real, *static_cast<const Ground::Draw*>(context));
    }

    bool upload(Group& gr)
    {
        const uint32_t quads = static_cast<uint32_t>(gr.vertices.size() / 4);
        if (quads == gr.uploaded && gr.mesh)
        {
            return true;
        }
        if (!gr.mesh || Ground::capacity(gr.mesh) < quads)
        {
            // A new buffer with room to grow; Direct3D keeps the old one alive for frames still in flight.
            const uint32_t capacity = std::max({quads + quads / 2, Ground::capacity(gr.mesh) * 2, 1024u});
            Ground::releaseMesh(gr.mesh);
            gr.uploaded = 0;
            gr.mesh = Ground::createMesh(g.real, capacity);
            if (!gr.mesh)
            {
                return false;
            }
        }
        if (!Ground::write(g.real, gr.mesh, gr.uploaded, gr.vertices.data() + size_t(gr.uploaded) * 4,
                quads - gr.uploaded))
        {
            return false;
        }
        gr.uploaded = quads;
        return true;
    }

    // The ground as drawTileLayers would leave it: base tiles, then the layer passes with their render flag.
    void drawGround(void* device)
    {
        DeviceProxy* proxy = DeviceProxy::instance();
        if (!proxy || device != proxy || !g.haveOffset || !g.groundRows)
        {
            return;
        }
        const float zoom = member<float>(g.view, WorldView::zoom);
        if (!(zoom > 0.0f))
        {
            return;
        }
        const float invZoom = 1.0f / zoom;
        const float width = static_cast<float>(Resolution::width());
        const float height = static_cast<float>(Resolution::height());
        int maxPass = -1;
        float slotX[9], slotY[9];
        bool visible[9] = {};
        for (int s = 0; s < 9; ++s)
        {
            SectorCache* c = g.slots[s];
            if (!c || !c->any || c->frame != g.frame)
            {
                continue;
            }
            // Loaded tile (64 * row, 64 * column) of this slot.
            const int sr = s / 3 * 64, sc = s % 3 * 64;
            slotX[s] = g.offsetX + 48.0f * static_cast<float>(sc - sr);
            slotY[s] = g.offsetY + 24.0f * static_cast<float>(sc + sr);
            if ((c->maxX + slotX[s]) * invZoom < 0.0f || (c->minX + slotX[s]) * invZoom > width ||
                (c->maxY + slotY[s]) * invZoom < 0.0f || (c->minY + slotY[s]) * invZoom > height)
            {
                continue;
            }
            visible[s] = true;
            for (Group& gr : c->groups)
            {
                if (!upload(gr))
                {
                    disable("a vertex buffer could not be created or filled");
                    return;
                }
                maxPass = std::max(maxPass, static_cast<int>(gr.pass));
            }
        }
        auto setFlag = reinterpret_cast<FlagsSetFn>(Addr::renderFlags_set);
        void* flags = reinterpret_cast<FlagsInstanceFn>(Addr::renderFlags_instance)();
        bool failed = false;
        for (int pass = 0; pass <= maxPass && !failed; ++pass)
        {
            const bool twoTextures = pass >= 1 && (pass - 1) % 2 == 0;
            if (pass >= 1)
            {
                setFlag(flags, nullptr, kRenderFlagLayers, twoTextures ? 0 : 1);
            }
            for (int s = 0; s < 9 && !failed; ++s)
            {
                if (!visible[s])
                {
                    continue;
                }
                for (Group& gr : g.slots[s]->groups)
                {
                    if (gr.pass != pass || !gr.uploaded)
                    {
                        continue;
                    }
                    // Stage 1 only for two-texture layers; elsewhere the game leaves it as it is.
                    proxy->SetTexture(0, gr.page0 >= 0 ? g.pages[gr.page0].surface : nullptr);
                    if (twoTextures)
                    {
                        proxy->SetTexture(1, gr.page1 >= 0 ? g.pages[gr.page1].surface : nullptr);
                    }
                    Ground::Draw d = {gr.mesh, 0, gr.uploaded, invZoom, slotX[s] * invZoom, slotY[s] * invZoom, kGroundZ};
                    if (!proxy->drawDirect(&drawCallback, &d))
                    {
                        failed = true;
                        break;
                    }
                    ++g.draws;
                }
            }
        }
        if (maxPass >= 1)
        {
            setFlag(flags, nullptr, kRenderFlagLayers, 1);
        }
        if (failed)
        {
            disable("the render state at the ground needs something its shader doesn't do");
        }
    }

    void __fastcall hookTileRow(void* self, void* edx, void* device, void* rowPos, int detail)
    {
        if (!g.active || self != g.view)
        {
            g_origTileRow(self, edx, device, rowPos, detail);
            return;
        }
        if (detail)
        {
            g.groundRows = true;
            visitRow(self, *static_cast<const RowPos*>(rowPos));
        }
        g_inRow = g.active;
        g_origTileRow(self, edx, device, rowPos, detail);
        g_inRow = false;
        // The game's blend layer list (for drawTileLayers) is not used: keep it empty, so it never fills up and asks
        // for a flush in the middle of the walk.
        member<uint32_t>(self, WorldView::layeredTileCount) = 0;
    }

    void __fastcall hookAdd(void* self, void* edx, void* device, const float* quad)
    {
        if (!g_inRow)
        {
            g_origAdd(self, edx, device, quad);
        }
    }

    void __fastcall hookSetTexture(void* self, void* edx, void* device, uint32_t handle)
    {
        if (!g_inRow)
        {
            g_origSetTexture(self, edx, device, handle);
        }
    }

    // After the rows (or in the middle of them, before water tiles the walk flushes early): the cached ground once per
    // frame instead of the game's layers.
    void __fastcall hookLayers(void* self, void* edx, void* device)
    {
        if (!g.active || self != g.view)
        {
            g_origLayers(self, edx, device);
            return;
        }
        if (!g.drawn)
        {
            g.drawn = true;
            drawGround(device);
        }
    }

    bool dynamicLight()
    {
        const uint8_t* ws = reinterpret_cast<WorldStateFn>(Addr::worldState_instance)();
        return ws && ((ws[WorldState::flags] & 0x20) || ws[WorldState::tileRendererLight]);
    }

    void logStats()
    {
        ++g.frames;
        const DWORD now = GetTickCount();
        if (!g.nextLog)
        {
            g.nextLog = now + 5000;
            return;
        }
        if (static_cast<int>(now - g.nextLog) < 0)
        {
            return;
        }
        g.nextLog = now + 5000;
        size_t quads = 0, groups = 0;
        for (auto& c : g.caches)
        {
            groups += c->groups.size();
            for (const Group& gr : c->groups)
            {
                quads += gr.vertices.size() / 4;
            }
        }
        int pages = 0, cells = 0;
        for (const AtlasPage& p : g.pages)
        {
            pages += p.surface != nullptr;
            cells += p.used;
        }
        const double f = std::max<double>(g.frames, 1);
        LOG("Ground mesh: {} sectors cached ({} groups, {} quads), {:.0f} draws per frame, {} tiles built, {} sector "
            "restarts, atlas {} pages / {} textures", g.caches.size(), groups, quads, g.draws / f, g.tilesBuilt,
            g.sectorResets, pages, cells);
        g.frames = g.draws = g.tilesBuilt = g.sectorResets = 0;
    }
}

void GroundMesh::install()
{
    if (!g_config.groundMesh)
    {
        return;
    }
    if (!g_config.ddrawD3D9)
    {
        LOG("Ground mesh: off, it needs [DDraw] Backend=d3d9");
        return;
    }
    if (!Addr::cWorldView_renderTileRow || !Addr::cWorldView_drawTileLayers || !Addr::cQuadBatcher_add ||
        !Addr::cQuadBatcher_setTexture || !Addr::renderFlags_instance || !Addr::renderFlags_set ||
        !Addr::worldState_instance || !Addr::layerRecordCache || !Addr::cTextureManager_get || !Addr::g_pTextureManager)
    {
        LOG("Ground mesh: off, game code not found");
        return;
    }
    Patch::begin();
    Patch::hook(g_origTileRow, Addr::cWorldView_renderTileRow, &hookTileRow, "cWorldView::renderTileRow (ground mesh)");
    Patch::hook(g_origLayers, Addr::cWorldView_drawTileLayers, &hookLayers, "cWorldView::drawTileLayers (ground mesh)");
    Patch::hook(g_origAdd, Addr::cQuadBatcher_add, &hookAdd, "cQuadBatcher::add");
    Patch::hook(g_origSetTexture, Addr::cQuadBatcher_setTexture, &hookSetTexture, "cQuadBatcher::setTexture");
    if (Patch::commit())
    {
        g.enabled = true;
        LOG("Ground mesh: on, the ground is drawn from vertex buffers per sector");
    }
}

bool GroundMesh::active()
{
    return g.active;
}

void GroundMesh::beginFrame(void* view, void* device)
{
    g.active = false;
    if (!g.enabled || !view)
    {
        return;
    }
    DeviceProxy* proxy = DeviceProxy::instance();
    if (!proxy || device != proxy)
    {
        return;
    }
    if (g.real != proxy->real())
    {
        // A new device: the buffers belonged to the old one.
        resetAll();
        g.real = proxy->real();
        if (!Ground::available(g.real))
        {
            disable("the device can't run its vertex shader");
            return;
        }
    }
    ++g.frame;
    g.view = view;
    g.groundRows = false;
    g.haveOffset = false;
    g.drawn = false;
    std::fill(std::begin(g.slots), std::end(g.slots), nullptr);

    // Another map (tile definitions, cell coordinates) or full atlas pages: everything is built again.
    auto* mapData = member<uint8_t*>(view, WorldView::mapData);
    auto* defs = mapData ? member<uint8_t*>(mapData, MapData::tileDefs) : nullptr;
    if (!defs)
    {
        return;
    }
    float uvs[kUvFloats];
    std::memcpy(uvs, static_cast<uint8_t*>(view) + WorldView::tileUvs, sizeof(uvs));
    if (g.startOver || mapData != g.mapData || defs != g.defs || std::memcmp(uvs, g.uvs, sizeof(uvs)) != 0)
    {
        if (g.mapData || g.startOver)
        {
            LOG("Ground mesh: {}, building the ground again", g.startOver ? "atlas full" : "new map data");
        }
        resetAll();
        g.startOver = false;
        g.mapData = mapData;
        g.defs = defs;
        std::memcpy(g.uvs, uvs, sizeof(uvs));
        if (!g.uvsChecked)
        {
            // The cells must lie inside their sheet: the atlas copies hold the sheet and a one-texel gutter.
            g.uvsChecked = true;
            for (float uv : g.uvs)
            {
                if (!(uv >= -0.01f && uv <= 1.01f))
                {
                    LOG("Ground mesh: tile cell coordinate {} outside its texture", uv);
                    disable("the tile cells don't fit the atlas");
                    return;
                }
            }
        }
    }
    // The game lights the ground through another renderer in this mode: leave the ground to it.
    if (dynamicLight())
    {
        return;
    }
    g.active = true;
    if (g_config.d3dStats)
    {
        logStats();
    }
}
