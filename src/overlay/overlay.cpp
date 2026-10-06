#include "overlay/overlay.h"
#include "ddraw9/gpu.h"
#include "input/gamepad.h"
#include "input/input_mode.h"
#include "config.h"
#include "log.h"

#include <imgui.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>

#include <atomic>
#include <initializer_list>
#include <mutex>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
    std::atomic<Overlay::DrawFn> g_draw{nullptr};
    std::atomic<DWORD> g_lastPresent{0};            // presents pass by: the overlay can show
    std::atomic<bool> g_gamepadNavigation{true};

    d9::IDirect3DDevice9Ex* g_device = nullptr;     // ImGui's backends are initialized for it
    HWND g_window = nullptr;
    float g_scale = 1.0f;

    struct Message
    {
        UINT message;
        WPARAM wParam;
        LPARAM lParam;
    };
    std::mutex g_queueLock;
    std::vector<Message> g_queue;

    bool isInput(UINT message)
    {
        return (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) || message == WM_KEYDOWN || message == WM_KEYUP ||
            message == WM_SYSKEYDOWN || message == WM_SYSKEYUP || message == WM_CHAR || message == WM_SYSCHAR ||
            message == WM_DEADCHAR || message == WM_UNICHAR;
    }

    // Dark, with the gold of the game's own windows.
    void applyStyle()
    {
        ImGuiStyle& style = ImGui::GetStyle();
        ImGui::StyleColorsDark(&style);
        style.WindowRounding = 4.0f;
        style.FrameRounding = 3.0f;
        style.GrabRounding = 3.0f;
        style.TabRounding = 3.0f;
        style.WindowBorderSize = 1.0f;
        style.FrameBorderSize = 1.0f;
        ImVec4* c = style.Colors;
        const ImVec4 gold(0.83f, 0.66f, 0.29f, 1.0f);
        const ImVec4 goldDim(0.55f, 0.43f, 0.18f, 1.0f);
        c[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.06f, 0.05f, 0.96f);
        c[ImGuiCol_Border] = goldDim;
        c[ImGuiCol_Text] = ImVec4(0.93f, 0.89f, 0.80f, 1.0f);
        c[ImGuiCol_TitleBg] = ImVec4(0.20f, 0.15f, 0.07f, 1.0f);
        c[ImGuiCol_TitleBgActive] = ImVec4(0.30f, 0.22f, 0.09f, 1.0f);
        c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.13f, 0.09f, 1.0f);
        c[ImGuiCol_FrameBgHovered] = ImVec4(0.32f, 0.25f, 0.12f, 1.0f);
        c[ImGuiCol_FrameBgActive] = ImVec4(0.40f, 0.31f, 0.14f, 1.0f);
        c[ImGuiCol_Button] = ImVec4(0.24f, 0.18f, 0.09f, 1.0f);
        c[ImGuiCol_ButtonHovered] = ImVec4(0.45f, 0.34f, 0.14f, 1.0f);
        c[ImGuiCol_ButtonActive] = goldDim;
        c[ImGuiCol_Header] = ImVec4(0.30f, 0.23f, 0.10f, 1.0f);
        c[ImGuiCol_HeaderHovered] = ImVec4(0.45f, 0.34f, 0.14f, 1.0f);
        c[ImGuiCol_HeaderActive] = goldDim;
        c[ImGuiCol_Tab] = ImVec4(0.20f, 0.15f, 0.07f, 1.0f);
        c[ImGuiCol_TabHovered] = ImVec4(0.45f, 0.34f, 0.14f, 1.0f);
        c[ImGuiCol_TabSelected] = ImVec4(0.38f, 0.29f, 0.12f, 1.0f);
        c[ImGuiCol_CheckMark] = gold;
        c[ImGuiCol_SliderGrab] = goldDim;
        c[ImGuiCol_SliderGrabActive] = gold;
        c[ImGuiCol_Separator] = goldDim;
        c[ImGuiCol_NavCursor] = gold;
    }

    // Fonts with the game's languages' letters (Latin, Cyrillic): Segoe UI, Tahoma (Wine), else ImGui's own.
    void loadFont(float size)
    {
        ImGuiIO& io = ImGui::GetIO();
        wchar_t windows[MAX_PATH] = {};
        GetWindowsDirectoryW(windows, MAX_PATH);
        for (const wchar_t* name : {L"\\Fonts\\segoeui.ttf", L"\\Fonts\\tahoma.ttf"})
        {
            const std::wstring path = std::wstring(windows) + name;
            if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
            {
                continue;
            }
            char utf8[MAX_PATH * 3] = {};
            WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, utf8, static_cast<int>(sizeof(utf8)), nullptr, nullptr);
            if (io.Fonts->AddFontFromFileTTF(utf8, size))
            {
                LOG("Overlay: font {}", utf8);
                return;
            }
        }
        io.Fonts->AddFontDefault();
        LOG("Overlay: no system font, ImGui's own");
    }

    bool init(d9::IDirect3DDevice9Ex* device, d9::IDirect3DSurface9* backBuffer, HWND window)
    {
        if (g_device == device && g_window == window)
        {
            return true;
        }
        if (g_device)
        {
            ImGui_ImplDX9_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            g_device = nullptr;
        }
        d9::D3DSURFACE_DESC desc = {};
        backBuffer->GetDesc(&desc);
        g_scale = desc.Height > 0 ? desc.Height / 768.0f : 1.0f;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;       // nothing written to the game folder
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad |
            ImGuiConfigFlags_NoMouseCursorChange;
        if (!ImGui_ImplWin32_Init(window) || !ImGui_ImplDX9_Init(reinterpret_cast<::IDirect3DDevice9*>(device)))
        {
            LOG("Overlay: ImGui backend initialization failed");
            ImGui::DestroyContext();
            return false;
        }
        applyStyle();
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(g_scale);
        style.FontSizeBase = 17.0f * g_scale;
        // Tooltips for what the controller or keyboard selected, as for the mouse.
        style.HoverFlagsForTooltipNav = ImGuiHoveredFlags_NoSharedDelay | ImGuiHoveredFlags_DelayShort;
        loadFont(style.FontSizeBase);
        g_device = device;
        g_window = window;
        LOG("Overlay: ImGui {} on Direct3D 9, {}x{} (scale {:.2f})", ImGui::GetVersion(), desc.Width, desc.Height, g_scale);
        return true;
    }

    void feedGamepad(ImGuiIO& io)
    {
        const Gamepad::State& s = Gamepad::state();
        if (!s.connected || !g_gamepadNavigation.load(std::memory_order_relaxed))
        {
            return;
        }
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        const auto button = [&](ImGuiKey key, uint32_t b) { io.AddKeyEvent(key, (s.buttons & b) != 0); };
        button(ImGuiKey_GamepadFaceDown, Gamepad::A);
        button(ImGuiKey_GamepadFaceRight, Gamepad::B);
        button(ImGuiKey_GamepadFaceLeft, Gamepad::X);
        button(ImGuiKey_GamepadFaceUp, Gamepad::Y);
        button(ImGuiKey_GamepadStart, Gamepad::Start);
        button(ImGuiKey_GamepadBack, Gamepad::Back);
        button(ImGuiKey_GamepadL1, Gamepad::LB);
        button(ImGuiKey_GamepadR1, Gamepad::RB);
        button(ImGuiKey_GamepadL3, Gamepad::L3);
        button(ImGuiKey_GamepadR3, Gamepad::R3);
        button(ImGuiKey_GamepadDpadUp, Gamepad::Up);
        button(ImGuiKey_GamepadDpadDown, Gamepad::Down);
        button(ImGuiKey_GamepadDpadLeft, Gamepad::Left);
        button(ImGuiKey_GamepadDpadRight, Gamepad::Right);
        const auto analog = [&](ImGuiKey key, float v) { io.AddKeyAnalogEvent(key, v > 0.1f, v > 0.0f ? v : 0.0f); };
        analog(ImGuiKey_GamepadL2, s.lt);
        analog(ImGuiKey_GamepadR2, s.rt);
        analog(ImGuiKey_GamepadLStickLeft, -s.lx);
        analog(ImGuiKey_GamepadLStickRight, s.lx);
        analog(ImGuiKey_GamepadLStickUp, s.ly);
        analog(ImGuiKey_GamepadLStickDown, -s.ly);
        analog(ImGuiKey_GamepadRStickLeft, -s.rx);
        analog(ImGuiKey_GamepadRStickRight, s.rx);
        analog(ImGuiKey_GamepadRStickUp, s.ry);
        analog(ImGuiKey_GamepadRStickDown, -s.ry);
    }

    void render(d9::IDirect3DDevice9Ex* device, d9::IDirect3DSurface9* backBuffer, HWND window)
    {
        g_lastPresent = GetTickCount();
        const Overlay::DrawFn draw = g_draw.load(std::memory_order_acquire);
        if (!draw)
        {
            return;
        }
        if (!init(device, backBuffer, window))
        {
            g_draw = nullptr;
            return;
        }
        {
            std::vector<Message> queue;
            {
                std::scoped_lock lock(g_queueLock);
                queue.swap(g_queue);
            }
            for (const Message& m : queue)
            {
                ImGui_ImplWin32_WndProcHandler(window, m.message, m.wParam, m.lParam);
            }
        }
        ImGuiIO& io = ImGui::GetIO();
        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        if (g_gamepadNavigation.load(std::memory_order_relaxed))
        {
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        }
        else
        {
            io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
        }
        feedGamepad(io);
        // With the controller: no mouse cursor, and the selected item always marked (and drawn as hovered).
        const bool controller = InputMode::controller();
        io.MouseDrawCursor = !controller;
        io.ConfigNavCursorVisibleAlways = controller;
        ImGui::NewFrame();
        const bool keep = draw();
        ImGui::Render();

        d9::IDirect3DSurface9* target = nullptr;
        d9::IDirect3DSurface9* depth = nullptr;
        d9::D3DVIEWPORT9 viewport = {};
        device->GetRenderTarget(0, &target);
        device->GetDepthStencilSurface(&depth);
        device->GetViewport(&viewport);
        device->SetRenderTarget(0, backBuffer);
        device->SetDepthStencilSurface(nullptr);
        if (SUCCEEDED(device->BeginScene()))
        {
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            device->EndScene();
        }
        device->SetRenderTarget(0, target);
        device->SetDepthStencilSurface(depth);
        device->SetViewport(&viewport);
        if (target)
        {
            target->Release();
        }
        if (depth)
        {
            depth->Release();
        }

        if (!keep)
        {
            Overlay::DrawFn expected = draw;
            g_draw.compare_exchange_strong(expected, nullptr);
        }
    }
}

