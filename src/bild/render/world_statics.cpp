// Draw-list take-over, static objects (world_statics.h). What objects2 (ENG 0x62FF60) does for a kind-1 entry, from its
// code at Addr::objects2StaticSprite (ENG 0x630217 .. 0x630A2A):
//   record = staticObject(id); descriptor = sprite(record +4); entry flag 0x10: footprint call; descriptor 0x20000: the
//   record's light; frame = descriptor +0x10 plus the animation offset (frames +0x2C, speed +0x30, from the game timer);
//   nothing while the world flags (0x182EE38) have 0x400; the frame's quads (36 bytes: texture, right, bottom, left,
//   top, u0, v0, u1, v1, relative to the entry's position at zoom 1); render flags lighting, blend factors, culling,
//   filtering off, alpha test on, depth test and write with descriptor 0x200 (depth by screen y and the record's
//   height * 28); per quad: placed by zoom around the screen center, culled, lit per corner (if the view lights statics),
//   faded by the see-through circles (descriptor 0x10), half a texel inset where the texture filter asks for it,
//   culled again and appended to the sprite batcher; then flushed, depth and alpha test off and the circles' fades
//   advanced.
// The functions and globals are taken from that code itself (calls, operands), checked byte by byte, so the patched
// operands (Resolution's screen size and center) are the ones read.
#include "world_statics.h"
#include "device_proxy.h"
#include "engine/sacred_addr.h"
#include "sacred/render.h"
#include "settings/config/ddraw.h"
#include "settings/config/render.h"
#include "log.h"
#include "patch.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace
{
    using namespace Sacred;

    uintptr_t g_site = 0;
    uintptr_t g_continue = 0;   // the entry loop's next entry
    uintptr_t g_resumeCall = 0; // the game's own code for the entry (its call of staticObject)
    uintptr_t g_resumeCall2 = 0; // the same for a miniature (kind 2)

    struct Fn
    {
        void* (__thiscall* staticObject)(void* objects, uint32_t id);
        uint8_t* (__thiscall* sprite)(void* sprites, int32_t id);
        void (__thiscall* footprint)(void* view, IDirect3DDevice7* device, void* descriptor, uint32_t x, uint32_t y);
        void (__thiscall* recordLight)(void* light, int32_t x, int32_t y, uint32_t value);
        uint8_t* (__cdecl* timer)();
        void* (__cdecl* frames)();
        uint32_t* (__thiscall* frame)(void* frames, uint32_t index);
        float* (__thiscall* frameQuads)(void* frames, uint32_t index);
        uint32_t (__thiscall* renderFlag)(void* flags, uint32_t flag, uint32_t on);
        uint32_t (__thiscall* lightAt)(void* light, int32_t x, int32_t y, int32_t z);
        float (__thiscall* spriteDepth)(void* view, float y, float height);
        bool (__thiscall* halfTexel)(void* filter);
        void (__thiscall* flush)(void* batcher, IDirect3DDevice7* device);
        void** sprites;
        const uint32_t* worldFlags;
        void* filter;
        uint32_t batcherOffset;
        // Operands read on every call (Resolution patches them): the screen center, size and 0.
        const float* const* halfW;
        const float* const* halfH;
        const float* const* width;
        const float* const* height;
        const float* zero;
        const float* fadeRadius;
        const float* fadeMin;
        const float* one;
        const float* full;      // 255
        const float* texel;
        const float* fadeMax;   // 0.8
        // Miniatures (kind 2).
        const float* cellSpan;      // 256: a grid cell's size is this / columns
        const float* angleStep;     // radians per rotation step
        const float* halfFactor;    // 0.5
        const double* linearZoom;   // 1.2: filtered above this zoom
        void* (__cdecl* texturePak)();
        uint32_t (__thiscall* textureHandle)(void* pak, const char* name);
        const char* glowName;
        uint32_t (__thiscall* lightFlags)(void* light);
        // Blob shadows (the footprint function).
        void* (__cdecl* renderFlags)();
        void* (__cdecl* lightObject)();
        uint32_t (__thiscall* shadowColor)(void* light);
        const float* shadowCell;    // 1/16: a cell of the shadow texture
        const float* const* shadowHalfW;
        const float* const* shadowHalfH;
    } g;

    decltype(Fn::footprint) g_origFootprint = nullptr;

    uintptr_t callAddr(uintptr_t at)
    {
        return at + 5 + *reinterpret_cast<const int32_t*>(at + 1);
    }

    template <class T>
    T callTarget(uintptr_t at)
    {
        return reinterpret_cast<T>(callAddr(at));
    }

    template <class T>
    T operand(uintptr_t at)
    {
        return *reinterpret_cast<T const*>(at);
    }

    // The view's fields (cWorldView): zoom, the see-through circles, the pass counters.
    struct Circle
    {
        int16_t x, y;
        uint32_t hits;
        float fade;
    };
    static_assert(sizeof(Circle) == 12);

    float zoom(const uint8_t* view) { return *reinterpret_cast<const float*>(view + 0xB60); }
    Circle* circles(uint8_t* view) { return reinterpret_cast<Circle*>(view + 0x96B3C); }
    int32_t circleCount(const uint8_t* view) { return *reinterpret_cast<const int32_t*>(view + 0x96FEC); }

    struct Vertex
    {
        float x, y, z, rhw;
        uint32_t color, specular;
        float u, v;
    };
    static_assert(sizeof(Vertex) == cSpriteBatcher::vertexSize);

    IDirectDrawSurface7* lookup(uint32_t handle)
    {
        cTexture* texture = cTextureManager::instance()->get(handle);
        return texture ? texture->surface : nullptr;
    }

    // The quads of one texture, drawn through the batcher in the order they came.
    // The quads of one texture, drawn through the batcher in the order they came. A full batch is drawn and started
    // over (the 16-bit indices reach 16384 vertices).
    struct Batch
    {
        static constexpr uint32_t kQuads = 4096;
        Vertex vertices[kQuads * 4];
        uint16_t indices[kQuads * 6];
        uint32_t quads = 0;
        uint32_t texture = 0;

        Batch()
        {
            for (uint32_t q = 0; q < kQuads; ++q)
            {
                const uint16_t b = static_cast<uint16_t>(q * 4);
                uint16_t* i = &indices[q * 6];
                i[0] = b; i[1] = b + 1; i[2] = b + 2; i[3] = b + 2; i[4] = b + 1; i[5] = b + 3;
            }
        }

        void add(DeviceProxy* proxy, const Vertex* v)
        {
            if (quads == kQuads)
            {
                draw(proxy);
            }
            Vertex* d = &vertices[quads * 4];
            d[0] = v[0];
            d[1] = v[1];
            d[2] = v[2];
            d[3] = v[3];
            ++quads;
        }

        void draw(DeviceProxy* proxy)
        {
            if (!quads)
            {
                return;
            }
            proxy->drawQuads(&lookup, texture, 0, cSpriteBatcher::fvf, vertices, quads * 4, indices, quads * 6);
            quads = 0;
        }
    };
    Batch g_batch;     // 160 KB, static

    int32_t ftol(float v) { return static_cast<int32_t>(v); }

    // The game's integer square root (bit by bit, rounded to the nearer).
    int32_t isqrt(int32_t n)
    {
        int32_t root = 0;
        for (int32_t bit = 0x10000000; bit; bit >>= 2)
        {
            const int32_t t = bit + root;
            root >>= 1;
            if (t <= n)
            {
                n -= t;
                root += bit;
            }
        }
        return root < n ? root + 1 : root;
    }

    // The see-through circles: a corner inside one gets the alpha of its distance from the center.
    void fade(uint8_t* view, Vertex (&v)[4])
    {
        const float z = zoom(view);
        const int32_t r = ftol(*g.fadeRadius / z);
        Circle* c = circles(view);
        const int32_t n = circleCount(view);
        for (int32_t i = 0; i < n; ++i, ++c)
        {
            for (Vertex& p : v)
            {
                if (!(static_cast<float>(c->x - r) < p.x) || static_cast<float>(c->x + r) < p.x ||
                    !(static_cast<float>(c->y - r) < p.y) || !(p.y < static_cast<float>(c->y + r)))
                {
                    continue;
                }
                const int32_t dx = ftol(p.x) - c->x, dy = ftol(p.y) - c->y;
                int32_t d = 0x7D00;
                if (dx <= 0x7D00 && dy <= 0x7D00 && dx >= -0x7D00 && dy >= -0x7D00)
                {
                    d = isqrt(dy * dy + dx * dx);
                }
                double t = static_cast<double>(d) * z / *g.fadeRadius;
                if (!(t <= *g.one))
                {
                    t = *g.one;
                }
                const double alpha = (*g.one - c->fade) * static_cast<double>(*g.fadeMin) +
                    (static_cast<double>(*g.full) - *g.fadeMin) * t;
                p.color = static_cast<uint32_t>(static_cast<int32_t>(alpha)) << 24 | (p.color & 0xFFFFFF);
                ++c->hits;
            }
        }
    }

    // Where a pass keeps, in its stack frame, whether statics are lit and the light object.
    struct Locals
    {
        uint32_t lit, light;
    };
    constexpr Locals kObjects2 = {0x1B, 0x34};   // objects2 (0x62FF60)
    constexpr Locals kObjects1 = {0x17, 0x3C};   // the first object pass (0x62E410)

    struct Entry
    {
        uint32_t kind;
        uint32_t id;
        float x, y;
    };

    // The entry as the game draws it; false: not this device (the game's code runs).
    bool draw(uint8_t* view, const Entry* entry, IDirect3DDevice7* device, void* flags, const uint8_t* frame, const Locals& at)
    {
        DeviceProxy* proxy = DeviceProxy::instance();
        if (!proxy || device != proxy)
        {
            return false;
        }
        const uint8_t* record = static_cast<const uint8_t*>(g.staticObject(*reinterpret_cast<void**>(view + 4), entry->id));
        const int32_t spriteId = *reinterpret_cast<const int32_t*>(record + 4);
        if (!spriteId)
        {
            return true;
        }
        uint8_t* desc = g.sprite(*g.sprites, spriteId);
        const uint32_t descFlags = *reinterpret_cast<const uint32_t*>(desc);
        if (entry->kind & 0x10)
        {
            uint32_t x, y;
            std::memcpy(&x, &entry->x, 4);
            std::memcpy(&y, &entry->y, 4);
            g.footprint(view, device, desc, x, y);
        }
        void* light = *reinterpret_cast<void* const*>(frame + at.light);
        if (descFlags & 0x20000)
        {
            g.recordLight(light, *reinterpret_cast<const int32_t*>(record + 0xE), *reinterpret_cast<const int32_t*>(record + 0x12),
                *reinterpret_cast<const uint16_t*>(desc + 0x32));
        }
        uint32_t index = *reinterpret_cast<const uint32_t*>(desc + 0x10);
        const uint32_t frameCount = *reinterpret_cast<const uint16_t*>(desc + 0x2C);
        if (frameCount > 1)
        {
            const uint8_t* t = g.timer();
            const int32_t period = static_cast<int32_t>(desc[0x30] * frameCount * 10);
            const int64_t counter = *reinterpret_cast<const int64_t*>(t + 0x10);
            const int64_t frequency = *reinterpret_cast<const int64_t*>(t + 0x20);
            const int32_t ms = static_cast<int32_t>(counter * 1000 / frequency);
            index += static_cast<uint32_t>(((ms % period) * static_cast<int32_t>(frameCount)) / period);
        }
        void* frames = g.frames();
        if (*g.worldFlags & 0x400)
        {
            return true;
        }
        const uint32_t* quadSet = g.frame(frames, index);
        if (!quadSet)
        {
            return true;
        }
        const float* quads = g.frameQuads(frames, index);
        if (!quads)
        {
            return true;
        }
        g.renderFlag(flags, 0x20, 0);
        g.renderFlag(flags, 8, 0);
        g.renderFlag(flags, 0x10, 0);
        g.renderFlag(flags, 2, 0);
        g.renderFlag(flags, 0x800, 0);
        g.renderFlag(flags, 0x4000, 1);
        const bool depth = (descFlags & 0x200) != 0;
        float height = 0.0f;
        if (depth)
        {
            g.renderFlag(flags, 0x100, 1);
            g.renderFlag(flags, 1, 1);
            height = static_cast<float>(record[0x33] * 0x1C);
        }
        else
        {
            g.renderFlag(flags, 0x100, 0);
            g.renderFlag(flags, 1, 0);
        }
        ++*reinterpret_cast<uint32_t*>(view + 0x97090);
        // Quads the game's batcher holds come first.
        g.flush(view + g.batcherOffset, device);

        const float z = zoom(view);
        const float halfW = **g.halfW, halfH = **g.halfH, w = **g.width, h = **g.height, zero = *g.zero;
        const float cx = (entry->x - halfW) / z + halfW;
        const float cy = (entry->y - halfH) / z + halfH;
        const bool lit = frame[at.lit] != 0;
        const uint32_t count = *quadSet;
        const float* q = quads + 4;     // the game's pointer: 0x10 into the 36-byte record
        uint32_t texture = 0;
        for (uint32_t i = 0; i < count; ++i, q += 9)
        {
            const float left = q[-1] / z + cx, right = q[-3] / z + cx;
            const float top = q[0] / z + cy, bottom = q[-2] / z + cy;
            if (!(left < w) || right < zero || !(top < h) || bottom < zero)
            {
                continue;
            }
            Vertex v[4];
            v[0].x = v[1].x = left;
            v[2].x = v[3].x = right;
            v[0].y = v[2].y = top;
            v[1].y = v[3].y = bottom;
            for (Vertex& p : v)
            {
                p.z = 0.0f;
                p.rhw = 1.0f;
                p.specular = 0;
            }
            if (lit)
            {
                const int32_t xl = ftol(entry->x + q[-1]), xr = ftol(entry->x + q[-3]);
                const int32_t yt = ftol(entry->y + q[0]), yb = ftol(entry->y + q[-2]);
                v[0].color = g.lightAt(light, xl, yt, 0) | 0xFF000000;
                v[1].color = g.lightAt(light, xl, yb, 0) | 0xFF000000;
                v[2].color = g.lightAt(light, xr, yt, 0) | 0xFF000000;
                v[3].color = g.lightAt(light, xr, yb, 0) | 0xFF000000;
            }
            else
            {
                v[0].color = v[1].color = v[2].color = v[3].color = 0xFFFFFFFF;
            }
            if (descFlags & 0x10)
            {
                fade(view, v);
            }
            if (depth)
            {
                for (Vertex& p : v)
                {
                    p.z = g.spriteDepth(view, p.y, height);
                }
            }
            const uint32_t handle = *reinterpret_cast<const uint32_t*>(q - 4);
            if (handle != texture)
            {
                g_batch.draw(proxy);
                texture = handle;
                g_batch.texture = handle;
            }
            const float e = g.halfTexel(g.filter) ? *g.texel : 0.0f;
            v[0].u = v[1].u = q[1] + e;
            v[2].u = v[3].u = q[3] - e;
            v[0].v = v[2].v = q[2] + e;
            v[1].v = v[3].v = q[4] - e;
            // The batcher's own test: some of the quad on the screen.
            float minX = v[0].x, maxX = v[0].x, minY = v[0].y, maxY = v[0].y;
            for (const Vertex& p : v)
            {
                minX = p.x < minX ? p.x : minX;
                maxX = p.x > maxX ? p.x : maxX;
                minY = p.y < minY ? p.y : minY;
                maxY = p.y > maxY ? p.y : maxY;
            }
            if (zero < maxX && minX < w && zero < maxY && minY < h)
            {
                g_batch.add(proxy, v);
                ++*reinterpret_cast<uint32_t*>(view + 0x97094);
            }
        }
        g_batch.draw(proxy);
        static bool s_logged = false;
        if (!s_logged)
        {
            s_logged = true;
            LOG("World statics: first static object drawn by SacredBild ({} quads)", count);
        }
        g.renderFlag(flags, 0x100, 0);
        g.renderFlag(flags, 1, 0);
        g.renderFlag(flags, 0x4000, 0);
        // The circles' fades go up with how many corners they faded.
        Circle* c = circles(view);
        const uint32_t n = static_cast<uint32_t>(circleCount(view));
        for (uint32_t i = 0; i < n; ++i, ++c)
        {
            if (c->hits)
            {
                c->fade = static_cast<float>(static_cast<double>(c->hits) / static_cast<int32_t>(count << 2) + c->fade);
                if (c->fade > *g.fadeMax)
                {
                    c->fade = 0.8f;
                }
            }
        }
        return true;
    }

    bool __cdecl drawEntry(uint8_t* view, const Entry* entry, IDirect3DDevice7* device, void* flags, const uint8_t* frame)
    {
        return draw(view, entry, device, flags, frame, kObjects2);
    }

    bool drawMiniature(uint8_t* view, const Entry* entry, IDirect3DDevice7* device, void* flags, const uint8_t* frame, const Locals& at);

    bool __cdecl drawEntry1(uint8_t* view, const Entry* entry, IDirect3DDevice7* device, void* flags, const uint8_t* frame)
    {
        return draw(view, entry, device, flags, frame, kObjects1);
    }

    bool __cdecl drawMiniatureEntry1(uint8_t* view, const Entry* entry, IDirect3DDevice7* device, void* flags, const uint8_t* frame)
    {
        return drawMiniature(view, entry, device, flags, frame, kObjects1);
    }

    uintptr_t g_continue1 = 0, g_resume1 = 0, g_resume1Mini = 0;

    // The first object pass: esi the view, ebp the entry, edi the device, ebx the render flags.
    __declspec(naked) void stub1()
    {
        __asm
        {
            pushad
            lea eax, [esp + 32]
            push eax
            push ebx
            push edi
            push ebp
            push esi
            call drawEntry1
            add esp, 20
            test al, al
            popad
            jne done
            mov eax, dword ptr [ebp + 4]
            mov ecx, dword ptr [esi + 4]
            push eax
            jmp dword ptr [g_resume1]
        done:
            jmp dword ptr [g_continue1]
        }
    }

    __declspec(naked) void stub1Mini()
    {
        __asm
        {
            pushad
            lea eax, [esp + 32]
            push eax
            push ebx
            push edi
            push ebp
            push esi
            call drawMiniatureEntry1
            add esp, 20
            test al, al
            popad
            jne done
            mov ecx, dword ptr [ebp + 4]
            push ecx
            mov ecx, dword ptr [esi + 4]
            jmp dword ptr [g_resume1Mini]
        done:
            jmp dword ptr [g_continue1]
        }
    }

    // The first object pass's static objects and miniatures: the same code as objects2's (instruction for instruction,
    // other registers and frame slots), at Addr::objects1StaticSprite and +0x805.
    bool resolvePass1(uintptr_t s)
    {
        if (!Patch::verify(s, {0x8B, 0x45, 0x04, 0x8B, 0x4E, 0x04, 0x50, 0xE8}) || !Patch::verify(s + 0x015, {0x0F, 0x84}) ||
            !Patch::verify(s + 0x805, {0x8B, 0x4D, 0x04, 0x51, 0x8B, 0x4E, 0x04, 0xE8}) ||
            callAddr(s + 0x007) != reinterpret_cast<uintptr_t>(g.staticObject) ||
            callAddr(s + 0x80C) != reinterpret_cast<uintptr_t>(g.staticObject))
        {
            LOG("World statics: the first object pass's code is not the expected one; the game draws its static objects");
            return false;
        }
        g_continue1 = s + 0x015 + 6 + *reinterpret_cast<const int32_t*>(s + 0x017);
        g_resume1 = s + 0x007;
        g_resume1Mini = s + 0x80C;
        return true;
    }

    // At the site: esi the view, ebx the entry, edi the render flags, [ebp + 8] the device; the function's frame at esp.
    __declspec(naked) void stub()
    {
        __asm
        {
            pushad
            lea eax, [esp + 32]
            push eax
            push edi
            push dword ptr [ebp + 8]
            push ebx
            push esi
            call drawEntry
            add esp, 20
            test al, al
            popad
            jne done
            mov ecx, dword ptr [ebx + 4]
            push ecx
            mov ecx, dword ptr [esi + 4]
            jmp dword ptr [g_resumeCall]
        done:
            jmp dword ptr [g_continue]
        }
    }

    // A miniature (kind 2): one quad from a static record's own texture (its descriptor's +4), either a square cut out
    // of it (+0x2E, +0x2F the corner, +0x30 the size) or a cell of a grid of +0x2E x +0x2F cells animated over time
    // (record flag 0x40; +0x30 the rotation in steps, +0x31 / +0x32 the speed), lit per corner, and a glow over it
    // where the view lights statics and the descriptor has a light of its own.
    bool drawMiniature(uint8_t* view, const Entry* entry, IDirect3DDevice7* device, void* flags, const uint8_t* frame, const Locals& at)
    {
        DeviceProxy* proxy = DeviceProxy::instance();
        if (!proxy || device != proxy)
        {
            return false;
        }
        const uint8_t* record = static_cast<const uint8_t*>(g.staticObject(*reinterpret_cast<void**>(view + 4), entry->id));
        const uint8_t* desc = g.sprite(*g.sprites, *reinterpret_cast<const int32_t*>(record + 4));
        const uint32_t descFlags = *reinterpret_cast<const uint32_t*>(desc);
        const float z = zoom(view);
        const float halfW = **g.halfW, halfH = **g.halfH, w = **g.width, h = **g.height, zero = *g.zero;
        const float a = static_cast<float>(record[0x2E]), b = static_cast<float>(record[0x2F]);
        const uint8_t step = record[0x30];
        const float c = static_cast<float>(step);
        const float cx = (entry->x - halfW) / z + halfW;
        const float cy = (entry->y - halfH) / z + halfH;
        const float texel = *g.texel;
        void* light = *reinterpret_cast<void* const*>(frame + at.light);
        const int32_t recordX = *reinterpret_cast<const int32_t*>(record + 0xE);
        const int32_t recordY = *reinterpret_cast<const int32_t*>(record + 0x12);
        float corner[4][2] = {};
        float u0, v0, u1, v1, r;
        if (record[8] & 0x40)
        {
            const float cellW = *g.cellSpan / a, cellH = *g.cellSpan / b;
            corner[1][0] = cellW;
            corner[2][1] = cellH;
            corner[3][0] = cellW;
            corner[3][1] = cellH;
            if (step)
            {
                const double angle = static_cast<double>(c) * *g.angleStep;
                const float sn = static_cast<float>(std::sin(angle));
                const double cs = std::cos(angle);
                for (float(&p)[2] : corner)
                {
                    const double x = p[0], y = p[1];
                    p[0] = static_cast<float>(x * cs + y * sn);
                    p[1] = static_cast<float>(y * cs - x * sn);
                }
            }
            const uint8_t* t = g.timer();
            const int64_t counter = *reinterpret_cast<const int64_t*>(t + 0x10);
            const int64_t frequency = *reinterpret_cast<const int64_t*>(t + 0x20);
            const uint32_t ms = static_cast<uint32_t>(counter * 1000 / frequency);
            const int32_t columns = record[0x2E];
            const uint32_t q = (ms << 2) / (static_cast<uint32_t>(record[0x31]) * static_cast<uint32_t>(columns));
            const int32_t cell = static_cast<int32_t>((q % 1000) * record[0x32]) / 1000;
            u0 = static_cast<float>(cell % columns) * cellW * texel;
            v0 = static_cast<float>(cell / columns) * cellH * texel;
            u1 = cellW * texel + u0;
            v1 = cellH * texel + v0;
            r = cellW / z;
            if (descFlags & 0x20000)
            {
                const int32_t y = ftol(static_cast<float>(recordY) + cellH * *g.halfFactor);
                const int32_t x = ftol(static_cast<float>(recordX) + cellW * *g.halfFactor);
                g.recordLight(light, x, y, *reinterpret_cast<const uint16_t*>(desc + 0x32));
            }
        }
        else
        {
            corner[1][0] = c;
            corner[2][1] = c;
            corner[3][0] = c;
            corner[3][1] = c;
            u0 = a * texel;
            v0 = b * texel;
            u1 = (c + a) * texel;
            v1 = (c + b) * texel;
            r = c / z;
            if (descFlags & 0x20000)
            {
                const float half = c * *g.halfFactor;
                const int32_t y = ftol(static_cast<float>(recordY) + half);
                const int32_t x = ftol(static_cast<float>(recordX) + half);
                g.recordLight(light, x, y, *reinterpret_cast<const uint16_t*>(desc + 0x32));
            }
        }
        Vertex v[4] = {};
        for (int i = 0; i < 4; ++i)
        {
            v[i].x = corner[i][0] / z + cx;
            v[i].y = corner[i][1] / z + cy;
            v[i].rhw = 1.0f;
        }
        if (!(v[0].x - r < w) || v[1].x + r < zero || !(v[0].y - r < h) || v[2].y + r < zero)
        {
            return true;
        }
        v[0].u = u0; v[0].v = v0;
        v[1].u = u1; v[1].v = v0;
        v[2].u = u0; v[2].v = v1;
        v[3].u = u1; v[3].v = v1;
        const bool lit = frame[at.lit] != 0;
        for (int i = 0; i < 4; ++i)
        {
            if (!(descFlags & 0x20000) && lit)
            {
                const int32_t y = ftol(static_cast<float>(ftol(corner[i][1])) + entry->y);
                const int32_t x = ftol(static_cast<float>(ftol(corner[i][0])) + entry->x);
                v[i].color = g.lightAt(light, x, y, 0) | 0xFF000000;
            }
            else
            {
                v[i].color = 0xFFFFFFFF;
            }
        }
        g.renderFlag(flags, 8, 0);
        g.renderFlag(flags, 0x10, 0);
        g.renderFlag(flags, 0x100, 0);
        g.renderFlag(flags, 1, 0);
        g.renderFlag(flags, 0x800, z > *g.linearZoom ? 1 : 0);
        g_batch.texture = *reinterpret_cast<const uint32_t*>(desc + 4);
        g_batch.add(proxy, v);
        g_batch.draw(proxy);
        ++*reinterpret_cast<uint32_t*>(view + 0x9708C);
        static bool s_logged = false;
        if (!s_logged)
        {
            s_logged = true;
            LOG("World statics: first miniature drawn by SacredBild ({})", record[8] & 0x40 ? "animated" : "square");
        }
        if (!lit || !(descFlags & 0x20000))
        {
            return true;
        }
        v[0].x -= r; v[0].y -= r;
        v[1].x += r; v[1].y -= r;
        v[2].x -= r; v[2].y += r;
        v[3].x += r; v[3].y += r;
        v[0].u = 0.0f; v[0].v = 0.0f;
        v[1].u = 0.0f; v[1].v = 1.0f;
        v[2].u = 1.0f; v[2].v = 0.0f;
        v[3].u = 1.0f; v[3].v = 1.0f;
        const uint8_t* l = static_cast<const uint8_t*>(light);
        uint32_t glow = 0xFFFFFFFF;
        if (l[0x9E0C])
        {
            glow = 0x40FFDA2B;
        }
        else if (g.lightFlags(light) & 0x20)
        {
            glow = *reinterpret_cast<const uint32_t*>(l + 0x12E54) << 23 | 0xFFEC95;
        }
        for (Vertex& p : v)
        {
            p.color = glow;
        }
        g.renderFlag(flags, 0x800, 1);
        g_batch.texture = g.textureHandle(g.texturePak(), g.glowName);
        g_batch.add(proxy, v);
        g_batch.draw(proxy);
        return true;
    }

    bool __cdecl drawMiniatureEntry(uint8_t* view, const Entry* entry, IDirect3DDevice7* device, void* flags, const uint8_t* frame)
    {
        return drawMiniature(view, entry, device, flags, frame, kObjects2);
    }

    __declspec(naked) void stub2()
    {
        __asm
        {
            pushad
            lea eax, [esp + 32]
            push eax
            push edi
            push dword ptr [ebp + 8]
            push ebx
            push esi
            call drawMiniatureEntry
            add esp, 20
            test al, al
            popad
            jne done
            mov ecx, dword ptr [ebx + 4]
            push ecx
            mov ecx, dword ptr [esi + 4]
            jmp dword ptr [g_resumeCall2]
        done:
            jmp dword ptr [g_continue]
        }
    }

    // A blob shadow under a static object (the game's footprint function, ENG 0x631EB0, also called by the first object
    // pass): one quad of the view's shadow texture (+0x97070; 16 x 16 cells, the cell's corner at view +0x30 + 8 * the
    // descriptor's +0x5B) at twice the descriptor's offset (+0x5D, +0x5F), +0x64 wide, slanted where +0x63 says so, in
    // the light's shadow color. The quad is the view's own vertex template (+0x96FF0), filled in as the game does.
    void __fastcall footprint(uint8_t* view, void*, IDirect3DDevice7* device, uint8_t* desc, uint32_t xBits, uint32_t yBits)
    {
        DeviceProxy* proxy = DeviceProxy::instance();
        if (!proxy || device != proxy)
        {
            g_origFootprint(view, device, desc, xBits, yBits);
            return;
        }
        float x, y;
        std::memcpy(&x, &xBits, 4);
        std::memcpy(&y, &yBits, 4);
        void* flags = g.renderFlags();
        const float z = zoom(view);
        const float halfW = **g.shadowHalfW, halfH = **g.shadowHalfH;
        const float cx = (x - halfW) / z + halfW;
        const float cy = (y - halfH) / z + halfH;
        const float ox = static_cast<float>(*reinterpret_cast<const int16_t*>(desc + 0x5D)) * 2.0f;
        const float oy = static_cast<float>(*reinterpret_cast<const int16_t*>(desc + 0x5F)) * 2.0f;
        const float size = static_cast<float>(*reinterpret_cast<const uint16_t*>(desc + 0x64));
        const uint16_t cellIndex = *reinterpret_cast<const uint16_t*>(desc + 0x5B);
        Vertex* v = reinterpret_cast<Vertex*>(view + 0x96FF0);
        const uint32_t color = g.shadowColor(g.lightObject());
        for (int i = 0; i < 4; ++i)
        {
            v[i].color = color;
        }
        if (desc[0x63])
        {
            v[0].x = ox / z + cx;
            v[1].x = (size + size + ox) / z + cx;
            v[2].x = (ox - size) / z + cx;
            v[3].x = (size + ox) / z + cx;
        }
        else
        {
            v[0].x = v[2].x = (ox - size) / z + cx;
            v[1].x = v[3].x = (size + ox) / z + cx;
        }
        v[0].y = v[1].y = (oy - size) / z + cy;
        v[2].y = v[3].y = oy / z + cy;
        const float* cell = reinterpret_cast<const float*>(view + 0x30 + cellIndex * 8);
        const float step = *g.shadowCell;
        v[0].u = cell[0];        v[0].v = cell[1];
        v[1].u = cell[0] + step; v[1].v = cell[1];
        v[2].u = cell[0];        v[2].v = cell[1] + step;
        v[3].u = cell[0] + step; v[3].v = cell[1] + step;
        g.renderFlag(flags, 0x100, 0);
        g.renderFlag(flags, 1, 0);
        g.renderFlag(flags, 8, 0);
        g.renderFlag(flags, 0x10, 0);
        g.renderFlag(flags, 0x800, 1);
        static bool s_logged = false;
        if (!s_logged)
        {
            s_logged = true;
            LOG("World statics: first blob shadow drawn by SacredBild");
        }
        g_batch.texture = *reinterpret_cast<const uint32_t*>(view + 0x97070);
        g_batch.add(proxy, v);
        g_batch.draw(proxy);
    }

    bool resolveFootprint(uintptr_t f)
    {
        struct Check
        {
            uint32_t offset;
            std::initializer_list<uint8_t> bytes;
        };
        const Check checks[] = {
            {0x000, {0x83, 0xEC, 0x08, 0x53, 0x55, 0x56, 0x57, 0x8B, 0xF1, 0xE8}}, {0x012, {0xD8, 0x25}},
            {0x04A, {0xD8, 0x25}}, {0x084, {0xE8}}, {0x08B, {0xE8}}, {0x090, {0x89, 0x86, 0x60, 0x70, 0x09, 0x00}},
            {0x1B0, {0xD8, 0x05}}, {0x202, {0xE8}}, {0x236, {0x8B, 0x96, 0x70, 0x70, 0x09, 0x00}},
            {0x274, {0xFF, 0x52, 0x64}}, {0x27E, {0xC2, 0x10, 0x00}},
        };
        for (const Check& c : checks)
        {
            if (!Patch::verify(f + c.offset, c.bytes))
            {
                LOG("World statics: the footprint function's code at +{:#x} is not the expected one; the game draws blob shadows", c.offset);
                return false;
            }
        }
        if (callAddr(f + 0x202) != reinterpret_cast<uintptr_t>(g.renderFlag))
        {
            return false;
        }
        g.renderFlags = callTarget<decltype(g.renderFlags)>(f + 0x009);
        g.shadowHalfW = reinterpret_cast<const float* const*>(f + 0x014);
        g.shadowHalfH = reinterpret_cast<const float* const*>(f + 0x04C);
        g.lightObject = callTarget<decltype(g.lightObject)>(f + 0x084);
        g.shadowColor = callTarget<decltype(g.shadowColor)>(f + 0x08B);
        g.shadowCell = operand<const float*>(f + 0x1B2);
        return true;
    }

    // ---- The water pass (cWorldView::drawWaterTiles, ENG 0x62DD00) ----
    // The view's liquid cells (+0x3FF2C, 0x98 bytes each, count +0x80E3C): the tile (+0), the cell's screen position
    // (+0x04, +0x08 floats), its tile position (+0x0C, +0x10 ints), the region index (+0x14 short), and four
    // pretransformed corners (+0x18, 32 bytes each) for the reflection quad. Each cell is
    // water (tile type 0x90) or lava (0xA0); its region picks the liquid's material in the light object (+0x91E8 + 0xD8
    // * material: frame textures, animation rate +0xC8, alpha factor +0xD0, reflection on +0xCC).
    // With the detail setting (byte at 0x182EE78) 2 or more: a reflection quad per cell from its corners, shifted by 0.2 /
    // zoom, gray by the tile corners' depth bytes (+0x10..+0x13) and the light's shadow alpha, additive, with the
    // texture the liquid batcher had last; the cells' average position goes to the water sound. Then the surface: a quad
    // per cell from the view's corner offsets (+0xA90, one set per position modulo 4), the animated frame, per-corner
    // light where the view lights statics, alpha by the depth bytes times the material's factor.
    struct Water
    {
        void* (__cdecl* renderFlags)();
        void* (__cdecl* lightObject)();
        uint32_t (__thiscall* shadowColor)(void* light);
        uint8_t* (__cdecl* timer)();
        void (__cdecl* soundManager)();
        void (__cdecl* mapToWorld)(float* out, const float* in);
        void* soundUpdate;
        const float* reflectShift;
        const uint8_t* detail;
        const float* frameRate;
        const float* const* halfW;
        const float* const* halfH;
        const int32_t* centerX;
        const int32_t* centerY;
        uint32_t batcherOffset;
    } w;

    decltype(Addr::cWorldView_drawWaterTiles)::Ptr g_origWater = nullptr;

    // sound(count, x, y) as the game makes it: the sound manager is fetched with the arguments already pushed, and its
    // update (thiscall) takes them.
    __declspec(naked) void __cdecl waterSound(uint32_t, int32_t, int32_t)
    {
        __asm
        {
            push dword ptr [esp + 12]
            push dword ptr [esp + 12]
            push dword ptr [esp + 12]
            call dword ptr [w.soundManager]
            mov ecx, eax
            call dword ptr [w.soundUpdate]
            ret
        }
    }

    bool onScreen(const Vertex* v)
    {
        const float wd = **g.width, ht = **g.height, zero = *g.zero;
        float minX = v[0].x, maxX = v[0].x, minY = v[0].y, maxY = v[0].y;
        for (int k = 0; k < 4; ++k)
        {
            const Vertex& p = v[k];
            minX = p.x < minX ? p.x : minX;
            maxX = p.x > maxX ? p.x : maxX;
            minY = p.y < minY ? p.y : minY;
            maxY = p.y > maxY ? p.y : maxY;
        }
        return zero < maxX && minX < wd && zero < maxY && minY < ht;
    }

    // The liquid's material for a cell, or null (not a liquid tile).
    const uint8_t* material(uint8_t* view, const uint8_t* cell, const uint8_t* light)
    {
        const uint8_t* tile = *reinterpret_cast<uint8_t* const*>(cell);
        const int16_t regionIndex = *reinterpret_cast<const int16_t*>(cell + 0x14);
        const uint8_t* region = *reinterpret_cast<uint8_t* const*>(view + 0x970B0 + regionIndex * 4);
        const uint8_t type = tile[0x1F] & 0xF0;
        uint8_t index;
        if (type == 0x90)
        {
            index = (*reinterpret_cast<uint8_t* const*>(region + 0x17C))[0xF7];
        }
        else if (type == 0xA0)
        {
            index = (*reinterpret_cast<uint8_t* const*>(region + 0x17C))[0xF8];
        }
        else
        {
            return nullptr;
        }
        return light + index * 0xD8 + 0x91E8;
    }

    uint32_t clampByte(int32_t v)
    {
        return v < 0 ? 0u : v > 0xFF ? 0xFFu : static_cast<uint32_t>(v);
    }

    void __fastcall water(uint8_t* view, void*, IDirect3DDevice7* device)
    {
        DeviceProxy* proxy = DeviceProxy::instance();
        if (!proxy || device != proxy)
        {
            g_origWater(reinterpret_cast<cWorldView*>(view), nullptr, device);
            return;
        }
        void* flags = w.renderFlags();
        uint8_t* light = static_cast<uint8_t*>(w.lightObject());
        const bool lit = (light[0x10] & 0x20) || light[0x9E0C];
        const float z = zoom(view);
        const float shift = *w.reflectShift / z;
        const int32_t count = *reinterpret_cast<const int32_t*>(view + 0x80E3C);
        if (!count)
        {
            waterSound(static_cast<uint16_t>(count), *reinterpret_cast<const int32_t*>(view + 0x14),
                *reinterpret_cast<const int32_t*>(view + 0x18));
            return;
        }
        uint32_t* batcherTexture = reinterpret_cast<uint32_t*>(view + w.batcherOffset + 0x4FC0);
        uint8_t* cells = view + 0x3FF2C;
        if (*w.detail >= 2)
        {
            g.renderFlag(flags, 0x800, 1);
            g.renderFlag(flags, 8, 1);
            g.renderFlag(flags, 0x10, 1);
            g.renderFlag(flags, 4, 1);
            g.renderFlag(flags, 0x4000, 0);
            const uint32_t shade = w.shadowColor(w.lightObject()) >> 24;
            w.timer();
            uint32_t sumX = 0, sumY = 0;
            bool any = false;
            // Quads the liquid batcher holds come first, with its texture, as the reflection quads do.
            g.flush(view + w.batcherOffset, device);
            g_batch.texture = *batcherTexture;
            for (int32_t i = 0; i < count; ++i)
            {
                uint8_t* cell = cells + i * 0x98;
                const uint8_t* m = material(view, cell, light);
                if (!m || !m[0xCC])
                {
                    continue;
                }
                const float* c = reinterpret_cast<const float*>(cell + 0x18);
                const int32_t cx = *reinterpret_cast<const int32_t*>(cell + 0xC), cy = *reinterpret_cast<const int32_t*>(cell + 0x10);
                Vertex* t = reinterpret_cast<Vertex*>(view + 0x81640 + ((cx & 1) + (cy & 1) * 2) * 0x80);
                t[0].x = shift + c[0];
                t[0].y = c[1];
                t[1].x = c[8];
                t[1].y = shift + c[9];
                t[2].x = c[0x10];
                t[2].y = c[0x11] - shift;
                t[3].x = c[0x18] - shift;
                t[3].y = c[0x19];
                const uint8_t* tile = *reinterpret_cast<uint8_t* const*>(cell);
                const uint8_t depth[4] = {0x10, 0x11, 0x13, 0x12};
                for (int k = 0; k < 4; ++k)
                {
                    const uint32_t g8 = clampByte(-static_cast<int8_t>(tile[depth[k]]) * 8) * shade >> 8;
                    t[k].color = (g8 << 8 | g8) << 8 | g8;
                }
                const int16_t regionIndex = *reinterpret_cast<const int16_t*>(cell + 0x14);
                const uint8_t* region = *reinterpret_cast<uint8_t* const*>(view + 0x970B0 + regionIndex * 4);
                sumX += *reinterpret_cast<const uint32_t*>(region + 0x44) + *reinterpret_cast<const uint32_t*>(region + 0x34) + cx;
                sumY += *reinterpret_cast<const uint32_t*>(region + 0x48) + *reinterpret_cast<const uint32_t*>(region + 0x38) + cy;
                any = true;
                if (onScreen(t))
                {
                    g_batch.add(proxy, t);
                }
            }
            if (any)
            {
                const uint32_t ax = sumX / static_cast<uint32_t>(count), ay = sumY / static_cast<uint32_t>(count);
                const float in[3] = {static_cast<float>(ax), static_cast<float>(ay), 0.0f};
                float out[3] = {};
                w.mapToWorld(out, in);
                const int32_t y = ftol(out[1]);
                const int32_t x = ftol(out[0]);
                waterSound(static_cast<uint16_t>(count), x, y);
            }
            else
            {
                waterSound(0, 0, 0);
            }
            g_batch.draw(proxy);
        }
        g.renderFlag(flags, 0x800, 1);
        g.renderFlag(flags, 8, 0);
        g.renderFlag(flags, 0x10, 0);
        const float halfW = **w.halfW, halfH = **w.halfH;
        const int32_t centerX = *w.centerX, centerY = *w.centerY;
        uint32_t texture = *batcherTexture;
        g_batch.texture = texture;
        for (int32_t i = 0; i < count; ++i)
        {
            uint8_t* cell = cells + i * 0x98;
            const uint8_t* m = material(view, cell, light);
            if (!m)
            {
                continue;
            }
            int32_t frameIndex = 0;
            if (*w.detail >= 1)
            {
                const int32_t rate = *reinterpret_cast<const int32_t*>(m + 0xC8);
                const uint8_t* t = w.timer();
                const int64_t counter = *reinterpret_cast<const int64_t*>(t + 0x10);
                const int64_t frequency = *reinterpret_cast<const int64_t*>(t + 0x20);
                const uint32_t ms = static_cast<uint32_t>(counter * 1000 / frequency);
                const int64_t phase = (ms >> 1) & 0x3FF;
                frameIndex = static_cast<int32_t>(static_cast<double>(phase * rate) * *w.frameRate);
            }
            const float cellX = *reinterpret_cast<const float*>(cell + 0x4);
            const float cellY = *reinterpret_cast<const float*>(cell + 0x8);
            const int32_t ix = *reinterpret_cast<const int32_t*>(cell + 0xC), iy = *reinterpret_cast<const int32_t*>(cell + 0x10);
            Vertex* t = reinterpret_cast<Vertex*>(view + 0x80E40 + ((ix & 3) + (iy & 3) * 4) * 0x80);
            const float* offsets = reinterpret_cast<const float*>(view + 0xA90);
            for (int k = 0; k < 4; ++k)
            {
                t[k].x = offsets[k * 2] + cellX;
                t[k].y = cellY + offsets[k * 2 + 1];
            }
            uint32_t colors[4] = {0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF};
            if (lit)
            {
                const int32_t bx = ftol((cellX - halfW) * z) + centerX;
                const int32_t by = ftol((cellY - halfH) * z) + centerY;
                const int32_t* lightOffsets = reinterpret_cast<const int32_t*>(view + 0xAB0);
                for (int k = 0; k < 4; ++k)
                {
                    colors[k] = g.lightAt(light, bx + lightOffsets[k * 2], lightOffsets[k * 2 + 1] + by, 0);
                }
            }
            const uint8_t* tile = *reinterpret_cast<uint8_t* const*>(cell);
            const int32_t factor = *reinterpret_cast<const int32_t*>(m + 0xD0);
            const uint8_t depth[4] = {0x10, 0x11, 0x13, 0x12};
            for (int k = 0; k < 4; ++k)
            {
                t[k].color = clampByte(static_cast<int8_t>(tile[depth[k]]) * factor) << 24 | colors[k];
            }
            const uint32_t handle = *reinterpret_cast<const uint32_t*>(m + frameIndex * 4);
            if (handle != texture)
            {
                g_batch.draw(proxy);
                texture = handle;
                g_batch.texture = handle;
                *batcherTexture = handle;
            }
            if (onScreen(t))
            {
                g_batch.add(proxy, t);
            }
            ++*reinterpret_cast<uint32_t*>(view + 0x97098);
        }
        g_batch.draw(proxy);
        static bool s_logged = false;
        if (!s_logged)
        {
            s_logged = true;
            LOG("World statics: first water drawn by SacredBild ({} cells)", count);
        }
    }

    bool resolveWater(uintptr_t f)
    {
        struct Check
        {
            uint32_t offset;
            std::initializer_list<uint8_t> bytes;
        };
        const Check checks[] = {
            {0x008, {0xE8}}, {0x013, {0xE8}}, {0x038, {0xD9, 0x05}}, {0x056, {0xA0}}, {0x06F, {0xE8}}, {0x0AA, {0xE8}},
            {0x0B6, {0xE8}}, {0x2DB, {0x8D, 0x8F}}, {0x2E3, {0xE8}}, {0x34D, {0xE8}}, {0x371, {0xE8}}, {0x378, {0xE8}},
            {0x3A3, {0xE8}}, {0x496, {0xD8, 0x0D}}, {0x50C, {0xD8, 0x25}}, {0x520, {0xD8, 0x25}}, {0x528, {0x81, 0xC3}},
            {0x539, {0x05}}, {0x571, {0xE8}}, {0x68C, {0xE8}}, {0x6E3, {0xC2, 0x04, 0x00}},
            // The cell array and its count where this file reads them (ReBorn moved them).
            {0x044, {0x8B, 0x87, 0x3C, 0x0E, 0x08, 0x00}}, {0x0D7, {0x8D, 0xB7, 0x44, 0xFF, 0x03, 0x00}},
            {0x3E2, {0x8D, 0xAF, 0x3C, 0xFF, 0x03, 0x00}},
        };
        for (const Check& c : checks)
        {
            if (!Patch::verify(f + c.offset, c.bytes))
            {
                LOG("World statics: the water pass's code at +{:#x} is not the expected one; the game draws water", c.offset);
                return false;
            }
        }
        if (callAddr(f + 0x06F) != reinterpret_cast<uintptr_t>(g.renderFlag) ||
            callAddr(f + 0x571) != reinterpret_cast<uintptr_t>(g.lightAt) ||
            callAddr(f + 0x3A3) != reinterpret_cast<uintptr_t>(g.flush) ||
            callAddr(f + 0x0B6) != reinterpret_cast<uintptr_t>(g.timer))
        {
            LOG("World statics: the water pass calls other functions than expected; the game draws water");
            return false;
        }
        w.renderFlags = callTarget<decltype(w.renderFlags)>(f + 0x008);
        w.lightObject = callTarget<decltype(w.lightObject)>(f + 0x013);
        w.reflectShift = operand<const float*>(f + 0x03A);
        w.detail = operand<const uint8_t*>(f + 0x057);
        w.shadowColor = callTarget<decltype(w.shadowColor)>(f + 0x0AA);
        w.timer = callTarget<decltype(w.timer)>(f + 0x0B6);
        w.batcherOffset = operand<uint32_t>(f + 0x2DD);
        w.mapToWorld = callTarget<decltype(w.mapToWorld)>(f + 0x34D);
        w.soundManager = callTarget<decltype(w.soundManager)>(f + 0x371);
        w.soundUpdate = callTarget<void*>(f + 0x378);
        w.frameRate = operand<const float*>(f + 0x498);
        w.halfW = reinterpret_cast<const float* const*>(f + 0x50E);
        w.halfH = reinterpret_cast<const float* const*>(f + 0x522);
        w.centerX = reinterpret_cast<const int32_t*>(f + 0x52A);
        w.centerY = reinterpret_cast<const int32_t*>(f + 0x53A);
        return true;
    }

    bool resolve(uintptr_t s)
    {
        struct Check
        {
            uint32_t offset;
            std::initializer_list<uint8_t> bytes;
        };
        const Check checks[] = {
            {0x000, {0x8B, 0x4B, 0x04, 0x51, 0x8B, 0x4E, 0x04, 0xE8}},
            {0x015, {0x0F, 0x84}}, {0x01B, {0x8B, 0x0D}}, {0x022, {0xE8}}, {0x044, {0xE8}}, {0x070, {0xE8}},
            {0x08F, {0xE8}}, {0x0FB, {0xE8}}, {0x104, {0xA1}}, {0x125, {0xE8}}, {0x13F, {0xE8}}, {0x156, {0xE8}},
            {0x203, {0xD8, 0x25}}, {0x216, {0xD8, 0xB6, 0x60, 0x0B, 0x00, 0x00}}, {0x23B, {0xD8, 0x25}},
            {0x2CF, {0xD8, 0x1D}}, {0x2EC, {0xD8, 0x1D}}, {0x303, {0xD8, 0x1D}}, {0x354, {0xE8}}, {0x40F, {0xD9, 0x05}},
            {0x54A, {0xD8, 0x15}}, {0x56C, {0xD8, 0x0D}}, {0x572, {0xD9, 0x05}}, {0x5FF, {0xE8}}, {0x682, {0x8D, 0x8E}},
            {0x691, {0xB9}}, {0x696, {0xE8}}, {0x6A2, {0xD8, 0x05}}, {0x76F, {0xE8}}, {0x7F2, {0xD8, 0x1D}},
            // Miniatures (kind 2).
            {0x820, {0x8B, 0x4B, 0x04, 0x51, 0x8B, 0x4E, 0x04, 0xE8}}, {0x838, {0xE8}}, {0x8C4, {0xD8, 0x3D}},
            {0x932, {0xD8, 0x0D}}, {0x977, {0xE8}}, {0xA63, {0xD8, 0x0D}}, {0xB6C, {0xE8}}, {0xCE8, {0xE8}},
            {0xD4A, {0xDC, 0x1D}}, {0xED9, {0xE8}}, {0xF23, {0x68}}, {0xF28, {0xE8}}, {0xF2F, {0xE8}},
        };
        for (const Check& c : checks)
        {
            if (!Patch::verify(s + c.offset, c.bytes))
            {
                LOG("World statics: objects2's code at +{:#x} is not the expected one; the game draws static objects", c.offset);
                return false;
            }
        }
        g.staticObject = callTarget<decltype(g.staticObject)>(s + 0x007);
        g_continue = s + 0x015 + 6 + *reinterpret_cast<const int32_t*>(s + 0x017);
        g.sprites = operand<void**>(s + 0x01D);
        g.sprite = callTarget<decltype(g.sprite)>(s + 0x022);
        g.footprint = callTarget<decltype(g.footprint)>(s + 0x044);
        g.recordLight = callTarget<decltype(g.recordLight)>(s + 0x070);
        g.timer = callTarget<decltype(g.timer)>(s + 0x08F);
        g.frames = callTarget<decltype(g.frames)>(s + 0x0FB);
        g.worldFlags = operand<const uint32_t*>(s + 0x105);
        g.frame = callTarget<decltype(g.frame)>(s + 0x125);
        g.frameQuads = callTarget<decltype(g.frameQuads)>(s + 0x13F);
        g.renderFlag = callTarget<decltype(g.renderFlag)>(s + 0x156);
        g.halfW = reinterpret_cast<const float* const*>(s + 0x205);
        g.halfH = reinterpret_cast<const float* const*>(s + 0x23D);
        g.width = reinterpret_cast<const float* const*>(s + 0x2D1);
        g.zero = operand<const float*>(s + 0x2EE);
        g.height = reinterpret_cast<const float* const*>(s + 0x305);
        g.lightAt = callTarget<decltype(g.lightAt)>(s + 0x354);
        g.fadeRadius = operand<const float*>(s + 0x411);
        g.one = operand<const float*>(s + 0x54C);
        g.fadeMin = operand<const float*>(s + 0x56E);
        g.full = operand<const float*>(s + 0x574);
        g.spriteDepth = callTarget<decltype(g.spriteDepth)>(s + 0x5FF);
        g.batcherOffset = operand<uint32_t>(s + 0x684);
        g.filter = operand<void*>(s + 0x692);
        g.halfTexel = callTarget<decltype(g.halfTexel)>(s + 0x696);
        g.texel = operand<const float*>(s + 0x6A4);
        g.flush = callTarget<decltype(g.flush)>(s + 0x76F);
        g.fadeMax = operand<const float*>(s + 0x7F4);
        g_resumeCall = s + 0x007;
        // The miniature code calls the same functions (checked: its calls go where the static object's go).
        if (callAddr(s + 0x827) != reinterpret_cast<uintptr_t>(g.staticObject) ||
            callAddr(s + 0x838) != reinterpret_cast<uintptr_t>(g.sprite) ||
            callAddr(s + 0x977) != reinterpret_cast<uintptr_t>(g.timer) ||
            callAddr(s + 0xB6C) != reinterpret_cast<uintptr_t>(g.recordLight) ||
            callAddr(s + 0xCE8) != reinterpret_cast<uintptr_t>(g.lightAt))
        {
            LOG("World statics: objects2's miniature code calls other functions than expected; the game draws static objects");
            return false;
        }
        g.cellSpan = operand<const float*>(s + 0x8C6);
        g.angleStep = operand<const float*>(s + 0x934);
        g.halfFactor = operand<const float*>(s + 0xA65);
        g.linearZoom = operand<const double*>(s + 0xD4C);
        g.lightFlags = callTarget<decltype(g.lightFlags)>(s + 0xED9);
        g.glowName = operand<const char*>(s + 0xF24);
        g.texturePak = callTarget<decltype(g.texturePak)>(s + 0xF28);
        g.textureHandle = callTarget<decltype(g.textureHandle)>(s + 0xF2F);
        g_resumeCall2 = s + 0x827;
        return true;
    }
}

