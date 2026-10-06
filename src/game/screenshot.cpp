#include "game/screenshot.h"
#include "game/frame_hooks.h"
#include "game/sacred_addr.h"
#include "config.h"
#include "log.h"
#include "patch.h"

#include <windows.h>
#include <ddraw.h>
#include <oleauto.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
    using namespace Sacred;

    // cdecl (bits, pitch, mode), called by dxDriver7_flip with the locked back buffer.
    using CaptureFn = void(__cdecl*)(void* bits, int pitch, int mode);
    CaptureFn g_origCapture = nullptr;

    // Not exported by every SDK library this links against.
    constexpr GUID kClsidWicImagingFactory = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
    constexpr GUID kWicPixelFormat24bppBGR = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0c}};
    constexpr GUID kContainerFormatPng = {0x1b7cfaf4, 0x713f, 0x473c, {0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf}};
    constexpr GUID kContainerFormatJpeg = {0x19e4a5aa, 0x5662, 0x4fc5, {0xa0, 0xc0, 0x17, 0x58, 0x02, 0x8e, 0x10, 0x57}};

    template <class T>
    T& member(void* obj, uintptr_t offset)
    {
        return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(obj) + offset);
    }

    struct Image
    {
        UINT width = 0, height = 0;
        std::vector<uint8_t> bgr;   // 24 bits per pixel, rows of width * 3 bytes
    };

    // The number of the next free Capture\shotNNNN: none of the game's .tga/.jpg and SacredBild's .png/.jpg may
    // exist. -1 if all are taken.
    int reserveNumber()
    {
        static int next = 0;
        CreateDirectoryW(L"Capture", nullptr);
        for (int i = next; i < 10000; ++i)
        {
            bool taken = false;
            for (const wchar_t* used : {L"tga", L"jpg", L"png"})
            {
                taken = taken || GetFileAttributesW(Fmt::format(L"Capture\\shot{:04}.{}", i, used).c_str()) != INVALID_FILE_ATTRIBUTES;
            }
            if (!taken)
            {
                next = i + 1;
                return i;
            }
        }
        return -1;
    }

    HRESULT encode(const Image& image, const std::wstring& path, bool jpeg)
    {
        ComPtr<IWICImagingFactory> wic;
        HRESULT hr = CoCreateInstance(kClsidWicImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
        ComPtr<IWICStream> stream;
        if (SUCCEEDED(hr))
        {
            hr = wic->CreateStream(&stream);
        }
        if (SUCCEEDED(hr))
        {
            hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
        }
        ComPtr<IWICBitmapEncoder> encoder;
        if (SUCCEEDED(hr))
        {
            hr = wic->CreateEncoder(jpeg ? kContainerFormatJpeg : kContainerFormatPng, nullptr, &encoder);
        }
        if (SUCCEEDED(hr))
        {
            hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        }
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> options;
        if (SUCCEEDED(hr))
        {
            hr = encoder->CreateNewFrame(&frame, &options);
        }
        if (SUCCEEDED(hr) && jpeg)
        {
            PROPBAG2 option = {};
            option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
            VARIANT quality;
            VariantInit(&quality);
            quality.vt = VT_R4;
            quality.fltVal = 0.95f;
            hr = options->Write(1, &option, &quality);
        }
        if (SUCCEEDED(hr))
        {
            hr = frame->Initialize(options.Get());
        }
        if (SUCCEEDED(hr))
        {
            hr = frame->SetSize(image.width, image.height);
        }
        WICPixelFormatGUID format = kWicPixelFormat24bppBGR;
        if (SUCCEEDED(hr))
        {
            hr = frame->SetPixelFormat(&format);
        }
        if (SUCCEEDED(hr) && format != kWicPixelFormat24bppBGR)
        {
            hr = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
        }
        if (SUCCEEDED(hr))
        {
            hr = frame->WritePixels(image.height, image.width * 3, static_cast<UINT>(image.bgr.size()),
                const_cast<BYTE*>(image.bgr.data()));
        }
        if (SUCCEEDED(hr))
        {
            hr = frame->Commit();
        }
        if (SUCCEEDED(hr))
        {
            hr = encoder->Commit();
        }
        return hr;
    }

    void save(Image image, int number, bool jpeg)
    {
        const char* extension = jpeg ? "jpg" : "png";
        const std::string name = Fmt::format("Capture\\shot{:04}.{}", number, extension);
        const std::wstring path(name.begin(), name.end());
        const auto start = std::chrono::steady_clock::now();
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const HRESULT hr = encode(image, path, jpeg);
        if (SUCCEEDED(com))
        {
            CoUninitialize();
        }
        if (FAILED(hr))
        {
            DeleteFileW(path.c_str());
            LOG("Screenshot: writing {} failed ({:08x})", name, static_cast<uint32_t>(hr));
            return;
        }
        LOG("Screenshot: {} ({}x{}, {} ms)", name, image.width, image.height,
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
    }

    // Copies the whole back buffer and saves it on a worker thread. False if its format is not one this handles
    // (the game's own capture runs then).
    bool screenshot(const uint8_t* bits, int pitch)
    {
        void* driver = FrameHooks::dxDriver();
        auto* back = driver ? member<IDirectDrawSurface7*>(driver, DxDriver::back) : nullptr;
        DDSURFACEDESC2 desc = {};
        desc.dwSize = sizeof(desc);
        if (!back || FAILED(back->GetSurfaceDesc(&desc)))
        {
            return false;
        }
        const DDPIXELFORMAT& pf = desc.ddpfPixelFormat;
        const bool bgrx = pf.dwRBitMask == 0xFF0000 && pf.dwGBitMask == 0xFF00 && pf.dwBBitMask == 0xFF;
        const bool rgbx = pf.dwRBitMask == 0xFF && pf.dwGBitMask == 0xFF00 && pf.dwBBitMask == 0xFF0000;
        if (pf.dwRGBBitCount != 32 || !(bgrx || rgbx))
        {
            LOG("Screenshot: back buffer format {} bits, masks {:x} {:x} {:x}: left to the game", pf.dwRGBBitCount,
                pf.dwRBitMask, pf.dwGBitMask, pf.dwBBitMask);
            return false;
        }
        const int number = reserveNumber();
        if (number < 0)
        {
            LOG("Screenshot: Capture\\shot0000 .. shot9999 are all taken");
            return true;
        }
        Image image;
        image.width = desc.dwWidth;
        image.height = desc.dwHeight;
        image.bgr.resize(static_cast<size_t>(image.width) * 3 * image.height);
        const int blue = bgrx ? 0 : 2, red = 2 - blue;
        uint8_t* out = image.bgr.data();
        for (UINT y = 0; y < image.height; ++y)
        {
            const uint8_t* in = bits + static_cast<ptrdiff_t>(y) * pitch;
            for (UINT x = 0; x < image.width; ++x, in += 4, out += 3)
            {
                out[0] = in[blue];
                out[1] = in[1];
                out[2] = in[red];
            }
        }
        std::thread(save, std::move(image), number, g_config.screenshotJpeg).detach();
        return true;
    }

    // Mode -1 is a single screenshot. Other modes belong to the game's frame-sequence capture (frames >= 0 into a
    // mapped file, < -1 writes them out), which assumes 1024x768; nothing in the game starts it with more than one
    // frame, so it is left as it is.
    void __cdecl hookCapture(void* bits, int pitch, int mode)
    {
        if (mode == -1 && bits && screenshot(static_cast<const uint8_t*>(bits), pitch))
        {
            return;
        }
        g_origCapture(bits, pitch, mode);
    }
}

void Screenshot::install()
{
    Patch::hook(g_origCapture, Addr::captureScreenshot, &hookCapture, "captureScreenshot");
}
