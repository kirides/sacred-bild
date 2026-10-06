#include "overlay/prompts.h"
#include "ddraw9/gpu.h"
#include "log.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace
{
    // The atlas (tools/gen_prompts.py): 64x64 cells, 8 per row, 18 per style, then the "+"; premultiplied alpha.
    constexpr int kAtlasResource = 201;     // RCDATA in version.rc
    constexpr int kAtlasWidth = 512, kAtlasHeight = 512;
    constexpr int kCell = 64;
    constexpr int kColumns = 8;
    constexpr int kPerStyle = 18;
    constexpr int kPlusCell = 3 * kPerStyle;
    constexpr float kPlusWidth = 0.55f;     // of a button's size
    constexpr float kGap = 0.04f;
    constexpr DWORD kStaleMs = 250;         // prompts not renewed for this long are not drawn

    struct Vertex
    {
        float x, y, z, rhw;
        DWORD color;
        float u, v;
    };
    constexpr DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

    // The presenting thread's.
    std::vector<Vertex> g_building;
    Gamepad::Style g_style = Gamepad::Style::Xbox;

    std::mutex g_lock;
    std::vector<Vertex> g_ready;
    DWORD g_readyTick = 0;

    // The device's, created on first draw.
    d9::IDirect3DDevice9Ex* g_device = nullptr;
    d9::IDirect3DTexture9* g_texture = nullptr;
    bool g_failed = false;

    // The cell of a single button (or stick) in the current style; -1 if there is none.
    int cellOf(uint32_t button)
    {
        if (!button || (button & (button - 1)))
        {
            return -1;
        }
        unsigned long bit = 0;
        _BitScanForward(&bit, button);
        return bit < kPerStyle ? static_cast<int>(g_style) * kPerStyle + static_cast<int>(bit) : -1;
    }

    void quad(int cell, float x, float y, float w, float h, float alpha)
    {
        if (cell < 0)
        {
            return;
        }
        const float u0 = static_cast<float>(cell % kColumns) * kCell, v0 = static_cast<float>(cell / kColumns) * kCell;
        // Premultiplied: the vertex color scales all four channels.
        const DWORD a = static_cast<DWORD>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
        const DWORD color = (a << 24) | (a << 16) | (a << 8) | a;
        // Pixel centers: Direct3D 9 samples a pretransformed pixel at its top-left corner.
        const float l = x - 0.5f, t = y - 0.5f, r = x + w - 0.5f, b = y + h - 0.5f;
        constexpr float su = 1.0f / kAtlasWidth, sv = 1.0f / kAtlasHeight;
        const Vertex v[4] = {
            {l, t, 0.0f, 1.0f, color, u0 * su, v0 * sv},
            {r, t, 0.0f, 1.0f, color, (u0 + kCell) * su, v0 * sv},
            {l, b, 0.0f, 1.0f, color, u0 * su, (v0 + kCell) * sv},
            {r, b, 0.0f, 1.0f, color, (u0 + kCell) * su, (v0 + kCell) * sv},
        };
        for (int i : {0, 1, 2, 2, 1, 3})
        {
            g_building.push_back(v[i]);
        }
    }

    // The atlas PNG from the DLL's resources, as a texture of the device.
    bool createTexture(d9::IDirect3DDevice9Ex* device)
    {
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&createTexture), &module);
        HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(kAtlasResource), MAKEINTRESOURCEW(10));   // RT_RCDATA
        HGLOBAL loaded = resource ? LoadResource(module, resource) : nullptr;
        const void* png = loaded ? LockResource(loaded) : nullptr;
        if (!png)
        {
            LOG("Prompts: atlas resource missing");
            return false;
        }
        SDL_Surface* decoded = SDL_LoadPNG_IO(SDL_IOFromConstMem(png, SizeofResource(module, resource)), true);
        SDL_Surface* bgra = decoded ? SDL_ConvertSurface(decoded, SDL_PIXELFORMAT_ARGB8888) : nullptr;
        SDL_DestroySurface(decoded);
        if (!bgra || bgra->w != kAtlasWidth || bgra->h != kAtlasHeight)
        {
            LOG("Prompts: atlas not decoded ({})", bgra ? "unexpected size" : SDL_GetError());
            SDL_DestroySurface(bgra);
            return false;
        }
        d9::IDirect3DTexture9* staging = nullptr;
        bool ok = SUCCEEDED(device->CreateTexture(bgra->w, bgra->h, 1, 0, d9::D3DFMT_A8R8G8B8, d9::D3DPOOL_SYSTEMMEM,
                      &staging, nullptr)) &&
            SUCCEEDED(device->CreateTexture(bgra->w, bgra->h, 1, 0, d9::D3DFMT_A8R8G8B8, d9::D3DPOOL_DEFAULT,
                &g_texture, nullptr));
        d9::D3DLOCKED_RECT locked = {};
        if (ok && SUCCEEDED(staging->LockRect(0, &locked, nullptr, 0)))
        {
            for (int y = 0; y < bgra->h; ++y)
            {
                memcpy(static_cast<uint8_t*>(locked.pBits) + y * locked.Pitch,
                    static_cast<const uint8_t*>(bgra->pixels) + y * bgra->pitch, bgra->w * 4);
            }
            staging->UnlockRect(0);
            ok = SUCCEEDED(device->UpdateTexture(staging, g_texture));
        }
        else
        {
            ok = false;
        }
        SDL_DestroySurface(bgra);
        if (staging)
        {
            staging->Release();
        }
        if (!ok)
        {
            LOG("Prompts: no atlas texture");
            if (g_texture)
            {
                g_texture->Release();
                g_texture = nullptr;
            }
            return false;
        }
        LOG("Prompts: atlas loaded");
        return true;
    }
}