void WorldStatics::installWater()
{
    if (!Config::ddraw.nvrhi || !Config::render.takeOverStatics || !Addr::objects2StaticSprite ||
        !Addr::cWorldView_drawWaterTiles || !Addr::cTextureManager_get || !Addr::g_pTextureManager)
    {
        return;
    }
    // The functions it shares with the static objects are taken from objects2's code.
    if (!resolve(Addr::objects2StaticSprite) || !resolveWater(Addr::cWorldView_drawWaterTiles))
    {
        return;
    }
    Patch::begin();
    const bool hooked = Patch::hook(g_origWater, Addr::cWorldView_drawWaterTiles, &water, "cWorldView::drawWaterTiles");
    if (Patch::commit() && hooked)
    {
        LOG("World statics: SacredBild draws the water and lava surfaces");
    }
}

void WorldStatics::install()
{
    if (!Config::ddraw.nvrhi || !Config::render.takeOverStatics || !Addr::objects2StaticSprite || !Addr::cTextureManager_get ||
        !Addr::g_pTextureManager)
    {
        return;
    }
    const uintptr_t s = Addr::objects2StaticSprite;
    if (!resolve(s))
    {
        return;
    }
    // jmp stub; nop; nop (over mov ecx,[ebx+4]; push ecx; mov ecx,[esi+4])
    auto jump = [](uintptr_t at, void (*to)()) {
        uint8_t code[7] = {0xE9, 0, 0, 0, 0, 0x90, 0x90};
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(to) - (at + 5));
        std::memcpy(code + 1, &rel, sizeof(rel));
        return Patch::write(at, code, sizeof(code));
    };
    if (resolveFootprint(reinterpret_cast<uintptr_t>(g.footprint)))
    {
        g_origFootprint = g.footprint;
        if (Patch::hook(g_origFootprint, &footprint, "cWorldView::drawFootprint"))
        {
            LOG("World statics: SacredBild draws the blob shadows of static objects");
        }
    }
    if (Addr::objects1StaticSprite && resolvePass1(Addr::objects1StaticSprite) && jump(Addr::objects1StaticSprite, &stub1) &&
        jump(Addr::objects1StaticSprite + 0x805, &stub1Mini))
    {
        LOG("World statics: SacredBild draws the first object pass's static objects and miniatures");
    }
    if (jump(s, &stub) && jump(s + 0x820, &stub2))
    {
        g_site = s;
        LOG("World statics: SacredBild draws the draw lists' static objects and miniatures");
    }
}
