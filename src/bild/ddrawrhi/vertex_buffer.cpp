#include "ddrawrhi/vertex_buffer.h"
#include "ddrawrhi/context.h"
#include "render/fvf.h"
#include "log.h"

namespace DDrawRhi
{
    namespace
    {
        const void* g_vtable = nullptr;
    }

    VertexBuffer::VertexBuffer(const D3DVERTEXBUFFERDESC& desc, UINT stride)
        : m_desc(desc), m_stride(stride), m_data(size_t(desc.dwNumVertices) * stride)
    {
        if (!g_vtable)
        {
            g_vtable = *reinterpret_cast<void* const*>(static_cast<IDirect3DVertexBuffer7*>(this));
        }
    }

    VertexBuffer* VertexBuffer::from(const void* iface)
    {
        if (iface && g_vtable && *static_cast<const void* const*>(iface) == g_vtable)
        {
            return static_cast<VertexBuffer*>(static_cast<IDirect3DVertexBuffer7*>(const_cast<void*>(iface)));
        }
        return nullptr;
    }

    HRESULT VertexBuffer::create(const D3DVERTEXBUFFERDESC& in, VertexBuffer** out)
    {
        *out = nullptr;
        const UINT stride = Fvf::stride(in.dwFVF);
        if (!stride || !in.dwNumVertices || in.dwNumVertices > (64u << 20) / stride)
        {
            return DDERR_INVALIDPARAMS;
        }
        D3DVERTEXBUFFERDESC desc = in;
        desc.dwSize = sizeof(desc);
        *out = new (std::nothrow) VertexBuffer(desc, stride);
        return *out ? D3D_OK : DDERR_OUTOFMEMORY;
    }

    HRESULT VertexBuffer::QueryInterface(REFIID riid, LPVOID* out)
    {
        if (!out)
        {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IDirect3DVertexBuffer7)
        {
            *out = static_cast<IDirect3DVertexBuffer7*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

    ULONG VertexBuffer::AddRef()
    {
        return static_cast<ULONG>(InterlockedIncrement(&m_refs));
    }

    ULONG VertexBuffer::Release()
    {
        const LONG refs = InterlockedDecrement(&m_refs);
        if (refs == 0)
        {
            delete this;
        }
        return static_cast<ULONG>(refs);
    }

    HRESULT VertexBuffer::Lock(DWORD flags, LPVOID* data, LPDWORD size)
    {
        if (!data)
        {
            return DDERR_INVALIDPARAMS;
        }
        (void)flags;    // draws copy what they use when they are made: discarding and appending are free
        *data = m_data.data();
        m_locked = true;
        if (size)
        {
            *size = m_desc.dwNumVertices * m_stride;
        }
        return D3D_OK;
    }

    HRESULT VertexBuffer::Unlock()
    {
        if (!m_locked)
        {
            return DDERR_NOTLOCKED;
        }
        m_locked = false;
        return D3D_OK;
    }

    HRESULT VertexBuffer::ProcessVertices(DWORD, DWORD, DWORD, LPDIRECT3DVERTEXBUFFER7, DWORD, LPDIRECT3DDEVICE7, DWORD)
    {
        unsupported("IDirect3DVertexBuffer7::ProcessVertices");
        return DDERR_UNSUPPORTED;
    }

    HRESULT VertexBuffer::GetVertexBufferDesc(LPD3DVERTEXBUFFERDESC desc)
    {
        if (!desc)
        {
            return DDERR_INVALIDPARAMS;
        }
        *desc = m_desc;
        return D3D_OK;
    }

    HRESULT VertexBuffer::Optimize(LPDIRECT3DDEVICE7, DWORD)
    {
        return D3D_OK;
    }

    HRESULT VertexBuffer::ProcessVerticesStrided(DWORD, DWORD, DWORD, LPD3DDRAWPRIMITIVESTRIDEDDATA, DWORD, LPDIRECT3DDEVICE7, DWORD)
    {
        unsupported("IDirect3DVertexBuffer7::ProcessVerticesStrided");
        return DDERR_UNSUPPORTED;
    }
}
