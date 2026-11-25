#include "ddraw9/vertex_buffer.h"
#include "ddraw9/gpu.h"
#include "render/fvf.h"
#include "log.h"

namespace DDraw9
{
    namespace
    {
        const void* g_vtable = nullptr;
    }

    VertexBuffer::VertexBuffer(const D3DVERTEXBUFFERDESC& desc, d9::IDirect3DVertexBuffer9* buffer, UINT stride)
        : m_desc(desc), m_buffer(buffer), m_stride(stride)
    {
        if (!g_vtable)
        {
            g_vtable = *reinterpret_cast<void* const*>(static_cast<IDirect3DVertexBuffer7*>(this));
        }
    }

    VertexBuffer::~VertexBuffer()
    {
        m_buffer->Release();
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
        d9::IDirect3DDevice9Ex* dev = Gpu::device();
        const UINT stride = Fvf::stride(in.dwFVF);
        if (!dev || !stride || !in.dwNumVertices)
        {
            return DDERR_INVALIDPARAMS;
        }
        // Always dynamic: appending (NOOVERWRITE) and discarding are what vertex buffers are locked for here.
        const DWORD usage = D3DUSAGE_DYNAMIC | ((in.dwCaps & D3DVBCAPS_WRITEONLY) ? D3DUSAGE_WRITEONLY : 0);
        d9::IDirect3DVertexBuffer9* buffer = nullptr;
        const HRESULT hr = dev->CreateVertexBuffer(in.dwNumVertices * stride, usage, 0, d9::D3DPOOL_DEFAULT, &buffer, nullptr);
        if (FAILED(hr))
        {
            LOG("Direct3D 9: creating a vertex buffer ({} vertices, FVF {:x}) failed ({:08x})", in.dwNumVertices, in.dwFVF,
                static_cast<uint32_t>(hr));
            return hr == D3DERR_OUTOFVIDEOMEMORY ? DDERR_OUTOFVIDEOMEMORY : DDERR_GENERIC;
        }
        D3DVERTEXBUFFERDESC desc = in;
        desc.dwSize = sizeof(desc);
        *out = new VertexBuffer(desc, buffer, stride);
        return D3D_OK;
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
        DWORD lockFlags = flags & (DDLOCK_DISCARDCONTENTS | DDLOCK_NOOVERWRITE);    // == D3DLOCK_DISCARD, D3DLOCK_NOOVERWRITE
        if ((flags & DDLOCK_READONLY) && !(m_desc.dwCaps & D3DVBCAPS_WRITEONLY))
        {
            lockFlags |= D3DLOCK_READONLY;
        }
        const HRESULT hr = m_buffer->Lock(0, 0, data, lockFlags);
        if (FAILED(hr))
        {
            *data = nullptr;
            return DDERR_SURFACEBUSY;
        }
        if (size)
        {
            *size = m_desc.dwNumVertices * m_stride;
        }
        return D3D_OK;
    }

    HRESULT VertexBuffer::Unlock()
    {
        return SUCCEEDED(m_buffer->Unlock()) ? D3D_OK : DDERR_NOTLOCKED;
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
