#include "rhi/backend.h"

#include "log.h"

#include <directx/d3d12.h>
#include <dxgi1_6.h>
#include <dxgidebug.h>
#include <nvrhi/d3d12.h>

#include <string>

// Direct3D 12 for Rhi: a DXGI factory and adapter, the device and its direct queue, and a flip-model swap chain on the
// game window with a frame-latency waitable object. d3d12.dll and dxgi.dll are delay-loaded (ddraw.dll must load where
// they are missing), so they are loaded by hand first: a delay-load failure would be an exception.
namespace Rhi::Detail
{
    namespace
    {
        template <class T>
        struct Com
        {
            T* p = nullptr;
            Com() = default;
            Com(const Com&) = delete;
            Com& operator=(const Com&) = delete;
            ~Com() { reset(); }
            void reset()
            {
                if (p)
                {
                    p->Release();
                    p = nullptr;
                }
            }
            T** put()
            {
                reset();
                return &p;
            }
            T* operator->() const { return p; }
            explicit operator bool() const { return p != nullptr; }
        };

        std::string narrow(const wchar_t* text)
        {
            std::string out;
            for (; *text; ++text)
            {
                out += *text < 0x80 ? static_cast<char>(*text) : '?';
            }
            return out;
        }

        constexpr UINT kImages = 3;

        class D3D12Backend final : public Backend
        {
        public:
            ~D3D12Backend() override
            {
                if (m_queue && m_d3d)
                {
                    flushQueue();
                }
                releaseImages();
                m_device = nullptr;
                if (m_waitable)
                {
                    CloseHandle(m_waitable);
                }
            }

