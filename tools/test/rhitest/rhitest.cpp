// Test bench for src/bild/rhi outside the game (-DSACREDBILD_RHITEST=ON): a window, the device and swap chain of the API
// asked for, clears in changing colors, a resize half way, frame times. Writes rhitest.log next to the exe and exits with 0
// if every step worked.
//
//   rhitest [d3d12|vulkan] [--validation] [--novsync] [--latency N] [--seconds S] [--width W] [--height H]

#include "rhi/rhi.h"

#include <nvrhi/utils.h>

#include "log.h"
#include "rhi/shaders/test_shaders.inc"

#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    bool g_quit = false;
    struct Triangle;
    Triangle* g_triangle = nullptr;

    struct Params
    {
        float tint[4];
        float offset[2];
        float pad[2];
    };

    // The textured triangle of test.hlsl: a 4x4 checker texture, a point sampler, a volatile constant buffer.
    struct Triangle
    {
        nvrhi::ShaderHandle vs, ps;
        nvrhi::TextureHandle texture;
        nvrhi::SamplerHandle sampler;
        nvrhi::BufferHandle params;
        nvrhi::BindingLayoutHandle layout;
        nvrhi::BindingSetHandle set;
        nvrhi::GraphicsPipelineHandle pipeline;
        nvrhi::IFramebuffer* pipelineFor = nullptr;   // pipelines are made for a framebuffer's formats

        bool init(nvrhi::IDevice* dev, nvrhi::ICommandList* cmd)
        {
            vs = Rhi::createShader(nvrhi::ShaderType::Vertex, "vsMain", kTestVSDxil, kTestVSSpirv);
            ps = Rhi::createShader(nvrhi::ShaderType::Pixel, "psMain", kTestPSDxil, kTestPSSpirv);
            texture = dev->createTexture(nvrhi::TextureDesc().setWidth(4).setHeight(4).setFormat(nvrhi::Format::RGBA8_UNORM)
                .setDebugName("checker").setInitialState(nvrhi::ResourceStates::ShaderResource).setKeepInitialState(true));
            sampler = dev->createSampler(nvrhi::SamplerDesc().setAllFilters(false).setAllAddressModes(nvrhi::SamplerAddressMode::Wrap));
            params = dev->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(Params), "params", 16));
            if (!vs || !ps || !texture || !sampler || !params)
            {
                return false;
            }
            uint32_t texels[16];
            for (int i = 0; i < 16; ++i)
            {
                texels[i] = ((i & 1) ^ ((i >> 2) & 1)) ? 0xFFFFFFFFu : 0xFF404040u;
            }
            cmd->open();
            cmd->writeTexture(texture, 0, 0, texels, 16);
            cmd->close();
            dev->executeCommandList(cmd);

            nvrhi::BindingLayoutDesc layoutDesc;
            layoutDesc.visibility = nvrhi::ShaderType::All;
            layoutDesc.bindings = {
                nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
                nvrhi::BindingLayoutItem::Texture_SRV(0),
                nvrhi::BindingLayoutItem::Sampler(0),
            };
            layout = dev->createBindingLayout(layoutDesc);
            nvrhi::BindingSetDesc setDesc;
            setDesc.bindings = {
                nvrhi::BindingSetItem::ConstantBuffer(0, params),
                nvrhi::BindingSetItem::Texture_SRV(0, texture),
                nvrhi::BindingSetItem::Sampler(0, sampler),
            };
            set = dev->createBindingSet(setDesc, layout);
            return layout && set;
        }

        void draw(nvrhi::IDevice* dev, nvrhi::ICommandList* cmd, float t)
        {
            nvrhi::IFramebuffer* fb = Rhi::backBufferFramebuffer();
            if (!pipeline || pipelineFor != fb)
            {
                nvrhi::GraphicsPipelineDesc desc;
                desc.VS = vs;
                desc.PS = ps;
                desc.primType = nvrhi::PrimitiveType::TriangleList;
                desc.bindingLayouts = {layout};
                desc.renderState.depthStencilState.depthTestEnable = false;
                desc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
                pipeline = dev->createGraphicsPipeline(desc, fb->getFramebufferInfo());
                pipelineFor = fb;
            }
            const Params p = {{1.0f, 0.8f + 0.2f * std::sin(t), 0.6f, 1.0f}, {0.3f * std::sin(t * 1.3f), 0.2f * std::cos(t)}, {}};
            cmd->writeBuffer(params, &p, sizeof(p));
            nvrhi::GraphicsState state;
            state.pipeline = pipeline;
            state.framebuffer = fb;
            state.bindings = {set};
            state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(Rhi::width()), static_cast<float>(Rhi::height())));
            cmd->setGraphicsState(state);
            cmd->draw(nvrhi::DrawArguments().setVertexCount(3));
        }
    };

    LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_CLOSE || msg == WM_DESTROY)
        {
            g_quit = true;
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    HWND makeWindow(int width, int height)
    {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = windowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"SacredBildRhiTest";
        RegisterClassW(&wc);
        RECT r = {0, 0, width, height};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        HWND hwnd = CreateWindowW(wc.lpszClassName, L"SacredBild RHI test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
            CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
        return hwnd;
    }

    void pump()
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    // Copies the back buffer of the frame being recorded into a staging texture (before present), then writes it as a
    // 24-bit BMP once the GPU is done.
    bool screenshot(nvrhi::IDevice* dev, nvrhi::ICommandList* cmd, const char* path)
    {
        const uint32_t w = Rhi::width(), h = Rhi::height();
        nvrhi::StagingTextureHandle staging = dev->createStagingTexture(nvrhi::TextureDesc().setWidth(w).setHeight(h)
            .setFormat(Rhi::backBufferFormat()).setDebugName("screenshot"), nvrhi::CpuAccessMode::Read);
        if (!staging || !Rhi::beginFrame())
        {
            return false;
        }
        cmd->open();
        cmd->clearTextureFloat(Rhi::backBuffer(), nvrhi::AllSubresources, nvrhi::Color(0.1f, 0.1f, 0.2f, 1.0f));
        g_triangle->draw(dev, cmd, 1.0f);
        cmd->copyTexture(staging, nvrhi::TextureSlice(), Rhi::backBuffer(), nvrhi::TextureSlice());
        cmd->close();
        dev->executeCommandList(cmd);
        Rhi::present();
        dev->waitForIdle();
        size_t pitch = 0;
        const auto* src = static_cast<const uint8_t*>(dev->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &pitch));
        if (!src)
        {
            return false;
        }
        const bool bgra = Rhi::backBufferFormat() == nvrhi::Format::BGRA8_UNORM;
        const uint32_t row = (w * 3 + 3) & ~3u;
        std::vector<uint8_t> pixels(static_cast<size_t>(row) * h);
        for (uint32_t y = 0; y < h; ++y)
        {
            const uint8_t* in = src + pitch * (h - 1 - y);
            uint8_t* out = pixels.data() + static_cast<size_t>(row) * y;
            for (uint32_t x = 0; x < w; ++x)
            {
                out[x * 3 + 0] = in[x * 4 + (bgra ? 0 : 2)];
                out[x * 3 + 1] = in[x * 4 + 1];
                out[x * 3 + 2] = in[x * 4 + (bgra ? 2 : 0)];
            }
        }
        dev->unmapStagingTexture(staging);
        BITMAPFILEHEADER fh = {};
        BITMAPINFOHEADER ih = {};
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof(fh) + sizeof(ih);
        fh.bfSize = fh.bfOffBits + static_cast<DWORD>(pixels.size());
        ih.biSize = sizeof(ih);
        ih.biWidth = static_cast<LONG>(w);
        ih.biHeight = static_cast<LONG>(h);
        ih.biPlanes = 1;
        ih.biBitCount = 24;
        FILE* f = std::fopen(path, "wb");
        if (!f)
        {
            return false;
        }
        std::fwrite(&fh, sizeof(fh), 1, f);
        std::fwrite(&ih, sizeof(ih), 1, f);
        std::fwrite(pixels.data(), 1, pixels.size(), f);
        std::fclose(f);
        return true;
    }

    // Runs frames for `seconds`; returns frames presented, or -1 if a frame could not begin.
    int runFrames(nvrhi::ICommandList* cmd, Triangle& triangle, double seconds, double& worstMs)
    {
        using Clock = std::chrono::steady_clock;
        const auto start = Clock::now();
        auto last = start;
        int frames = 0;
        worstMs = 0;
        while (!g_quit && std::chrono::duration<double>(Clock::now() - start).count() < seconds)
        {
            pump();
            if (!Rhi::beginFrame())
            {
                return -1;
            }
            const float t = static_cast<float>(Rhi::frameIndex()) * 0.02f;
            cmd->open();
            cmd->clearTextureFloat(Rhi::backBuffer(), nvrhi::AllSubresources,
                nvrhi::Color(0.5f + 0.5f * std::sin(t), 0.5f + 0.5f * std::sin(t + 2.1f), 0.5f + 0.5f * std::sin(t + 4.2f), 1.0f));
            triangle.draw(Rhi::device(), cmd, t);
            cmd->close();
            Rhi::device()->executeCommandList(cmd);
            Rhi::present();
            ++frames;
            const auto now = Clock::now();
            worstMs = std::max(worstMs, std::chrono::duration<double, std::milli>(now - last).count());
            last = now;
        }
        return frames;
    }
}

