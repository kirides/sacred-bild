#include "game/d3d_stats.h"
#include "log.h"

#include <windows.h>
#include <objbase.h>
#include <ddraw.h>
#include <intrin.h>
#include <psapi.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    struct TextureInfo
    {
        uint32_t width = 0;
        uint32_t height = 0;
        std::string format;
        bool colorKey = false;
        uint32_t uses = 0;          // SetTexture switches to it
    };

    std::mutex g_detailMutex;
    std::unordered_set<void*> g_frameTextures;
    std::unordered_map<void*, TextureInfo> g_textures;
    std::unordered_map<uint64_t, uint32_t> g_drawKinds;     // type | fvf | vertex count | indexed
    std::unordered_map<uint64_t, uint32_t> g_stateValues;   // state << 32 | value
    std::unordered_map<uint64_t, uint32_t> g_stageValues;   // stage << 48 | type << 32 | value
    std::unordered_map<uint32_t, uint32_t> g_flushCauses;   // see onFlushCause
    int g_reportsSinceDetail = 0;
    // Detail is collected in the first frame after each report only: the maps are too slow for every call.
    std::atomic<bool> g_detailFrame{false};

    std::string describeFormat(const DDPIXELFORMAT& pf)
    {
        if (pf.dwFlags & DDPF_FOURCC)
        {
            const char cc[5] = {char(pf.dwFourCC), char(pf.dwFourCC >> 8), char(pf.dwFourCC >> 16), char(pf.dwFourCC >> 24), 0};
            return cc;
        }
        return Fmt::format("{}bpp A{:x}R{:x}G{:x}B{:x}", pf.dwRGBBitCount, (pf.dwFlags & DDPF_ALPHAPIXELS) ? pf.dwRGBAlphaBitMask : 0,
            pf.dwRBitMask, pf.dwGBitMask, pf.dwBBitMask);
    }

    template <class Map>
    std::vector<std::pair<typename Map::key_type, uint32_t>> topN(const Map& m, size_t n)
    {
        std::vector<std::pair<typename Map::key_type, uint32_t>> v(m.begin(), m.end());
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        if (v.size() > n) v.resize(n);
        return v;
    }

    void logDetail()
    {
        static constexpr uint32_t kBuckets[] = {16, 32, 64, 128, 256, 512, 1024, 0xFFFFFFFF};
        uint32_t sizes[std::size(kBuckets)] = {};
        std::unordered_map<std::string, uint32_t> formats;
        uint32_t colorKeyed = 0;
        uint64_t pixels = 0;
        for (const auto& [surface, t] : g_textures)
        {
            const uint32_t side = std::max(t.width, t.height);
            for (size_t i = 0; i < std::size(kBuckets); ++i)
            {
                if (side <= kBuckets[i])
                {
                    ++sizes[i];
                    break;
                }
            }
            ++formats[t.format];
            colorKeyed += t.colorKey;
            pixels += uint64_t(t.width) * t.height;
        }
        LOG("Detail: textures seen {} ({:.1f} Mpixel, {} color-keyed) | max side <=16:{} <=32:{} <=64:{} <=128:{} <=256:{} "
            "<=512:{} <=1024:{} larger:{}", g_textures.size(), pixels / 1e6, colorKeyed, sizes[0], sizes[1], sizes[2],
            sizes[3], sizes[4], sizes[5], sizes[6], sizes[7]);
        for (const auto& [fmt, n] : topN(formats, 8))
        {
            LOG("Detail:   format {:<28} {}", fmt, n);
        }
        for (const auto& [key, n] : topN(g_drawKinds, 12))
        {
            LOG("Detail:   draw type {} fvf {:03x} verts {:<5} {} x{}", key >> 56, (key >> 32) & 0xFFFFFF,
                (key >> 1) & 0x7FFFFFFF, (key & 1) ? "indexed" : "", n);
        }
        for (const auto& [key, n] : topN(g_stateValues, 24))
        {
            LOG("Detail:   rs {:>3} = {:08x} x{}", key >> 32, uint32_t(key), n);
        }
        for (const auto& [key, n] : topN(g_stageValues, 16))
        {
            LOG("Detail:   tss {} {:>2} = {:08x} x{}", key >> 48, (key >> 32) & 0xFFFF, uint32_t(key), n);
        }
        for (const auto& [key, n] : topN(g_flushCauses, 12))
        {
            if (key & 0x10000)
            {
                LOG("Detail:   batch ended by tss {} {:>2} x{}", (key >> 8) & 0xFF, key & 0xFF, n);
            }
            else
            {
                LOG("Detail:   batch ended by rs {:>3} x{}", key, n);
            }
        }
    }

    auto& g_counters = D3DStats::Detail::counters;
    auto& g_times = D3DStats::Detail::times;

    HANDLE g_renderThread = nullptr;
    unsigned long g_renderThreadId = 0;

    // TSC <-> QPC calibration, refined at every report.
    int64_t g_tscStart = 0;
    int64_t g_qpcStart = 0;
    int64_t g_qpcFreq = 1;

    int64_t g_reportQpc = 0;
    uint32_t g_frames = 0;

    // Frame times: a frame much slower than the running average is a hitch, logged with what it did.
    double g_avgFrameMs = 0.0;
    double g_maxFrameMs = 0.0;
    uint32_t g_hitches = 0;
    uint32_t g_hitchLogs = 0;
    uint32_t g_frameCounters[D3DStats::CounterCount] = {};     // counters at the end of the previous frame
    int64_t g_frameTimes[D3DStats::TimerCount] = {};
    int64_t g_lastFrameTsc = 0;
    uint64_t g_lastFrameCycles = 0;     // the render thread's CPU cycles at the previous frame
    D3DStats::Timer g_markTimer = D3DStats::TimerCount;
    int64_t g_markStart = 0;
    uint64_t g_lastThreadCpu = 0;

    // World view passes (render thread only): what the current pass has used since it was entered.
    struct PassTotals
    {
        int64_t ticks = 0, proxyTicks = 0;
        uint32_t draws = 0, submits = 0;
    };
    D3DStats::Pass g_pass = D3DStats::PNone;
    int64_t g_passStart = 0, g_passProxy = 0;
    uint32_t g_passDraws = 0, g_passSubmits = 0;
    PassTotals g_passTotals[D3DStats::PassCount];

    uint32_t gameDraws()
    {
        return g_counters[D3DStats::CDraw].load(std::memory_order_relaxed) +
            g_counters[D3DStats::CDrawIndexed].load(std::memory_order_relaxed) +
            g_counters[D3DStats::CDrawVB].load(std::memory_order_relaxed);
    }

    int64_t qpc()
    {
        LARGE_INTEGER v;
        QueryPerformanceCounter(&v);
        return v.QuadPart;
    }

    uint64_t threadCpu100ns()
    {
        FILETIME c, e, k, u;
        if (!g_renderThread || !GetThreadTimes(g_renderThread, &c, &e, &k, &u))
        {
            return 0;
        }
        return (static_cast<uint64_t>(k.dwHighDateTime) << 32 | k.dwLowDateTime) +
            (static_cast<uint64_t>(u.dwHighDateTime) << 32 | u.dwLowDateTime);
    }
}

