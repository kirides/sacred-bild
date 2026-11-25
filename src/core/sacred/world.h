#pragma once
#include "game/sacred_addr.h"
#include "sacred/render.h"

#include <windows.h>
#include <cstddef>
#include <cstdint>

struct IDirect3DDevice7;

// The world view: the isometric world drawn on the screen (cWorldView; its subclass cWorldView0 is the one in game),
// and the map data it draws the ground from.
//
// Ground: renderTileRow draws each tile's base through the view's quad batcher and collects tiles with blend layers
// into a fixed array (at most layeredTileCapacity per frame); after all rows cWorldView0::render flushes the batcher
// and drawTileLayers draws the collected layers. A tile's quad has the corners left, top, bottom, right at
// (-48.2, 0), (0, -24.2), (0, 24.2), (48.2, 0) around its position, each raised by its height; tile (row r, column c)
// of the 192x192 loaded tiles lies at (48 (c - r), 24 (c + r)) in view coordinates, plus one offset per frame.
namespace Sacred
{
    // A position in the world, as screenToWorld gives it.
    struct WorldPoint
    {
        uint16_t _00;
        uint8_t _02[2];
        int32_t x, y;
        uint8_t layer;
        uint8_t _0d[3];
    };
    static_assert(sizeof(WorldPoint) == 0x10 && offsetof(WorldPoint, x) == 0x04 && offsetof(WorldPoint, layer) == 0x0C);

    // An entry of the world view's pick list: an object's rect on the screen (physical pixels). Ids with bit 31 set
    // are not objects (the picker ranks them separately).
    struct PickEntry
    {
        uint32_t id;
        int32_t x, y;               // left, top
        int16_t width, height;
        uint8_t _10[0x1C - 0x10];

        float centerX() const { return x + width * 0.5f; }
        float centerY() const { return y + height * 0.5f; }
    };
    static_assert(sizeof(PickEntry) == 0x1C && offsetof(PickEntry, width) == 0x0C);

    // A tile of a sector.
    struct Tile
    {
        int32_t def;                // tile definition index (cMapData::tileDefs)
        uint8_t _04[0x0C - 0x04];
        uint32_t layers;            // first blend layer record id (cMapData::layerRecord), 0: none
        // Pixels up (signed) and light (gray, alpha 255) of the corners, in the order left, top, right, bottom.
        int8_t heights[4];
        uint8_t light[4];
        uint8_t _18[0x20 - 0x18];
    };
    static_assert(sizeof(Tile) == 0x20 && offsetof(Tile, layers) == 0x0C && offsetof(Tile, heights) == 0x10);
    static_assert(offsetof(Tile, light) == 0x14);

    struct TileDef
    {
        uint8_t _00[0x20];
        uint32_t texture;           // texture manager handle
        uint16_t uvCell;            // cell of cWorldView::tileUvs
        uint8_t _26[0x40 - 0x26];
    };
    static_assert(sizeof(TileDef) == 0x40 && offsetof(TileDef, texture) == 0x20 && offsetof(TileDef, uvCell) == 0x24);

    // A blend layer record. drawTileLayers draws a tile's chain in passes: two-texture records with render flag
    // 0x2000 off, one-texture records with it on, alternating until every chain is done; each pass draws its records
    // up to the first one of the other kind.
    struct LayerRecord
    {
        uint8_t _00[0x04];
        // Low 17 bits: stage 0 tile definition; high 15 bits: stage 1 tile definition, 0 = a one-texture record.
        uint32_t defs;
        uint8_t _08[0x0C - 0x08];
        uint32_t next;              // next record id, 0: end of the chain
    };
    static_assert(sizeof(LayerRecord) == 0x10);
    static_assert(offsetof(LayerRecord, defs) == 0x04 && offsetof(LayerRecord, next) == 0x0C);

    // One of the 3x3 loaded sectors around the camera (64x64 tiles). The sector objects are reused for other parts of
    // the map as the camera moves.
    struct Sector
    {
        static constexpr int size = 64;

