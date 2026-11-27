#pragma once
#include "ddraw9/d3d9_api.h"

#include <cstdint>
#include <vector>

namespace DDrawRhi
{
    // IDirect3DVertexBuffer7 in system memory: draws from it copy the vertices they use into the frame's upload ring
    // (Context::upload), as draws from user pointers do. Appending (NOOVERWRITE) and discarding need nothing else then.
    class VertexBuffer final : public IDirect3DVertexBuffer7
    {
    public:
        static HRESULT create(const D3DVERTEXBUFFERDESC& desc, VertexBuffer** out);
        static VertexBuffer* from(const void* iface);

        const uint8_t* data() const { return m_data.data(); }
        DWORD vertices() const { return m_desc.dwNumVertices; }
        DWORD fvf() const { return m_desc.dwFVF; }
        UINT stride() const { return m_stride; }

        STDMETHOD(QueryInterface)(REFIID riid, LPVOID* out) override;
        STDMETHOD_(ULONG, AddRef)() override;
        STDMETHOD_(ULONG, Release)() override;
        STDMETHOD(Lock)(DWORD flags, LPVOID* data, LPDWORD size) override;
        STDMETHOD(Unlock)() override;
        STDMETHOD(ProcessVertices)(DWORD op, DWORD dstIndex, DWORD count, LPDIRECT3DVERTEXBUFFER7 src, DWORD srcIndex,
            LPDIRECT3DDEVICE7 device, DWORD flags) override;
        STDMETHOD(GetVertexBufferDesc)(LPD3DVERTEXBUFFERDESC desc) override;
        STDMETHOD(Optimize)(LPDIRECT3DDEVICE7 device, DWORD flags) override;
        STDMETHOD(ProcessVerticesStrided)(DWORD op, DWORD dstIndex, DWORD count, LPD3DDRAWPRIMITIVESTRIDEDDATA data,
            DWORD fvf, LPDIRECT3DDEVICE7 device, DWORD flags) override;

    private:
        VertexBuffer(const D3DVERTEXBUFFERDESC& desc, UINT stride);
        ~VertexBuffer() = default;

        LONG m_refs = 1;
        D3DVERTEXBUFFERDESC m_desc;
        UINT m_stride;
        std::vector<uint8_t> m_data;
        bool m_locked = false;
    };
}