std::string D3DStats::memorySummary()
{
    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    size_t used = 0, largestFree = 0;
    MEMORY_BASIC_INFORMATION mbi = {};
    for (auto* p = static_cast<uint8_t*>(nullptr) + 0x10000; VirtualQuery(p, &mbi, sizeof(mbi)); p += mbi.RegionSize)
    {
        if (mbi.State == MEM_FREE)
        {
            largestFree = std::max<size_t>(largestFree, mbi.RegionSize);
        }
        else
        {
            used += mbi.RegionSize;
        }
        if (mbi.RegionSize == 0)
        {
            break;
        }
    }
    return Fmt::format("private {} MB, address space {} MB used, largest free {} MB", pmc.PrivateUsage >> 20,
        used >> 20, largestFree >> 20);
}

void D3DStats::onTexture(void* surface)
{
    if (!surface || !g_detailFrame.load(std::memory_order_relaxed))
    {
        return;
    }
    std::scoped_lock lock(g_detailMutex);
    if (g_frameTextures.insert(surface).second)
    {
        count(CUniqueTex);
    }
    auto [it, inserted] = g_textures.try_emplace(surface);
    ++it->second.uses;
    if (inserted)
    {
        DDSURFACEDESC2 desc = {};
        desc.dwSize = sizeof(desc);
        if (SUCCEEDED(static_cast<IDirectDrawSurface7*>(surface)->GetSurfaceDesc(&desc)))
        {
            it->second.width = desc.dwWidth;
            it->second.height = desc.dwHeight;
            it->second.format = describeFormat(desc.ddpfPixelFormat);
            it->second.colorKey = (desc.dwFlags & DDSD_CKSRCBLT) != 0;
        }
    }
}