void Prompts::begin(Gamepad::Style style)
{
    g_building.clear();
    g_style = style;
}

float Prompts::width(Bindings::Binding binding, float size)
{
    if (!binding.button)
    {
        return 0.0f;
    }
    return binding.modifier ? size * (2.0f + kPlusWidth + 2.0f * kGap) : size;
}

float Prompts::add(Bindings::Binding binding, float x, float y, float size, float ax, float ay, float alpha)
{
    const float w = width(binding, size);
    if (w <= 0.0f)
    {
        return 0.0f;
    }
    float left = std::round(x - w * ax);
    const float top = std::round(y - size * ay);
    size = std::round(size);
    if (binding.modifier)
    {
        quad(cellOf(binding.modifier), left, top, size, size, alpha);
        left += size * (1.0f + kGap);
        const float plus = std::round(size * kPlusWidth);
        quad(kPlusCell, std::round(left), std::round(top + (size - plus) * 0.5f), plus, plus, alpha);
        left = std::round(left + size * (kPlusWidth + kGap));
    }
    quad(cellOf(binding.button), left, top, size, size, alpha);
    return w;
}

void Prompts::addStacked(Bindings::Binding binding, float x, float bottom, float size, float alpha)
{
    if (!binding.button)
    {
        return;
    }
    size = std::round(size);
    const float left = std::round(x - size * 0.5f);
    float top = std::round(bottom - size);
    quad(cellOf(binding.button), left, top, size, size, alpha);
    if (binding.modifier)
    {
        const float plus = std::round(size * kPlusWidth);
        top -= std::round(size * kGap) + plus;
        quad(kPlusCell, std::round(x - plus * 0.5f), top, plus, plus, alpha);
        top -= std::round(size * (1.0f + kGap));
        quad(cellOf(binding.modifier), left, top, size, size, alpha);
    }
}

void Prompts::end()
{
    std::scoped_lock lock(g_lock);
    g_ready.swap(g_building);
    g_readyTick = GetTickCount();
}

bool Prompts::pending()
{
    std::scoped_lock lock(g_lock);
    return !g_ready.empty() && GetTickCount() - g_readyTick < kStaleMs;
}

