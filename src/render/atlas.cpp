#include "render/atlas.h"
#include "game/d3d_stats.h"
#include "log.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <span>
#include <string>

namespace
{
    // Private data tag for the destruction trackers attached to game textures.
    // {5F0C2B71-8E3A-4C4B-9D12-6A1F3E7B2C90}
    constexpr GUID kTrackerGuid = {0x5f0c2b71, 0x8e3a, 0x4c4b, {0x9d, 0x12, 0x6a, 0x1f, 0x3e, 0x7b, 0x2c, 0x90}};

    // A copy is dropped for good once its texture changed this often.
    constexpr uint8_t kMaxChanges = 8;
    // A texture used with alternating addressing modes keeps its gutter after this many refills.
    constexpr uint8_t kMaxEdgeSwitches = 4;

    // Textures whose tracker was released: the surface was destroyed (or its private data replaced). Filled from
    // any thread, possibly while DirectDraw holds its own lock, so it never waits for the renderer.
    struct DestroyedQueue
    {
        std::mutex mutex;
        std::vector<std::pair<IDirectDrawSurface7*, uint32_t>> items;
        std::atomic<bool> pending{false};
    };

    DestroyedQueue& destroyedQueue()
    {
        static auto* queue = new DestroyedQueue;    // never freed: trackers can outlive the atlas
        return *queue;
    }

    std::atomic<uint32_t> g_nextTrackerId{1};

    // Attached to a texture with DDSPD_IUNKNOWNPOINTER; DirectDraw releases it when the surface goes away.
    class Tracker final : public IUnknown
    {
    public:
        Tracker(IDirectDrawSurface7* texture, uint32_t id) : m_texture(texture), m_id(id) {}

        STDMETHOD(QueryInterface)(REFIID riid, void** out) override
        {
            if (!out)
            {
                return E_POINTER;
            }
            if (riid == IID_IUnknown)
            {
                *out = this;
                AddRef();
                return S_OK;
            }
            *out = nullptr;
            return E_NOINTERFACE;
        }

        STDMETHOD_(ULONG, AddRef)() override
        {
            return static_cast<ULONG>(InterlockedIncrement(&m_refs));
        }

        STDMETHOD_(ULONG, Release)() override
        {
            const LONG refs = InterlockedDecrement(&m_refs);
            if (refs == 0)
            {
                DestroyedQueue& queue = destroyedQueue();
                {
                    std::scoped_lock lock(queue.mutex);
                    queue.items.emplace_back(m_texture, m_id);
                }
                queue.pending.store(true, std::memory_order_release);
                delete this;
            }
            return static_cast<ULONG>(refs);
        }

    private:
        IDirectDrawSurface7* m_texture;
        uint32_t m_id;
        volatile LONG m_refs = 1;
    };

    bool samePixelFormat(const DDPIXELFORMAT& a, const DDPIXELFORMAT& b)
    {
        constexpr DWORD kFlags = DDPF_RGB | DDPF_ALPHAPIXELS;
        return (a.dwFlags & kFlags) == (b.dwFlags & kFlags) && a.dwRGBBitCount == b.dwRGBBitCount &&
            a.dwRBitMask == b.dwRBitMask && a.dwGBitMask == b.dwGBitMask && a.dwBBitMask == b.dwBBitMask &&
            ((a.dwFlags & DDPF_ALPHAPIXELS) == 0 || a.dwRGBAlphaBitMask == b.dwRGBAlphaBitMask);
    }

    std::string describeFormat(const DDPIXELFORMAT& pf)
    {
        return std::format("{}bpp A{:x}R{:x}G{:x}B{:x}", pf.dwRGBBitCount,
            (pf.dwFlags & DDPF_ALPHAPIXELS) ? pf.dwRGBAlphaBitMask : 0, pf.dwRBitMask, pf.dwGBitMask, pf.dwBBitMask);
    }
}

TextureAtlas::TextureAtlas(IDirectDraw7* ddraw, const Options& options, BeforeModifyFn beforeModify, void* context)
    : m_ddraw(ddraw), m_options(options), m_beforeModify(beforeModify), m_context(context)
{
    if (m_ddraw)
    {
        m_ddraw->AddRef();
    }
    else
    {
        m_options.copies = false;
    }
}

TextureAtlas::~TextureAtlas()
{
    for (auto& page : m_pages)
    {
        if (page->surface)
        {
            page->surface->Release();
        }
    }
    if (m_ddraw)
    {
        m_ddraw->Release();
    }
}