void Overlay::install()
{
    if (!g_config.ddrawD3D9)
    {
        LOG("Overlay: off ([DDraw] Backend=chain)");
        return;
    }
    DDraw9::Gpu::setOverlay(&render);
}

bool Overlay::available()
{
    return g_config.ddrawD3D9 && g_lastPresent.load() && GetTickCount() - g_lastPresent.load() < 1000;
}

void Overlay::open(DrawFn draw)
{
    {
        std::scoped_lock lock(g_queueLock);
        g_queue.clear();
    }
    g_gamepadNavigation = true;
    g_lastPresent = GetTickCount();
    g_draw.store(draw, std::memory_order_release);
}

bool Overlay::isOpen()
{
    if (!g_draw.load(std::memory_order_acquire))
    {
        return false;
    }
    // No frames for a while (the device is gone): the screen can't be seen, so it doesn't hold the input either.
    if (GetTickCount() - g_lastPresent.load() > 2000)
    {
        g_draw = nullptr;
        return false;
    }
    return true;
}

bool Overlay::message(HWND, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (!isOpen() || !isInput(message))
    {
        return false;
    }
    std::scoped_lock lock(g_queueLock);
    if (g_queue.size() < 4096)
    {
        g_queue.push_back({message, wParam, lParam});
    }
    return true;
}

void Overlay::setGamepadNavigation(bool enabled)
{
    g_gamepadNavigation = enabled;
}

float Overlay::scale()
{
    return g_scale;
}