void Prompts::draw(d9::IDirect3DDevice9Ex* device, float width, float height)
{
    std::vector<Vertex> vertices;
    {
        std::scoped_lock lock(g_lock);
        if (g_ready.empty() || GetTickCount() - g_readyTick >= kStaleMs)
        {
            return;
        }
        vertices = g_ready;
    }
    if (device != g_device)
    {
        if (g_texture)
        {
            g_texture->Release();
            g_texture = nullptr;
        }
        g_device = device;
        g_failed = !createTexture(device);
    }
    if (g_failed || !g_texture)
    {
        return;
    }
    d9::IDirect3DStateBlock9* saved = nullptr;
    if (FAILED(device->CreateStateBlock(d9::D3DSBT_ALL, &saved)))
    {
        return;
    }
    const d9::D3DVIEWPORT9 viewport = {0, 0, static_cast<DWORD>(width), static_cast<DWORD>(height), 0.0f, 1.0f};
    device->SetViewport(&viewport);
    device->SetPixelShader(nullptr);
    device->SetVertexShader(nullptr);
    device->SetFVF(kFvf);
    device->SetTexture(0, g_texture);
    device->SetTexture(1, nullptr);
    device->SetRenderState(d9::D3DRS_ZENABLE, FALSE);
    device->SetRenderState(d9::D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(d9::D3DRS_CULLMODE, d9::D3DCULL_NONE);
    device->SetRenderState(d9::D3DRS_LIGHTING, FALSE);
    device->SetRenderState(d9::D3DRS_FOGENABLE, FALSE);
    device->SetRenderState(d9::D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(d9::D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(d9::D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(d9::D3DRS_SRGBWRITEENABLE, FALSE);
    device->SetRenderState(d9::D3DRS_FILLMODE, d9::D3DFILL_SOLID);
    device->SetRenderState(d9::D3DRS_SHADEMODE, d9::D3DSHADE_GOURAUD);
    device->SetRenderState(d9::D3DRS_COLORWRITEENABLE, 0xF);
    device->SetRenderState(d9::D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(d9::D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    device->SetRenderState(d9::D3DRS_BLENDOP, d9::D3DBLENDOP_ADD);
    device->SetRenderState(d9::D3DRS_SRCBLEND, d9::D3DBLEND_ONE);
    device->SetRenderState(d9::D3DRS_DESTBLEND, d9::D3DBLEND_INVSRCALPHA);
    device->SetTextureStageState(0, d9::D3DTSS_COLOROP, d9::D3DTOP_MODULATE);
    device->SetTextureStageState(0, d9::D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, d9::D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, d9::D3DTSS_ALPHAOP, d9::D3DTOP_MODULATE);
    device->SetTextureStageState(0, d9::D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, d9::D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, d9::D3DTSS_TEXCOORDINDEX, 0);
    device->SetTextureStageState(0, d9::D3DTSS_TEXTURETRANSFORMFLAGS, d9::D3DTTFF_DISABLE);
    device->SetTextureStageState(1, d9::D3DTSS_COLOROP, d9::D3DTOP_DISABLE);
    device->SetTextureStageState(1, d9::D3DTSS_ALPHAOP, d9::D3DTOP_DISABLE);
    device->SetSamplerState(0, d9::D3DSAMP_MINFILTER, d9::D3DTEXF_LINEAR);
    device->SetSamplerState(0, d9::D3DSAMP_MAGFILTER, d9::D3DTEXF_LINEAR);
    device->SetSamplerState(0, d9::D3DSAMP_MIPFILTER, d9::D3DTEXF_NONE);
    device->SetSamplerState(0, d9::D3DSAMP_ADDRESSU, d9::D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, d9::D3DSAMP_ADDRESSV, d9::D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, d9::D3DSAMP_SRGBTEXTURE, FALSE);
    device->DrawPrimitiveUP(d9::D3DPT_TRIANGLELIST, static_cast<UINT>(vertices.size() / 3), vertices.data(),
        sizeof(Vertex));
    saved->Apply();
    saved->Release();
}