int main(int argc, char** argv)
{
    Rhi::Settings settings;
    double seconds = 3.0;
    const char* shot = nullptr;
    int width = 1600, height = 900;
    for (int i = 1; i < argc; ++i)
    {
        const char* a = argv[i];
        if (!std::strcmp(a, "vulkan"))
            settings.api = Rhi::Api::Vulkan;
        else if (!std::strcmp(a, "d3d12"))
            settings.api = Rhi::Api::D3D12;
        else if (!std::strcmp(a, "--validation"))
            settings.validation = true;
        else if (!std::strcmp(a, "--novsync"))
            settings.vsync = false;
        else if (!std::strcmp(a, "--latency") && i + 1 < argc)
            settings.maxFrameLatency = std::atoi(argv[++i]);
        else if (!std::strcmp(a, "--seconds") && i + 1 < argc)
            seconds = std::atof(argv[++i]);
        else if (!std::strcmp(a, "--width") && i + 1 < argc)
            width = std::atoi(argv[++i]);
        else if (!std::strcmp(a, "--height") && i + 1 < argc)
            height = std::atoi(argv[++i]);
        else if (!std::strcmp(a, "--shot") && i + 1 < argc)
            shot = argv[++i];
    }

    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring log(path);
    log = log.substr(0, log.find_last_of(L"\\/") + 1) + L"rhitest.log";
    Log::init(log.c_str());

    HWND hwnd = makeWindow(width, height);
    if (!hwnd || !Rhi::init(hwnd, static_cast<uint32_t>(width), static_cast<uint32_t>(height), settings))
    {
        std::printf("FAIL: %s device\n", Rhi::apiName(settings.api));
        return 1;
    }
    std::printf("%s on %s\n", Rhi::apiName(settings.api), Rhi::adapterName().c_str());

    int status = 0;
    {
        nvrhi::CommandListHandle cmd = Rhi::device()->createCommandList();
        Triangle triangle;
        if (!triangle.init(Rhi::device(), cmd))
        {
            std::printf("FAIL: triangle resources\n");
            status = 1;
        }
        double worst = 0;
        auto report = [&](const char* phase, int frames, double secs) {
            if (frames < 0)
            {
                std::printf("FAIL: %s: a frame could not begin\n", phase);
                LOG("rhitest: {}: FAIL", phase);
                status = 1;
                return;
            }
            std::printf("%s: %d frames, %.1f fps, worst frame %.2f ms (%ux%u)\n", phase, frames, frames / secs, worst, Rhi::width(), Rhi::height());
            LOG("rhitest: {}: {} frames, {:.1f} fps, worst {:.2f} ms, {}x{}", phase, frames, frames / secs, worst, Rhi::width(), Rhi::height());
        };
        report("full", runFrames(cmd, triangle, seconds / 3, worst), seconds / 3);
        RECT r = {0, 0, width / 2, height / 2};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
        pump();
        Rhi::resize(static_cast<uint32_t>(width / 2), static_cast<uint32_t>(height / 2));
        report("half", runFrames(cmd, triangle, seconds / 3, worst), seconds / 3);
        r = {0, 0, width, height};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
        pump();
        Rhi::resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
        report("full again", runFrames(cmd, triangle, seconds / 3, worst), seconds / 3);
        g_triangle = &triangle;
        if (shot && !screenshot(Rhi::device(), cmd, shot))
        {
            std::printf("FAIL: screenshot\n");
            status = 1;
        }
    }
    Rhi::shutdown();
    DestroyWindow(hwnd);
    std::printf(status ? "FAIL\n" : "OK\n");
    return status;
}
