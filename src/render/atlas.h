#pragma once
#include <windows.h>
#include <objbase.h>
#include <ddraw.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

// Packs small game textures into large video-memory pages so draws using different textures can share
// one texture binding. Copies are refreshed when a texture's DirectDraw uniqueness value changes and
// dropped when the texture is destroyed (tracked through surface private data).
class TextureAtlas
{
public:
    struct Page;

    struct Slot
    {
        Page* page = nullptr;
        uint16_t x = 0, y = 0, w = 0, h = 0;   // texel rect inside the page, excluding padding
        DWORD uniqueness = 0;
        float scaleU = 1, scaleV = 1, offsetU = 0, offsetV = 0;
        uint32_t uploads = 0;
    };

    struct Page
    {
        IDirectDrawSurface7* surface = nullptr;
        DDPIXELFORMAT format = {};
        int size = 0;
        int shelfY = 0, shelfHeight = 0, cursorX = 0;
        uint32_t lastUse = 0;
        std::vector<IDirectDrawSurface7*> textures;   // slots living on this page
    };

    struct Stats
    {
        uint32_t uploads = 0;       // slot (re)uploads since last report
        uint32_t pageResets = 0;
        uint32_t rejected = 0;      // lookups of textures that can't be atlased
    };

    TextureAtlas(IDirectDraw7* ddraw, int pageSize);
    ~TextureAtlas();

    // Slot for `texture`, uploading it if needed; nullptr if it can't be atlased.
    // `beforeModify(page)` runs before an existing page region is overwritten.
    const Slot* lookup(IDirectDrawSurface7* texture, uint32_t frame, const std::function<void(Page*)>& beforeModify);

    void forget(IDirectDrawSurface7* texture);   // texture destroyed
    void checkLostPages(const std::function<void(Page*)>& beforeModify);

    size_t pageCount() const { return m_pages.size(); }
    size_t slotCount() const { return m_slots.size(); }
    Stats takeStats();

    std::recursive_mutex& mutex() { return m_mutex; }

private:
    struct Info
    {
        bool atlasable = false;
        DDPIXELFORMAT format = {};
        int width = 0, height = 0;
    };

    Info describe(IDirectDrawSurface7* texture);
    void track(IDirectDrawSurface7* texture);
    Page* allocate(const DDPIXELFORMAT& format, int w, int h, uint32_t frame, uint16_t& x, uint16_t& y,
        const std::function<void(Page*)>& beforeModify);
    Page* createPage(const DDPIXELFORMAT& format);
    void resetPage(Page* page);
    bool upload(IDirectDrawSurface7* texture, Slot& slot);

    std::recursive_mutex m_mutex;
    IDirectDraw7* m_ddraw;
    int m_pageSize;
    std::vector<std::unique_ptr<Page>> m_pages;
    std::unordered_map<IDirectDrawSurface7*, Info> m_info;
    std::unordered_map<IDirectDrawSurface7*, Slot> m_slots;
    Stats m_stats;
};
