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

#include <cstdint>
#include <cstring>
#include <vector>

namespace
{
    using namespace Sacred;

    uintptr_t g_site = 0;
    uintptr_t g_continue = 0;   // the entry loop's next entry
    uintptr_t g_resumeCall = 0; // the game's own code for the entry (its call of staticObject)

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
    } g;

    template <class T>
    T callTarget(uintptr_t at)
    {
        return reinterpret_cast<T>(at + 5 + *reinterpret_cast<const int32_t*>(at + 1));
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
    struct Batch
    {
        std::vector<Vertex> vertices;
        std::vector<uint16_t> indices;
        uint32_t texture = 0;

        void draw(DeviceProxy* proxy)
        {
            if (vertices.empty())
            {
                return;
            }
            const size_t quads = vertices.size() / 4;
            if (indices.size() < quads * 6)
            {
                const size_t from = indices.size() / 6;
                indices.resize(quads * 6);
                for (size_t q = from; q < quads; ++q)
                {
                    const uint16_t b = static_cast<uint16_t>(q * 4);
                    uint16_t* i = &indices[q * 6];
                    i[0] = b; i[1] = b + 1; i[2] = b + 2; i[3] = b + 2; i[4] = b + 1; i[5] = b + 3;
                }
            }
            proxy->drawQuads(&lookup, texture, 0, cSpriteBatcher::fvf, vertices.data(), static_cast<DWORD>(vertices.size()),
                indices.data(), static_cast<DWORD>(quads * 6));
            vertices.clear();
        }
    };
    Batch g_batch;

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

    struct Entry
    {
        uint32_t kind;
        uint32_t id;
        float x, y;
    };

    // The entry as the game draws it; false: not this device (the game's code runs).
    bool draw(uint8_t* view, const Entry* entry, IDirect3DDevice7* device, void* flags, const uint8_t* frame)
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
        void* light = *reinterpret_cast<void* const*>(frame + 0x34);
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
        const bool lit = frame[0x1B] != 0;
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
            Vertex v[4] = {};
            v[0].x = v[1].x = left;
            v[2].x = v[3].x = right;
            v[0].y = v[2].y = top;
            v[1].y = v[3].y = bottom;
            for (Vertex& p : v)
            {
                p.rhw = 1.0f;
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
                g_batch.vertices.insert(g_batch.vertices.end(), v, v + 4);
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
        return draw(view, entry, device, flags, frame);
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
        return true;
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
    uint8_t jump[7] = {0xE9, 0, 0, 0, 0, 0x90, 0x90};   // jmp stub; nop; nop (over mov ecx,[ebx+4]; push ecx; mov ecx,[esi+4])
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&stub) - (s + 5));
    std::memcpy(jump + 1, &rel, sizeof(rel));
    if (Patch::write(s, jump, sizeof(jump)))
    {
        g_site = s;
        LOG("World statics: SacredBild draws the draw lists' static objects");
    }
}