bool TextureAtlas::destroyedPending()
{
    return destroyedQueue().pending.load(std::memory_order_acquire);
}

void TextureAtlas::drain(std::vector<IDirectDrawSurface7*>& destroyed)
{
    DestroyedQueue& queue = destroyedQueue();
    m_drained.clear();
    {
        std::scoped_lock lock(queue.mutex);
        m_drained.swap(queue.items);
        queue.pending.store(false, std::memory_order_relaxed);
    }
    for (const auto& [texture, id] : m_drained)
    {
        auto it = m_entries.find(texture);
        if (it == m_entries.end() || it->second.trackerId != id)
        {
            continue;   // never registered here, or an older tracker of a surface registered again
        }
        drop(texture, it->second);
        CacheLine& line = m_cache[(reinterpret_cast<uintptr_t>(texture) >> 4) % kCacheLines];
        if (line.texture == texture)
        {
            line = {};
        }
        m_entries.erase(it);
        destroyed.push_back(texture);
    }
}

TextureAtlas::Entry& TextureAtlas::entry(IDirectDrawSurface7* texture)
{
    CacheLine& line = m_cache[(reinterpret_cast<uintptr_t>(texture) >> 4) % kCacheLines];
    if (line.texture == texture)
    {
        return *line.entry;
    }
    auto [it, inserted] = m_entries.try_emplace(texture);
    if (inserted)
    {
        describe(texture, it->second);
    }
    line = {texture, &it->second};
    return it->second;
}

void TextureAtlas::describe(IDirectDrawSurface7* texture, Entry& e)
{
    const uint32_t id = g_nextTrackerId.fetch_add(1, std::memory_order_relaxed);
    auto* tracker = new Tracker(texture, id);
    if (SUCCEEDED(texture->SetPrivateData(kTrackerGuid, static_cast<IUnknown*>(tracker), sizeof(IUnknown*),
            DDSPD_IUNKNOWNPOINTER)))
    {
        e.trackerId = id;
    }
    tracker->Release();     // DirectDraw holds its own reference now
    if (!e.trackerId)
    {
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            LOG("Atlas: texture {} can't be tracked (SetPrivateData failed)", static_cast<void*>(texture));
        }
        return;     // a destroyed texture could not be told from a new one at the same address: no copy
    }
    if (!m_options.copies)
    {
        return;
    }

    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);
    if (FAILED(texture->GetSurfaceDesc(&desc)))
    {
        return;
    }
    const DDPIXELFORMAT& pf = desc.ddpfPixelFormat;
    constexpr DWORD kNotPlainRgb = DDPF_FOURCC | DDPF_PALETTEINDEXED1 | DDPF_PALETTEINDEXED2 | DDPF_PALETTEINDEXED4 |
        DDPF_PALETTEINDEXED8 | DDPF_PALETTEINDEXEDTO8 | DDPF_BUMPDUDV | DDPF_ZBUFFER | DDPF_LUMINANCE;
    const bool rgb = (pf.dwFlags & DDPF_RGB) && !(pf.dwFlags & kNotPlainRgb) &&
        (pf.dwRGBBitCount == 16 || pf.dwRGBBitCount == 32);
    const bool mipmapped = (desc.ddsCaps.dwCaps & DDSCAPS_MIPMAP) || ((desc.dwFlags & DDSD_MIPMAPCOUNT) && desc.dwMipMapCount > 1);
    const int w = static_cast<int>(desc.dwWidth), h = static_cast<int>(desc.dwHeight);
    const int limit = std::min(m_options.maxTextureSize, m_options.pageSize - 2);
    if (!(desc.ddsCaps.dwCaps & DDSCAPS_TEXTURE) || (desc.ddsCaps.dwCaps2 & (DDSCAPS2_CUBEMAP | DDSCAPS2_VOLUME)) ||
        mipmapped || (desc.dwFlags & DDSD_CKSRCBLT) || !rgb || w < 1 || h < 1 || w > limit || h > limit)
    {
        return;
    }
    e.format = formatIndex(pf);
    e.width = static_cast<uint16_t>(w);
    e.height = static_cast<uint16_t>(h);
    e.eligible = m_pageLimit[e.format] > 0;
}

int TextureAtlas::formatIndex(const DDPIXELFORMAT& pf)
{
    for (size_t i = 0; i < m_formats.size(); ++i)
    {
        if (samePixelFormat(m_formats[i], pf))
        {
            return static_cast<int>(i);
        }
    }
    m_formats.push_back(pf);
    m_pageLimit.push_back(m_options.maxPagesPerFormat);
    return static_cast<int>(m_formats.size() - 1);
}