        uint8_t _00[0x44];
        int32_t originX;            // world tile column of its tile 0
        int32_t originY;            // world tile row of its tile 0
        uint8_t _4c[0x6C - 0x4C];
        Tile* tiles;                // size x size, row * size + column
    };
    static_assert(offsetof(Sector, originX) == 0x44 && offsetof(Sector, originY) == 0x48);
    static_assert(offsetof(Sector, tiles) == 0x6C);

    // A std::map<uint32_t, record> of the map data's record caches (as the exe's std::map lays it out).
    struct RecordMap
    {
        uint8_t* head;              // the head node, also end(); a node has the key at +0x10, the record at +0x14
        uint32_t size;
    };

    // The map data (constructor ENG 006335F0, clear 00633E80): tile definitions and record caches. Each record cache
    // is thiscall (id) -> record, 0 if id is 0. It looks the id up in its own RecordMap (Addr::recordMapFind),
    // stamps the node with the current time (Addr::g_recordStamp) on a hit, and on a miss reads the record from a
    // data file, inserts it and may evict the node with the oldest stamp.
    struct cMapData
    {
        uint8_t _00[0x258];
        TileDef* tileDefs;          // indexed by Tile::def
        uint8_t _25c[0xDE50 - 0x25C];
        RecordMap records;          // recordCache's: the 64-byte records also used by game logic (stamp: node +0x54)
        uint8_t _de58[0xDE5C - 0xDE58];
        RecordMap layerRecords;     // layerRecordCache's (stamp: node +0x24)

        LayerRecord* layerRecord(uint32_t id) { return Addr::layerRecordCache(this, id); }
    };
    static_assert(offsetof(cMapData, tileDefs) == 0x258);
    static_assert(offsetof(cMapData, records) == 0xDE50 && offsetof(cMapData, layerRecords) == 0xDE5C);

    // A world singleton (0x12E6C bytes, made on first use; also holds the view shake offsets at +0x9DC0).
    struct WorldState
    {
        uint8_t _00[0x10];
        uint8_t flags;              // 0x20: the ground is lit through cTileRenderer
        uint8_t _11[0x9E0C - 0x11];
        uint8_t tileRendererLight;  // set: the same

        // renderTileRow lights the ground through cTileRenderer instead of the tiles' own corner light then.
        bool tileRendererLit() const { return (flags & 0x20) || tileRendererLight; }

        static WorldState* instance() { return Addr::worldState_instance(); }
    };
    static_assert(offsetof(WorldState, flags) == 0x10 && offsetof(WorldState, tileRendererLight) == 0x9E0C);

    // Where a row of the row walk starts: initRowWalk sets the even and odd rows from the view's top-left corner;
    // cWorldView0::render then calls renderTileRow for each and steps it one tile row down (tile + 0x41, y + 48),
    // changing sector at sector edges.
    struct RowPos
    {
        float x, y;
        int16_t tile;               // row * 64 + column within the sector
        int16_t sector;             // 3x3 grid of loaded sectors around the camera: row * 3 + column
        int16_t steps;              // rows until the walk leaves the sector (cWorldView0::render only)
        int16_t _0e;
    };
    static_assert(sizeof(RowPos) == 0x10);

    // An animated (water/lava, record type 0x90/0xA0) tile as renderTileRow collects them for drawWaterTiles.
    struct WaterTile
    {
        uint8_t _00[0x98];
    };

    struct cWorldView
    {
        struct Vtable
        {
            void* _00[6];
            // Physical screen pixels to the world position under them.
            void(__fastcall* screenToWorld)(cWorldView* self, void* edx, int x, int y, WorldPoint* out);
        };

        static constexpr uint32_t tileUvCells = 18;
        static constexpr uint32_t layeredTileCapacity = 0x6D5;
        static constexpr uint32_t waterTileCapacity = 1750;
        // drawWaterTiles ends by setting the water ambience: calls at these offsets into it to a thiscall on the sound
        // system (ENG 00690690: water tile count (16 bit), their average position x, y; ret 0xC). The count is the
        // whole list's, the position the average of the tiles drawn (0 when there were none).
        static constexpr uintptr_t drawWaterTilesAmbienceCalls[] = {0x378, 0x393, 0x6FD};