            bool create(HWND window, uint32_t width, uint32_t height, const Settings& settings,
                nvrhi::IMessageCallback* messages) override
            {
                if (!LoadLibraryW(L"d3d12.dll") || !LoadLibraryW(L"dxgi.dll"))
                {
                    LOG("RHI: Direct3D 12: d3d12.dll or dxgi.dll missing (needs Windows 10 or later)");
                    return false;
                }
                m_vsync = settings.vsync;
                UINT factoryFlags = 0;
                if (settings.validation)
                {
                    Com<ID3D12Debug> debug;
                    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.put()))))
                    {
                        debug->EnableDebugLayer();
                        factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
                    }
                    else
                    {
                        LOG("RHI: Direct3D 12: debug layer not installed (Graphics Tools optional feature)");
                    }
                }
                HRESULT hr = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(m_factory.put()));
                if (FAILED(hr))
                {
                    LOG("RHI: Direct3D 12: CreateDXGIFactory2 failed ({:08x})", static_cast<uint32_t>(hr));
                    return false;
                }
                if (!pickAdapter())
                {
                    return false;
                }

                D3D12_COMMAND_QUEUE_DESC queueDesc = {};
                queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
                hr = m_d3d->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(m_queue.put()));
                if (FAILED(hr))
                {
                    LOG("RHI: Direct3D 12: CreateCommandQueue failed ({:08x})", static_cast<uint32_t>(hr));
                    return false;
                }

                Com<IDXGIFactory5> factory5;
                BOOL tearing = FALSE;
                if (SUCCEEDED(m_factory->QueryInterface(IID_PPV_ARGS(factory5.put()))))
                {
                    factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
                }
                m_tearing = tearing != FALSE;

                DXGI_SWAP_CHAIN_DESC1 desc = {};
                desc.Width = width;
                desc.Height = height;
                desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                desc.SampleDesc.Count = 1;
                desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                desc.BufferCount = kImages;
                desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
                m_swapFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (m_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
                desc.Flags = m_swapFlags;
                Com<IDXGISwapChain1> swap1;
                hr = m_factory->CreateSwapChainForHwnd(m_queue.p, window, &desc, nullptr, nullptr, swap1.put());
                if (FAILED(hr) || FAILED(swap1->QueryInterface(IID_PPV_ARGS(m_swap.put()))))
                {
                    LOG("RHI: Direct3D 12: CreateSwapChainForHwnd failed ({:08x})", static_cast<uint32_t>(hr));
                    return false;
                }
                // The game handles Alt+Enter and its window itself.
                m_factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
                m_swap->SetMaximumFrameLatency(static_cast<UINT>(settings.maxFrameLatency < 1 ? 1 : settings.maxFrameLatency));
                m_waitable = m_swap->GetFrameLatencyWaitableObject();

                nvrhi::d3d12::DeviceDesc deviceDesc;
                deviceDesc.errorCB = messages;
                deviceDesc.pDevice = m_d3d.p;
                deviceDesc.pGraphicsCommandQueue = m_queue.p;
                if (settings.validation)
                {
                    m_d3d->QueryInterface(IID_PPV_ARGS(m_infoQueue.put()));
                }
                m_device = nvrhi::d3d12::createDevice(deviceDesc);
                if (!m_device)
                {
                    LOG("RHI: Direct3D 12: NVRHI device creation failed");
                    return false;
                }
                return wrapImages(width, height);
            }

            nvrhi::IDevice* device() override { return m_device; }
            const std::string& adapterName() const override { return m_adapterName; }
            uint32_t imageCount() const override { return kImages; }
            nvrhi::ITexture* image(uint32_t index) override { return index < kImages ? m_images[index].Get() : nullptr; }
            nvrhi::Format format() const override { return nvrhi::Format::RGBA8_UNORM; }

            bool acquire(uint32_t& index) override
            {
                if (m_waitable)
                {
                    WaitForSingleObjectEx(m_waitable, 1000, TRUE);
                }
                index = m_swap->GetCurrentBackBufferIndex();
                return true;
            }

            bool present() override
            {
                logDebugMessages();
                const UINT flags = !m_vsync && m_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
                const HRESULT hr = m_swap->Present(m_vsync ? 1 : 0, flags);
                if (FAILED(hr) && !m_presentFailed)
                {
                    m_presentFailed = true;
                    LOG("RHI: Direct3D 12: Present failed ({:08x}){}", static_cast<uint32_t>(hr),
                        hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ? ": the GPU device was removed" : "");
                }
                return SUCCEEDED(hr);
            }

            bool resize(uint32_t width, uint32_t height) override
            {
                // NVRHI's waitForIdle waits for its own command lists, not for the presents queued after them, which
                // still reference the images.
                flushQueue();
                releaseImages();
                const HRESULT hr = m_swap->ResizeBuffers(kImages, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, m_swapFlags);
                logDebugMessages();
                if (FAILED(hr))
                {
                    LOG("RHI: Direct3D 12: ResizeBuffers failed ({:08x})", static_cast<uint32_t>(hr));
                    return false;
                }
                return wrapImages(width, height);
            }

        private:
            void flushQueue()
            {
                if (!m_fence)
                {
                    m_d3d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_fence.put()));
                }
                if (m_fence && SUCCEEDED(m_queue->Signal(m_fence.p, ++m_fenceValue)) && m_fence->GetCompletedValue() < m_fenceValue)
                {
                    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                    if (event && SUCCEEDED(m_fence->SetEventOnCompletion(m_fenceValue, event)))
                    {
                        WaitForSingleObject(event, 5000);
                    }
                    if (event)
                    {
                        CloseHandle(event);
                    }
                }
            }

            // With the debug layer: its messages (warnings and errors) into the log, at most a few hundred.
            void logDebugMessages()
            {
                if (!m_infoQueue)
                {
                    return;
                }
                const UINT64 count = m_infoQueue->GetNumStoredMessages();
                for (UINT64 i = 0; i < count && m_debugLogged < 300; ++i)
                {
                    SIZE_T size = 0;
                    m_infoQueue->GetMessage(i, nullptr, &size);
                    std::string buffer(size, char(0));
                    auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
                    if (SUCCEEDED(m_infoQueue->GetMessage(i, msg, &size)) && msg->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
                    {
                        ++m_debugLogged;
                        LOG("RHI: Direct3D 12 debug layer: {}", std::string_view(msg->pDescription, msg->DescriptionByteLength ? msg->DescriptionByteLength - 1 : 0));
                    }
                }
                m_infoQueue->ClearStoredMessages();
            }

            bool pickAdapter()
            {
                Com<IDXGIFactory6> factory6;
                const bool byPreference = SUCCEEDED(m_factory->QueryInterface(IID_PPV_ARGS(factory6.put())));
                for (UINT i = 0;; ++i)
                {
                    Com<IDXGIAdapter1> adapter;
                    const HRESULT hr = byPreference
                        ? factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(adapter.put()))
                        : m_factory->EnumAdapters1(i, adapter.put());
                    if (hr == DXGI_ERROR_NOT_FOUND)
                    {
                        break;
                    }
                    if (FAILED(hr))
                    {
                        continue;
                    }
                    DXGI_ADAPTER_DESC1 desc = {};
                    adapter->GetDesc1(&desc);
                    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                    {
                        continue;
                    }
                    if (SUCCEEDED(D3D12CreateDevice(adapter.p, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(m_d3d.put()))))
                    {
                        m_adapterName = narrow(desc.Description);
                        LOG("RHI: Direct3D 12: adapter {} ({} MB video memory)", m_adapterName, desc.DedicatedVideoMemory >> 20);
                        return true;
                    }
                }
                LOG("RHI: Direct3D 12: no adapter with feature level 11_0");
                return false;
            }

            bool wrapImages(uint32_t width, uint32_t height)
            {
                for (UINT i = 0; i < kImages; ++i)
                {
                    if (FAILED(m_swap->GetBuffer(i, IID_PPV_ARGS(m_buffers[i].put()))))
                    {
                        LOG("RHI: Direct3D 12: GetBuffer({}) failed", i);
                        return false;
                    }
                    nvrhi::TextureDesc desc;
                    desc.width = width;
                    desc.height = height;
                    desc.format = nvrhi::Format::RGBA8_UNORM;
                    desc.debugName = "swap chain image";
                    desc.isRenderTarget = true;
                    desc.initialState = nvrhi::ResourceStates::Present;
                    desc.keepInitialState = true;
                    m_images[i] = m_device->createHandleForNativeTexture(nvrhi::ObjectTypes::D3D12_Resource,
                        nvrhi::Object(m_buffers[i].p), desc);
                }
                return true;
            }

            void releaseImages()
            {
                for (UINT i = 0; i < kImages; ++i)
                {
                    m_images[i] = nullptr;
                    m_buffers[i].reset();
                }
            }

            Com<IDXGIFactory2> m_factory;
            Com<ID3D12Device> m_d3d;
            Com<ID3D12CommandQueue> m_queue;
            Com<ID3D12InfoQueue> m_infoQueue;
            Com<ID3D12Fence> m_fence;
            UINT64 m_fenceValue = 0;
            int m_debugLogged = 0;
            Com<IDXGISwapChain3> m_swap;
            Com<ID3D12Resource> m_buffers[kImages];
            nvrhi::TextureHandle m_images[kImages];
            nvrhi::DeviceHandle m_device;
            std::string m_adapterName;
            HANDLE m_waitable = nullptr;
            UINT m_swapFlags = 0;
            bool m_tearing = false;
            bool m_vsync = true;
            bool m_presentFailed = false;
        };
    }

    std::unique_ptr<Backend> makeD3D12()
    {
        return std::make_unique<D3D12Backend>();
    }
}
