#pragma once
#include <windows.h>
#include <objbase.h>
#include <ddraw.h>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

// Registry of the game's textures, plus copies of small ones packed into large video-memory pages so draws
// that used different textures can share one texture binding. Destroyed textures are noticed through surface
// private data; a copy is refreshed when its texture's DirectDraw uniqueness value changes.
//
// Each copy has a one-texel gutter, filled from the opposite edge for wrap addressing or by repeating the
// edge for clamp and mirror addressing, so point and bilinear sampling of texture coordinates in
// [-0.5, size + 0.5] texels reads exactly what the original texture would.
class TextureAtlas
{
public:
    struct Page
    {
        struct Shelf
        {
            int y = 0, height = 0, x = 0;
        };

        IDirectDrawSurface7* surface = nullptr;
        int format = 0;
        int size = 0;                                   // texels per side
        std::vector<Shelf> shelves;
        int nextY = 0;
        uint32_t lastUse = 0;
        std::vector<IDirectDrawSurface7*> textures;     // textures with a copy on this page
    };

    struct Entry
    {
        uint32_t trackerId = 0;     // 0: destruction is not tracked
        bool eligible = false;      // may be copied into a page
        int format = -1;
        uint16_t width = 0, height = 0;
        Page* page = nullptr;       // current copy, if any
        uint16_t x = 0, y = 0;      // copy origin inside the page (gutter excluded)
        float scaleU = 1, scaleV = 1, offsetU = 0, offsetV = 0;     // texture -> page coordinates
        DWORD uniqueness = 0;       // of the texture when copied
        uint32_t checkedFrame = 0;
        uint8_t changes = 0;        // content changes seen; frequently changing textures are not copied
        bool clampEdges = false;    // gutter repeats the edges (clamp/mirror) instead of wrapping
        uint8_t edgeSwitches = 0;   // gutter refills for another addressing mode
    };

    // Runs before a page region that pending draws may still use is overwritten.
    using BeforeModifyFn = void (*)(void* context);

    struct Options
    {
        bool copies = true;         // false: registry only
        int pageSize = 4096;        // halved (down to 1024) when the device refuses a page
        int maxPagesPerFormat = 4;
        int maxTextureSize = 512;
    };

    TextureAtlas(IDirectDraw7* ddraw, const Options& options, BeforeModifyFn beforeModify, void* context);
    ~TextureAtlas();
    TextureAtlas(const TextureAtlas&) = delete;
    TextureAtlas& operator=(const TextureAtlas&) = delete;

    // True if a texture seen before may have been destroyed since the last drain().
    static bool destroyedPending();
    // Forgets destroyed textures and appends them to `destroyed`.
    void drain(std::vector<IDirectDrawSurface7*>& destroyed);

    // Registers the texture on first use. The entry stays valid until the texture is destroyed and drained.
    Entry& entry(IDirectDrawSurface7* texture);

    // Makes sure the texture has an up-to-date copy in a page whose gutter suits the addressing mode (wrap, or
    // clamp/mirror with `clampEdges`); false if it can't have one now.
    bool place(IDirectDrawSurface7* texture, Entry& entry, uint32_t frame, bool clampEdges);

    // Once per frame before use: restores lost pages.
    void beginFrame();

    int pageCount() const { return static_cast<int>(m_pages.size()); }
    bool isPage(IDirectDrawSurface7* surface) const;

private:
    void describe(IDirectDrawSurface7* texture, Entry& entry);
    int formatIndex(const DDPIXELFORMAT& pf);
    Page* allocate(int format, int w, int h, uint32_t frame, int& x, int& y);
    bool fit(Page& page, int w, int h, int& x, int& y);
    Page* createPage(int format);
    void resetPage(Page& page);
    void drop(IDirectDrawSurface7* texture, Entry& entry);
    bool upload(IDirectDrawSurface7* texture, Entry& entry, bool gutterOnly);

    IDirectDraw7* m_ddraw;
    Options m_options;
    BeforeModifyFn m_beforeModify;
    void* m_context;
    bool m_failed = false;          // copying into pages does not work: no more copies
    uint32_t m_uploads = 0, m_uploadFailures = 0;
    std::vector<DDPIXELFORMAT> m_formats;
    std::vector<int> m_pageLimit;   // per format; lowered when page creation fails
    std::vector<std::unique_ptr<Page>> m_pages;
    std::unordered_map<IDirectDrawSurface7*, Entry> m_entries;

    // Direct-mapped lookup cache in front of m_entries (entry addresses are stable until erased).
    struct CacheLine
    {
        IDirectDrawSurface7* texture = nullptr;
        Entry* entry = nullptr;
    };
    static constexpr size_t kCacheLines = 512;
    CacheLine m_cache[kCacheLines];
    std::vector<std::pair<IDirectDrawSurface7*, uint32_t>> m_drained;
};