void D3DStats::onDraw(uint32_t primitiveType, uint32_t fvf, uint32_t vertexCount, bool indexed)
{
    if (!g_detailFrame.load(std::memory_order_relaxed))
    {
        return;
    }
    const uint64_t key = (uint64_t(primitiveType) << 56) | (uint64_t(fvf & 0xFFFFFF) << 32) |
        (uint64_t(vertexCount & 0x7FFFFFFF) << 1) | (indexed ? 1 : 0);
    std::scoped_lock lock(g_detailMutex);
    ++g_drawKinds[key];
}

void D3DStats::onRenderState(uint32_t state, uint32_t value)
{
    if (!g_detailFrame.load(std::memory_order_relaxed))
    {
        return;
    }
    std::scoped_lock lock(g_detailMutex);
    ++g_stateValues[(uint64_t(state) << 32) | value];
}

void D3DStats::onStageState(uint32_t stage, uint32_t type, uint32_t value)
{
    if (!g_detailFrame.load(std::memory_order_relaxed))
    {
        return;
    }
    std::scoped_lock lock(g_detailMutex);
    ++g_stageValues[(uint64_t(stage) << 48) | (uint64_t(type) << 32) | value];
}

void D3DStats::onFlushCause(uint32_t key)
{
    if (!g_detailFrame.load(std::memory_order_relaxed))
    {
        return;
    }
    std::scoped_lock lock(g_detailMutex);
    ++g_flushCauses[key];
}

D3DStats::Pass D3DStats::currentPass()
{
    return g_pass;
}

D3DStats::Pass D3DStats::enterPass(Pass pass)
{
    const Pass previous = g_pass;
    if (pass == previous || (previous == PNone && pass != PWorld))
    {
        return previous;
    }
    const int64_t t = now();
    const int64_t proxy = total(TProxy);
    const uint32_t draws = gameDraws();
    const uint32_t submits = g_counters[CSubmit].load(std::memory_order_relaxed);
    if (previous != PNone)
    {
        PassTotals& p = g_passTotals[previous];
        p.ticks += t - g_passStart;
        p.proxyTicks += proxy - g_passProxy;
        p.draws += draws - g_passDraws;
        p.submits += submits - g_passSubmits;
    }
    g_pass = pass;
    g_passStart = t;
    g_passProxy = proxy;
    g_passDraws = draws;
    g_passSubmits = submits;
    return previous;
}

void D3DStats::setRenderThread(unsigned long threadId)
{
    if (threadId == g_renderThreadId)
    {
        return;
    }
    if (g_renderThread)
    {
        CloseHandle(g_renderThread);
    }
    g_renderThreadId = threadId;
    g_renderThread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, threadId);
    g_lastThreadCpu = threadCpu100ns();
}

void D3DStats::mark(Timer next)
{
    if (!Detail::timing)
    {
        return;
    }
    const int64_t t = now();
    if (g_markTimer != TimerCount)
    {
        addTime(g_markTimer, t - g_markStart);
    }
    g_markTimer = next;
    g_markStart = t;
}

bool D3DStats::isRenderThread()
{
    return GetCurrentThreadId() == g_renderThreadId;
}