        const Vtable* vtable;
        cMapData* mapData;
        uint8_t _08[0x830 - 0x08];
        // Texture coordinates of the 18 tile cells of a 256x256 ground sheet: per cell 4 corners (left, top, bottom,
        // right) of (u, v). Base tiles use their definition's cell, blend layers the definition index % 18.
        float tileUvs[tileUvCells][8];
        uint8_t _a70[0xB60 - 0xA70];
        float zoom;                 // 0.5 .. 2.0
        uint8_t _b64[0x3FF1C - 0xB64];
        uint32_t layeredTileCount;
        uint8_t _3ff20[0x3FF2C - 0x3FF20];
        // Appended by renderTileRow without a bounds check: one more entry would overwrite the count.
        WaterTile waterTiles[waterTileCapacity];
        uint32_t waterTileCount;
        uint8_t _80e40[0x86890 - 0x80E40];
        cQuadBatcher quadBatcher;   // the ground's and the blend layers' quads
        uint8_t _8c1b8[0x96AB8 - 0x86890 - sizeof(cQuadBatcher)];
        // renderTileRow draws ground from column rowEdgeLeft - 1 to rowLength - rowEdgeRight + 1 (rest: margins).
        int32_t rowEdgeLeft;        // 6
        int32_t rowEdgeRight;       // 6
        uint8_t _96ac0[0x96AC8 - 0x96AC0];
        int32_t rowLength;          // tiles per row: view width / 96 + 12
        uint8_t _96acc[0x96B0C - 0x96ACC];
        // What can be picked on the screen, rebuilt by the world renderer every frame: a std::vector guarded by
        // pickLock. The world pick (Addr::worldPick) gives up on more than 1000 entries.
        CRITICAL_SECTION pickLock;
        PickEntry* pickBegin;
        PickEntry* pickEnd;
        uint8_t _96b2c[0x970B0 - 0x96B2C];
        Sector* sectors[9];         // the 3x3 loaded around the camera, row * 3 + column
        RowPos rowEven;
        RowPos rowOdd;              // even + (48, 24): one tile column further

        void screenToWorld(int x, int y, WorldPoint& out) { vtable->screenToWorld(this, nullptr, x, y, &out); }

        // The object id under (x, y) (physical pixels) ranked as the cursor does, 0 for none; the hit's rect goes to
        // rect (x, y, width | height << 16) if given.
        uint32_t pick(int x, int y, uint32_t excludeId, int32_t* rect)
        {
            return Addr::worldPick(this, x, y, excludeId, rect);
        }

        void drawTileLayers(IDirect3DDevice7* device) { Addr::cWorldView_drawTileLayers(this, device); }
        void drawWaterTiles(IDirect3DDevice7* device) { Addr::cWorldView_drawWaterTiles(this, device); }
    };
    static_assert(offsetof(cWorldView::Vtable, screenToWorld) == 0x18);
    static_assert(offsetof(cWorldView, mapData) == 0x04 && offsetof(cWorldView, tileUvs) == 0x830);
    static_assert(offsetof(cWorldView, zoom) == 0xB60 && offsetof(cWorldView, layeredTileCount) == 0x3FF1C);
    static_assert(offsetof(cWorldView, waterTiles) == 0x3FF2C && offsetof(cWorldView, waterTileCount) == 0x80E3C);
    static_assert(offsetof(cWorldView, quadBatcher) == 0x86890);
    static_assert(offsetof(cWorldView, rowEdgeLeft) == 0x96AB8 && offsetof(cWorldView, rowEdgeRight) == 0x96ABC);
    static_assert(offsetof(cWorldView, rowLength) == 0x96AC8);
    static_assert(offsetof(cWorldView, pickLock) == 0x96B0C);
    static_assert(offsetof(cWorldView, pickBegin) == 0x96B24 && offsetof(cWorldView, pickEnd) == 0x96B28);
    static_assert(offsetof(cWorldView, sectors) == 0x970B0);
    static_assert(offsetof(cWorldView, rowEven) == 0x970D4 && offsetof(cWorldView, rowOdd) == 0x970E4);
}
