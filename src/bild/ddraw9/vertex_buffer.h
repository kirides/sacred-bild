#pragma once
#include "ddraw9/d3d9_api.h"

namespace DDraw9
{
    // IDirect3DVertexBuffer7 on a dynamic Direct3D 9 vertex buffer. The lock flags DirectDraw and Direct3D 9 use for
    // appending and discarding have the same values.
    class VertexBuffer final : public IDirect3DVertexBuffer7
    {
    public:
        static HRESULT create(const D3DVERTEXBUFFERDESC& desc, VertexBuffer** out);
        static VertexBuffer* from(const void* iface);

        d9::IDirect3DVertexBuffer9* buffer() const { return m_buffer; }
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
        VertexBuffer(const D3DVERTEXBUFFERDESC& desc, d9::IDirect3DVertexBuffer9* buffer, UINT stride);
        ~VertexBuffer();

        LONG m_refs = 1;
        D3DVERTEXBUFFERDESC m_desc;
        d9::IDirect3DVertexBuffer9* m_buffer;
        UINT m_stride;
    };
}