void D3DStats::onFrame()
{
    const int64_t t = now();
    const int64_t q = qpc();
    if (!g_qpcStart)
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpcFreq = f.QuadPart;
        g_qpcStart = g_reportQpc = q;
        g_tscStart = g_lastFrameTsc = t;
        return;
    }
    const int64_t frameTicks = t - g_lastFrameTsc;
    addTime(TFrame, frameTicks);
    g_lastFrameTsc = t;
    ++g_frames;
    {
        const double tscPerMs = static_cast<double>(t - g_tscStart) * g_qpcFreq / (1000.0 * static_cast<double>(q - g_qpcStart));
        const double frameMs = static_cast<double>(frameTicks) / tscPerMs;
        // CPU time the thread got this frame (cycles at about the TSC rate): far below the frame time = it waited or
        // was not scheduled.
        ULONG64 cycles = 0;
        QueryThreadCycleTime(GetCurrentThread(), &cycles);
        const double ranMs = g_lastFrameCycles ? static_cast<double>(cycles - g_lastFrameCycles) / tscPerMs : 0.0;
        g_lastFrameCycles = cycles;
        uint32_t dc[CounterCount];
        double dms[TimerCount];
        for (int i = 0; i < CounterCount; ++i)
        {
            const uint32_t v = g_counters[i].load(std::memory_order_relaxed);
            dc[i] = v - g_frameCounters[i];
            g_frameCounters[i] = v;
        }
        for (int i = 0; i < TimerCount; ++i)
        {
            const int64_t v = g_times[i].load(std::memory_order_relaxed);
            dms[i] = static_cast<double>(v - g_frameTimes[i]) / tscPerMs;
            g_frameTimes[i] = v;
        }
        g_maxFrameMs = std::max(g_maxFrameMs, frameMs);
        if (g_avgFrameMs > 0.0 && frameMs > std::max(2.0 * g_avgFrameMs, g_avgFrameMs + 8.0))
        {
            ++g_hitches;
            if (g_hitchLogs++ < 5)
            {
                LOG("Hitch: {:.1f} ms (average {:.1f}), thread ran {:.1f} | world {:.1f} ui {:.1f} flip {:.1f} lockBack {:.1f} "
                    "sound lock {:.1f} animation wait {:.1f} | outside: own {:.1f} game before world {:.1f} after world {:.1f} "
                    "after ui {:.1f}, device calls outside the world view {:.1f} | atlas uploads {} page resets {} | record file reads {} | textures loaded {} KB | draws {} "
                    "submitted {}",
                    frameMs, g_avgFrameMs, ranMs, dms[TWorld], dms[TUi], dms[TFlip], dms[TLockBack], dms[TSoundWait],
                    dms[TAnimationWait], dms[TOwnAfterFlip], dms[TGameBeforeWorld], dms[TGameAfterWorld], dms[TGameAfterUi],
                    dms[TProxy] - dms[TWorldProxy], dc[CAtlasUpload], dc[CAtlasReset], dc[CRecordRead], dc[CTextureKB], dc[CDraw] + dc[CDrawIndexed] + dc[CDrawVB],
                    dc[CSubmit]);
            }
        }
        // Slow average: a single hitch barely moves it, a new steady frame rate takes over within a second.
        g_avgFrameMs = g_avgFrameMs > 0.0 ? g_avgFrameMs * 0.95 + std::min(frameMs, 4.0 * g_avgFrameMs) * 0.05 : frameMs;
    }
    if (g_detailFrame.exchange(false, std::memory_order_relaxed))
    {
        std::scoped_lock lock(g_detailMutex);
        g_frameTextures.clear();
    }

    if (q - g_reportQpc < g_qpcFreq)
    {
        return;
    }

    const double tscPerMs = static_cast<double>(t - g_tscStart) * g_qpcFreq / (1000.0 * static_cast<double>(q - g_qpcStart));
    const double wallMs = 1000.0 * static_cast<double>(q - g_reportQpc) / g_qpcFreq;
    const double frames = g_frames;

    uint32_t c[CounterCount];
    for (int i = 0; i < CounterCount; ++i) c[i] = g_counters[i].exchange(0, std::memory_order_relaxed);
    double ms[TimerCount];
    for (int i = 0; i < TimerCount; ++i)
    {
        ms[i] = static_cast<double>(g_times[i].exchange(0, std::memory_order_relaxed)) / tscPerMs / frames;
    }
    std::fill(std::begin(g_frameCounters), std::end(g_frameCounters), 0u);
    std::fill(std::begin(g_frameTimes), std::end(g_frameTimes), int64_t(0));
    const double maxFrameMs = g_maxFrameMs;
    const uint32_t hitches = g_hitches;
    g_maxFrameMs = 0.0;
    g_hitches = 0;
    g_hitchLogs = 0;

    const uint64_t cpu = threadCpu100ns();
    const double cpuPct = 100.0 * static_cast<double>(cpu - g_lastThreadCpu) / 10000.0 / wallMs;
    g_lastThreadCpu = cpu;

    const double draws = c[CDraw] + c[CDrawIndexed] + c[CDrawVB];
    // uniqueTex comes from the one detail frame.
    // world = game code + device calls; device calls = D3D (draw + state) + SacredBild's own work (proxy, batcher, stats).
    LOG("fps={:.1f} frame={:.2f}ms (max {:.1f}, {} hitches) world={:.2f} (game {:.2f}, device calls {:.2f}) ui={:.2f} flip={:.2f} | per frame: "
        "draws={:.0f} (TL {:.0f}, quads {:.0f}, VB {:.0f}) submitted={:.0f} verts={:.0f} setTex={:.0f} texSwitch={:.0f} "
        "uniqueTex={} rs={:.0f} tss={:.0f} xform={:.0f} clear={:.1f} | in d3d: draw={:.2f}ms state={:.2f}ms, "
        "SacredBild={:.2f}ms | lockBack={:.1f} ({:.2f}ms) | soundWait={:.2f}ms animWait={:.2f}ms | renderCPU={:.0f}%",
        frames * 1000.0 / wallMs, ms[TFrame], maxFrameMs, hitches, ms[TWorld], ms[TWorld] - ms[TWorldProxy], ms[TWorldProxy], ms[TUi],
        ms[TFlip], draws / frames, c[CDrawTL] / frames, c[CDrawQuad] / frames, c[CDrawVB] / frames,
        c[CSubmit] / frames, c[CVerts] / frames, c[CSetTexture] / frames, c[CTexSwitch] / frames, c[CUniqueTex],
        c[CRenderState] / frames, c[CStageState] / frames, c[CTransform] / frames, c[CClear] / frames, ms[TDraw],
        ms[TState], ms[TProxy] - ms[TDraw] - ms[TState], c[CLockBack] / frames, ms[TLockBack], ms[TSoundWait], ms[TAnimationWait], cpuPct);
    if (c[CMerged] || c[CFlushOther] || c[CFlushDirect])
    {
        LOG("batch: merged={:.0f} atlasDraws={:.0f} uploads={} pageResets={} | models batched={:.0f} ({:.0f} verts) "
            "direct={:.0f} | stages kept on the original texture: texture={:.0f} setup={:.0f} uv={:.0f} "
            "sharedCoords={:.0f} pagesFull={:.0f} | batches ended by texture={:.0f} (page->page {:.0f}, original {:.0f}) "
            "rs={:.0f} tss={:.0f} viewport={:.0f} format={:.0f} full={:.0f} direct={:.0f} atlas={:.0f} lighting={:.0f} "
            "world={:.0f} other={:.0f}",
            c[CMerged] / frames, c[CAtlasDraw] / frames, c[CAtlasUpload], c[CAtlasReset], c[CModelDraw] / frames,
            c[CModelVerts] / frames, c[CModelDirect] / frames, c[CAtlasSkipTexture] / frames,
            c[CAtlasSkipSetup] / frames, c[CAtlasRange] / frames, c[CAtlasSkipShared] / frames,
            c[CAtlasSkipFull] / frames, c[CFlushTexture] / frames, c[CFlushPages] / frames, c[CFlushOriginal] / frames,
            c[CFlushRenderState] / frames, c[CFlushStageState] / frames, c[CFlushViewport] / frames,
            c[CFlushFormat] / frames, c[CFlushFull] / frames, c[CFlushDirect] / frames, c[CFlushAtlas] / frames,
            c[CFlushLighting] / frames, c[CFlushWorld] / frames, c[CFlushOther] / frames);
    }

    if (c[CSkinDeferred] || c[CSkinCpu])
    {
        LOG("skinning: {:.0f} deforms left to the GPU, {:.0f} drawn by the shader, {:.0f} skinned on the CPU after all",
            c[CSkinDeferred] / frames, c[CSkinGpu] / frames, c[CSkinCpu] / frames);
    }

    // Per pass: exclusive time, of that inside the device proxy (D3D included), game draws -> submitted draws. A
    // merged batch is submitted (and its D3D time spent) in the pass that ends it, not the ones whose draws it holds.
    if (g_passTotals[PWorld].ticks)
    {
        static constexpr const char* kPassNames[PassCount] = {
            "", "other", "rows", "ground", "layers", "water", "objects", "objects2", "models"};
        std::string line;
        for (int p = PWorld; p < PassCount; ++p)
        {
            const PassTotals& pt = g_passTotals[p];
            line += Fmt::format("{}{} {:.2f} (device {:.2f}, {:.0f}->{:.0f})", p == PWorld ? "" : " | ", kPassNames[p],
                static_cast<double>(pt.ticks) / tscPerMs / frames, static_cast<double>(pt.proxyTicks) / tscPerMs / frames,
                pt.draws / frames, pt.submits / frames);
        }
        LOG("passes: {}", line);
    }
    std::fill(std::begin(g_passTotals), std::end(g_passTotals), PassTotals{});

    if (++g_reportsSinceDetail >= 30)
    {
        g_reportsSinceDetail = 0;
        std::scoped_lock lock(g_detailMutex);
        logDetail();
    }

    g_frames = 0;
    g_reportQpc = q;
    g_detailFrame.store(true, std::memory_order_relaxed);
}