bool TextureAtlas::place(IDirectDrawSurface7* texture, Entry& e, uint32_t frame, bool clampEdges)
{
    if (!e.eligible || m_failed)
    {
        return false;
    }
    if (e.page && e.checkedFrame != frame)
    {
        e.checkedFrame = frame;
        DWORD uniqueness = 0;
        texture->GetUniquenessValue(&uniqueness);
        if (uniqueness != e.uniqueness)
        {
            // The game changed the texture: refresh the copy, unless it keeps changing.
            m_beforeModify(m_context);
            if (++e.changes > kMaxChanges || !upload(texture, e, false))
            {
                drop(texture, e);
                e.eligible = false;
                return false;
            }
        }
    }
    if (e.page && e.clampEdges != clampEdges)
    {
        if (e.edgeSwitches >= kMaxEdgeSwitches)
        {
            return false;
        }
        ++e.edgeSwitches;
        m_beforeModify(m_context);
        e.clampEdges = clampEdges;
        if (!upload(texture, e, true))
        {
            drop(texture, e);
            e.eligible = false;
            return false;
        }
    }
    if (!e.page)
    {
        int x = 0, y = 0;
        Page* page = allocate(e.format, e.width + 2, e.height + 2, frame, x, y);
        if (!page)
        {
            return false;
        }
        e.page = page;
        e.x = static_cast<uint16_t>(x + 1);
        e.y = static_cast<uint16_t>(y + 1);
        e.clampEdges = clampEdges;
        page->textures.push_back(texture);
        const float inv = 1.0f / static_cast<float>(m_options.pageSize);
        e.scaleU = e.width * inv;
        e.scaleV = e.height * inv;
        e.offsetU = e.x * inv;
        e.offsetV = e.y * inv;
        e.checkedFrame = frame;
        if (!upload(texture, e, false))
        {
            drop(texture, e);
            e.eligible = false;
            return false;
        }
    }
    e.page->lastUse = frame;
    return true;
}

void TextureAtlas::beginFrame()
{
    for (auto& page : m_pages)
    {
        if (page->surface->IsLost() == DDERR_SURFACELOST)
        {
            const HRESULT hr = page->surface->Restore();
            LOG("Atlas: page {} lost, restore {:08x}", static_cast<void*>(page->surface), static_cast<uint32_t>(hr));
            resetPage(*page);
        }
    }
}

TextureAtlas::Page* TextureAtlas::allocate(int format, int w, int h, uint32_t frame, int& x, int& y)
{
    Page* lru = nullptr;
    int pages = 0;
    for (auto& page : m_pages)
    {
        if (page->format != format)
        {
            continue;
        }
        ++pages;
        if (fit(*page, w, h, x, y))
        {
            return page.get();
        }
        if (page->lastUse != frame && (!lru || page->lastUse < lru->lastUse))
        {
            lru = page.get();
        }
    }
    if (pages < m_pageLimit[format])
    {
        if (Page* page = createPage(format))
        {
            return fit(*page, w, h, x, y) ? page : nullptr;
        }
    }
    if (!lru)
    {
        return nullptr;     // every page of this format is used by the current frame
    }
    m_beforeModify(m_context);
    resetPage(*lru);
    D3DStats::count(D3DStats::CAtlasReset);
    return fit(*lru, w, h, x, y) ? lru : nullptr;
}

bool TextureAtlas::fit(Page& page, int w, int h, int& x, int& y)
{
    const int size = m_options.pageSize;
    int best = -1;
    for (size_t i = 0; i < page.shelves.size(); ++i)
    {
        const Page::Shelf& s = page.shelves[i];
        if (s.height >= h && s.x + w <= size && (best < 0 || s.height < page.shelves[best].height))
        {
            best = static_cast<int>(i);
        }
    }
    // A much taller shelf would waste its height: start a new one while there is room.
    const bool wasteful = best >= 0 && page.shelves[best].height > h + h / 2;
    if ((best < 0 || wasteful) && w <= size && page.nextY + h <= size)
    {
        page.shelves.push_back({page.nextY, h, 0});
        page.nextY += h;
        best = static_cast<int>(page.shelves.size() - 1);
    }
    if (best < 0)
    {
        return false;
    }
    Page::Shelf& shelf = page.shelves[best];
    x = shelf.x;
    y = shelf.y;
    shelf.x += w;
    return true;
}

TextureAtlas::Page* TextureAtlas::createPage(int format)
{
    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    desc.dwWidth = static_cast<DWORD>(m_options.pageSize);
    desc.dwHeight = static_cast<DWORD>(m_options.pageSize);
    desc.ddpfPixelFormat = m_formats[format];
    desc.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY;
    IDirectDrawSurface7* surface = nullptr;
    const HRESULT hr = m_ddraw->CreateSurface(&desc, &surface, nullptr);
    int pages = 0;
    for (auto& page : m_pages)
    {
        pages += page->format == format;
    }
    if (FAILED(hr) || !surface)
    {
        LOG("Atlas: creating a {}x{} {} page failed ({:08x}); {} page(s) for this format", m_options.pageSize,
            m_options.pageSize, describeFormat(m_formats[format]), static_cast<uint32_t>(hr), pages);
        m_pageLimit[format] = pages;
        return nullptr;
    }
    auto page = std::make_unique<Page>();
    page->surface = surface;
    page->format = format;
    LOG("Atlas: page {} ({}x{} {}), {} in total", pages + 1, m_options.pageSize, m_options.pageSize,
        describeFormat(m_formats[format]), m_pages.size() + 1);
    m_pages.push_back(std::move(page));
    return m_pages.back().get();
}

void TextureAtlas::resetPage(Page& page)
{
    for (IDirectDrawSurface7* texture : page.textures)
    {
        auto it = m_entries.find(texture);
        if (it != m_entries.end())
        {
            it->second.page = nullptr;
        }
    }
    page.textures.clear();
    page.shelves.clear();
    page.nextY = 0;
}

void TextureAtlas::drop(IDirectDrawSurface7* texture, Entry& e)
{
    if (!e.page)
    {
        return;
    }
    auto& list = e.page->textures;
    auto it = std::find(list.begin(), list.end(), texture);
    if (it != list.end())
    {
        *it = list.back();
        list.pop_back();
    }
    e.page = nullptr;   // the space stays unused until the page is reset
}

bool TextureAtlas::upload(IDirectDrawSurface7* texture, Entry& e, bool gutterOnly)
{
    if (!gutterOnly)
    {
        texture->GetUniquenessValue(&e.uniqueness);
    }
    const LONG x = e.x, y = e.y, w = e.width, h = e.height;
    // Source column/row for the left/top and right/bottom gutter: the opposite edge (wrap) or the same edge.
    const LONG left = e.clampEdges ? 0 : w - 1, right = e.clampEdges ? w - 1 : 0;
    const LONG top = e.clampEdges ? 0 : h - 1, bottom = e.clampEdges ? h - 1 : 0;
    struct Copy
    {
        LONG dx, dy, sx, sy, cw, ch;
    };
    const Copy copies[] = {
        {x - 1, y, left, 0, 1, h}, {x + w, y, right, 0, 1, h},
        {x, y - 1, 0, top, w, 1}, {x, y + h, 0, bottom, w, 1},
        {x - 1, y - 1, left, top, 1, 1}, {x + w, y - 1, right, top, 1, 1},
        {x - 1, y + h, left, bottom, 1, 1}, {x + w, y + h, right, bottom, 1, 1},
        {x, y, 0, 0, w, h},
    };
    // The gutter first, the texture itself last (skipped when only the addressing mode changed).
    for (const Copy& c : std::span<const Copy>(copies, gutterOnly ? 8 : 9))
    {
        RECT dst = {c.dx, c.dy, c.dx + c.cw, c.dy + c.ch};
        RECT src = {c.sx, c.sy, c.sx + c.cw, c.sy + c.ch};
        const HRESULT hr = e.page->surface->Blt(&dst, texture, &src, DDBLT_WAIT, nullptr);
        if (FAILED(hr))
        {
            ++m_uploadFailures;
            if (m_uploadFailures <= 8)
            {
                LOG("Atlas: copying texture {} ({}x{}) failed: {:08x}", static_cast<void*>(texture), w, h,
                    static_cast<uint32_t>(hr));
            }
            if (m_uploads == 0 && m_uploadFailures >= 4)
            {
                m_failed = true;
                LOG("Atlas: copies don't work here, batching continues without them");
            }
            return false;
        }
    }
    ++m_uploads;
    D3DStats::count(D3DStats::CAtlasUpload);
    return true;
}
