#include "settings_window.h"
#include "config.h"
#include "log.h"
#include "ui/form.h"

#include <algorithm>
#include <format>
#include <string>
#include <vector>

namespace
{
    using Ui::Choice;
    using Ui::Form;

    std::string ascii(const std::wstring& s)
    {
        std::string out;
        for (wchar_t c : s)
        {
            out += c < 0x80 ? static_cast<char>(c) : '?';
        }
        return out;
    }

    std::wstring widen(const std::string& s)
    {
        return {s.begin(), s.end()};
    }

    // The ini's own MediaFoundation value: g_config has it forced on with Backend=d3d9.
    bool iniMediaFoundation(const std::wstring& ini)
    {
        wchar_t buf[16] = {};
        GetPrivateProfileStringW(L"DDraw", L"MediaFoundation", L"1", buf, static_cast<DWORD>(std::size(buf)), ini.c_str());
        for (const wchar_t* no : {L"0", L"false", L"no", L"off"})
        {
            if (_wcsicmp(buf, no) == 0)
            {
                return false;
            }
        }
        return true;
    }

    int displayRefresh()
    {
        DEVMODEW mode = {};
        mode.dmSize = sizeof(mode);
        return EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1
            ? static_cast<int>(mode.dmDisplayFrequency) : 0;
    }

    // "W,H" for [Display] Width,Height: the primary display's modes the UI fits into, largest first; "0,0" = desktop.
    std::vector<Choice> resolutions()
    {
        std::vector<std::pair<int, int>> sizes{{1024, 768}};
        DEVMODEW mode = {};
        mode.dmSize = sizeof(mode);
        for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &mode); ++i)
        {
            if (mode.dmPelsWidth >= 1024 && mode.dmPelsHeight >= 768)
            {
                sizes.emplace_back(mode.dmPelsWidth, mode.dmPelsHeight);
            }
        }
        std::ranges::sort(sizes, std::greater{});
        sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());
        std::vector<Choice> choices{
            {std::format(L"Desktop ({} x {})", GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)), L"0,0"}};
        for (const auto [w, h] : sizes)
        {
            choices.push_back({std::format(L"{} x {}{}", w, h, w == 1024 && h == 768 ? L" (original)" : L""), std::format(L"{},{}", w, h)});
        }
        return choices;
    }

    std::wstring resolution()
    {
        return g_config.width > 0 && g_config.height > 0 ? std::format(L"{},{}", g_config.width, g_config.height) : L"0,0";
    }

    // Percent in the list, the ini's factor as the value.
    std::vector<Choice> uiScales()
    {
        std::vector<Choice> choices{{L"Fit to the screen height", L"0"}};
        for (int percent : {100, 125, 150, 175, 200, 250, 300})
        {
            choices.push_back({percent == 100 ? std::wstring(L"100 % (native pixels)") : std::format(L"{} %", percent),
                std::format(L"{}", percent / 100.0)});
        }
        return choices;
    }

    std::wstring fps(int n)
    {
        return n ? std::format(L"{} fps", n) : L"Off";
    }

    struct HudWindow
    {
        const wchar_t* key;
        const wchar_t* label;
        Config::UiPosition Config::*position;
        const wchar_t* tip = L"";
    };

    constexpr HudWindow kHudWindows[] = {
        {L"Taskbar", L"Taskbar", &Config::uiTaskbar},
        {L"Chat", L"Chat", &Config::uiChat},
        {L"Inventory", L"Inventory", &Config::uiInventory},
        {L"Stats", L"Stats", &Config::uiStats},
        {L"Equipment", L"Equipment", &Config::uiEquipment},
        {L"Minimap", L"Minimap", &Config::uiMinimap},
        {L"Portraits", L"Party portraits", &Config::uiPortraits, L"Party portraits (multiplayer)."},
        {L"Shops", L"Shops and chests", &Config::uiShops, L"Blacksmith, merchant, combat art master, chest, cube and trade windows."},
    };

    bool d3d9(const Form& f)
    {
        return f.value(L"DDraw", L"Backend") == L"d3d9";
    }

    bool batching(const Form& f)
    {
        return f.on(L"Render", L"Batch");
    }

    // The window, top to bottom. Each control names the ini key it edits and its current value.
    void declare(Form& ui, const std::wstring& ini)
    {
        ui.page(L"General");
        ui.group(L"Display");
        ui.combo(L"Resolution:", {L"Display", L"Width,Height"}, resolutions(), resolution())
            .tip(L"The resolution the game renders at. Desktop = the screen's resolution; a smaller one plays in a "
                 L"window. 1024 x 768 runs the original game without patches.");
        ui.combo(L"Window frame:", {L"Display", L"Borderless"},
              {{L"Automatic", L"auto"}, {L"Borderless", L"1"}, {L"Window frame", L"0"}},
              g_config.frame == Config::Frame::Auto ? L"auto" : g_config.frame == Config::Frame::Never ? L"1" : L"0")
            .tip(L"Automatic: a frame with a title bar (to move the window) when the window is smaller than the screen, "
                 L"none when it fills the screen.\nBorderless: never a frame.\nWindow frame: always a frame.\n"
                 L"The area inside the frame has the chosen resolution either way.");
        const int refresh = displayRefresh();
        std::vector<int> limits{0, 30, 60, 75, 90, 120, 144, 165, 240};
        if (refresh)
        {
            limits.push_back(refresh);
        }
        ui.combo(L"Frame limit:", {L"Display", L"FpsLimit"}, limits, std::max(g_config.fpsLimit, 0),
              [&](int n) {
                  return n == 60     ? std::format(L"60 fps (original{})", n == refresh ? L", display refresh" : L"")
                      : n == refresh ? std::format(L"{} fps (display refresh)", n)
                                     : fps(n);
              })
            .tip(L"The game's own frame limit in game (it uses 60), e.g. 144 for a 144 Hz display. The menus are unchanged.");
        ui.check(L"Wait for the display's refresh (VSync)", {L"Display", L"VSync"}, g_config.vsync)
            .enabledIf(d3d9)
            .tip(L"Show each frame on the display's refresh (no tearing). Off: show frames right away. "
                 L"Needs the SacredBild (Direct3D 9) renderer.");
        ui.check(L"Keep the mouse inside the game window", {L"Display", L"ClipCursor"}, g_config.clipCursor)
            .tip(L"Keep the mouse inside the game window while it is in the foreground (multiple monitors, a smaller "
                 L"window on a wide screen). Hold Alt to move the mouse out of the window.");

        ui.column();
        ui.group(L"Interface");
        ui.combo(L"UI size:", {L"UI", L"Scale"}, uiScales(), g_config.uiScale > 0.0f ? std::format(L"{}", g_config.uiScale) : L"0")
            .tip(L"The game's 1024 x 768 interface is drawn centered and scaled. Fit = as large as fits the screen "
                 L"height; a percentage = a fixed size (100 % = native pixels), capped at what fits.");
        ui.check(L"Use the UI size in the menus too", {L"UI", L"ScaleMode"}, g_config.uiScaleMenus)
            .values(L"Full", L"InGame")
            .enabledIf([](const Form& f) { return f.value(L"UI", L"Scale") != L"0"; })
            .tip(L"Use the UI size for the menus, the full-screen windows in game (options, save/load, map) and the "
                 L"loading screen too. Off: those always fill the screen height.");
        ui.check(L"Place the HUD windows at the screen edges", {L"UI", L"Anchor"}, g_config.uiAnchor)
            .tip(L"In game, place the taskbar, minimap, inventory and the other HUD windows at the screen edges "
                 L"(HUD layout tab). Off: the whole interface stays in the centered 1024 x 768 area.");
        ui.check(L"Smooth UI scaling (bilinear filtering)", {L"UI", L"LinearFilter"}, g_config.uiLinearFilter)
            .tip(L"Bilinear filtering for the scaled interface. Off: the game's sharp point sampling.");
        ui.group(L"Renderer");
        ui.combo(L"DirectDraw:", {L"DDraw", L"Backend"},
              {{L"SacredBild (Direct3D 9)", L"d3d9"}, {L"Chain-loaded ddraw", L"chain"}}, g_config.ddrawD3D9 ? L"d3d9" : L"chain")
            .tip(L"SacredBild (Direct3D 9): SacredBild runs the game's DirectDraw / Direct3D 7 on Direct3D 9 itself.\n"
                 L"Chain-loaded ddraw: the ddraw.dll set on the Advanced tab (DDrawCompat or another wrapper), else "
                 L"Windows' own.");
        ui.check(L"Animate characters on the GPU", {L"Render", L"GpuSkinning"}, g_config.gpuSkinning)
            .enabledIf(d3d9)
            .tip(L"Animate (skin) characters and their shadows in a vertex shader instead of on the CPU. "
                 L"Needs the SacredBild (Direct3D 9) renderer.");

        ui.page(L"Advanced");
        ui.group(L"Frame timing");
        ui.combo(L"In the background:", {L"Display", L"FpsLimitInactive"}, {0, 10, 15, 20, 30, 60},
              std::max(g_config.fpsLimitInactive, 0), fps)
            .tip(L"Frame limit while the game is in the background (another window has the focus), in game and in the menus.");
        ui.combo(L"Frames queued:", {L"Display", L"MaxFrameLatency"}, {1, 2, 3}, g_config.maxFrameLatency,
              [](int n) { return n <= 0 ? std::wstring(L"Driver default") : n == 1 ? std::wstring(L"1 (lowest latency)") : std::to_wstring(n); })
            .enabledIf(d3d9)
            .tip(L"Frames the CPU may prepare ahead of the GPU. 1 = lowest input latency. "
                 L"Needs the SacredBild (Direct3D 9) renderer.");
        ui.group(L"Rendering");
        ui.combo(L"Texture memory:", {L"Render", L"TextureBudgetMB"}, {0, 256, 512, 1024, 2048}, g_config.textureBudgetMB,
              [](int mb) { return mb ? std::format(L"{} MB", mb) : std::wstring(L"Automatic"); })
            .tip(L"Texture memory the game may keep loaded. A zoomed-out view at a high resolution shows far more "
                 L"different ground textures than the original 1024 x 768. Automatic = the game's own value, at least 256 MB.");
        ui.combo(L"Off-screen poses:", {L"Render", L"OffscreenPoses"}, {1, 2, 4, 8}, g_config.offscreenPoses,
              [](int n) { return n <= 1 ? std::wstring(L"Every frame") : n == 2 ? std::wstring(L"Every 2nd frame") : std::format(L"Every {}th frame", n); })
            .enabledIf([](const Form& f) { return d3d9(f) && f.on(L"Render", L"GpuSkinning"); })
            .tip(L"Characters not drawn in the last frames get their skeleton posed this often instead of every frame "
                 L"(most animated characters are off screen). Every frame = as the game does. Needs GPU animation.");
        ui.check(L"Merge draw calls", {L"Render", L"Batch"}, g_config.batch)
            .tip(L"Merge the world view's thousands of small draw calls into few large ones. Off = draw as the game does.");
        ui.indent();
        ui.beginEnabledIf(batching);
        ui.check(L"Let the GPU clip merged draws", {L"Render", L"BatchNoClip"}, g_config.batchNoClip)
            .tip(L"Let the GPU clip merged draws instead of Direct3D 7 on the CPU.");
        ui.check(L"Vertex buffers for merged draws", {L"Render", L"BatchVertexBuffer"}, g_config.batchVertexBuffer)
            .tip(L"Hand merged draws to Direct3D in vertex buffers instead of plain memory.");
        ui.check(L"Merge 3D models too", {L"Render", L"BatchModels"}, g_config.batchModels)
            .tip(L"Send 3D models (characters and their shadows) through the same vertex buffers, merged where they can be.");
        ui.check(L"Merge the ground's quads directly", {L"Render", L"BatchGround"}, g_config.batchGround)
            .tip(L"Hand the ground's quads to the merging directly instead of through three device calls per quad.");
        ui.check(L"Texture atlas:", {L"Render", L"Atlas"}, g_config.atlas)
            .tip(L"Copy small textures into large shared pages so draws with different textures can be merged too.");
        ui.indent();
        ui.beginEnabledIf([](const Form& f) { return f.on(L"Render", L"Atlas"); });
        ui.combo(nullptr, {L"Render", L"AtlasPageSize"}, {2048, 4096, 8192, 16384}, g_config.atlasPageSize,
              [](int px) { return std::format(L"{} px", px); })
            .tip(L"Atlas page size in texels (clamped to what the GPU supports).");
        ui.sameLine();
        ui.combo(nullptr, {L"Render", L"AtlasPages"}, {1, 2, 3, 4}, g_config.atlasPages,
              [](int n) { return std::format(L"{} page{}", n, n == 1 ? L"" : L"s"); })
            .tip(L"Atlas pages per texture format; the least recently used one is reused when full.");
        ui.sameLine();
        ui.combo(nullptr, {L"Render", L"AtlasMaxTextureSize"}, {128, 256, 512, 1024}, g_config.atlasMaxTextureSize,
              [](int px) { return std::format(L"up to {} px", px); })
            .tip(L"The largest texture copied into the atlas; larger ones are used directly.");
        ui.endEnabledIf();
        ui.endEnabledIf();
        ui.unindent();
        ui.unindent();
        ui.check(L"Animate on a second thread", {L"Render", L"AsyncAnimation"}, g_config.asyncAnimation)
            .tip(L"Advance the 3D animations on a second thread while the frame starts drawing (needs a second CPU core).");
        ui.check(L"Hash index for the map records", {L"Render", L"RecordIndex"}, g_config.recordIndex)
            .tip(L"Hash index in front of the game's record caches (looked up for every ground tile and object).");

        ui.column();
        ui.group(L"DirectDraw");
        ui.edit(L"Chain ddraw:", {L"DDraw", L"Chain"}, g_config.ddrawChain)
            .enabledIf([](const Form& f) { return !d3d9(f); })
            .tip(L"Chain-loaded ddraw renderer: the ddraw.dll to load behind SacredBild, relative to the game folder "
                 L"(DDrawCompat or another wrapper). Empty = Windows' own ddraw.dll.");
        ui.edit(L"d3d9.dll:", {L"DDraw", L"D3D9"}, g_config.d3d9Path)
            .enabledIf(d3d9)
            .tip(L"SacredBild (Direct3D 9) renderer: the d3d9.dll to use, e.g. DXVK's (relative to the game folder or "
                 L"absolute). Empty: a d3d9.dll next to the game exe if there is one, else Windows' own.");
        ui.check(L"Movies through Media Foundation", {L"DDraw", L"MediaFoundation"}, iniMediaFoundation(ini))
            .enabledIf([](const Form& f) { return !d3d9(f); })
            .tip(L"Play the intro and cutscene movies through Media Foundation instead of the game's DirectShow path "
                 L"(the window stays responsive). Always on with the SacredBild (Direct3D 9) renderer.");
        ui.group(L"Screenshots and diagnostics");
        ui.combo(L"Screenshots:", {L"Screenshot", L"Format"}, {{L"PNG", L"png"}, {L"JPEG (smaller)", L"jpg"}},
              g_config.screenshotJpeg ? L"jpg" : L"png")
            .tip(L"Print Screen saves the whole screen as Capture\\shotNNNN.png (or .jpg) in the game folder.");
        ui.combo(L"Crash dumps:", {L"Debug", L"CrashDump"}, {{L"Off", L"0"}, {L"Small", L"1"}, {L"All memory (large)", L"2"}},
              std::to_wstring(g_config.crashDump))
            .tip(L"When the game crashes, write SacredBild-crash-<date>-<time>.dmp next to the game exe (please attach "
                 L"it to bug reports). Small = threads and the memory they point to; all memory = hundreds of MB.");
        ui.check(L"Frame statistics", {L"Debug", L"D3DStats"}, g_config.d3dStats)
            .width(86)
            .tip(L"Log draw-call statistics and frame timings once per second to SacredBild.log.");
        ui.sameLine();
        ui.check(L"UI trace", {L"Debug", L"UiTrace"}, g_config.uiTrace)
            .tip(L"Press Scroll Lock in game to log one frame of UI drawing (what is drawn where, and by which game "
                 L"code) and the tooltips shown during the next 5 seconds to SacredBild.log.");
        ui.check(L"Movie fallback", {L"Debug", L"MovieFallback"}, g_config.movieFallback)
            .width(86)
            .tip(L"Play the movies through the fallback (DirectShow into a system memory surface) even where Media "
                 L"Foundation works, as on systems without it (Windows 7, Windows N editions, Wine).");
        ui.sameLine();
        ui.check(L"Skinning check", {L"Debug", L"SkinCheck"}, g_config.skinCheck)
            .tip(L"Compare Granny's character skinning with SacredBild's own, logged every 10 seconds.");
        constexpr const wchar_t* kProfilerTip =
            L"Sample the presenting thread at this interval and write SacredBild-profile.txt every 15 seconds.";
        ui.check(L"Profiler, every", {L"Debug", L"Profiler"}, g_config.profiler).tip(kProfilerTip);
        ui.beginEnabledIf([](const Form& f) { return f.on(L"Debug", L"Profiler"); });
        ui.sameLine();
        ui.edit(nullptr, {L"Debug", L"ProfilerIntervalUs"}, std::to_wstring(g_config.profilerIntervalUs))
            .digits()
            .width(30)
            .tip(kProfilerTip);
        ui.sameLine();
        ui.label(L"µs");
        ui.endEnabledIf();

        ui.page(L"Network");
        ui.group(L"LAN games");
        ui.check(L"Announce hosted games on every adapter", {L"Net", L"Relay"}, g_config.netRelay)
            .tip(L"Hosting: the gameserver Sacred starts for your game announces it on every network adapter (VPN "
                 L"adapters included) with that adapter's address, and to players who list your PC under Hosts.");
        ui.check(L"Send game messages right away", {L"Net", L"NoDelay"}, g_config.netNoDelay)
            .tip(L"Send each message to the server right away in both data flow modes. With MODEM/ISDN the game holds "
                 L"small messages back until the previous one is acknowledged (Nagle's algorithm), adding latency. "
                 L"Off = as the game does.");
        ui.edit(L"UDP port:", {L"Net", L"Port"}, std::to_wstring(g_config.netPort))
            .digits()
            .width(32)
            .tip(L"Hosting: UDP port for players who list the host under Hosts, for the UDP game connection and for "
                 L"the matchmaker (allow it in the firewall on the host).");
        ui.sameLine();
        ui.edit(L"Join timeout (s):", {L"Net", L"JoinTimeout"}, std::to_wstring(g_config.netJoinTimeout))
            .digits()
            .width(30)
            .tip(L"Hosting: seconds a joining player has to send its first message to the gameserver. The game allows "
                 L"5, which slow or distant connections can miss (\"connect timed out\"). Minimum 5.");
        ui.edit(L"Hosts:", {L"Net", L"Hosts"}, widen(g_config.netHosts))
            .tip(L"Joining: PCs whose LAN games should show up although their broadcasts don't reach you (VPNs like "
                 L"WireGuard or Tailscale). Comma-separated IP addresses or names, e.g. 10.8.0.2, 10.8.0.3:2105");
        ui.column();
        ui.group(L"Online games");
        ui.check(L"Game connection over UDP", {L"Net", L"Udp"}, g_config.netUdp)
            .tip(L"Carry the game connection over UDP instead of TCP when the other side has this on too: lost "
                 L"packets are resent after about one round trip instead of after 300 ms and more, the connection "
                 L"recovers right away after an outage, and survives a change of your address (Wi-Fi to mobile). "
                 L"Joining a host without it connects over TCP as before. Hosting: allow the UDP port in the "
                 L"firewall.");
        ui.edit(L"Matchmaker:", {L"Net", L"Matchmaker"}, widen(g_config.netMatchmaker))
            .tip(L"A SacredBild matchmaking server (name or address, optionally :port, default 2107). Its games show "
                 L"up in the LAN list, and joining one gets you introduced to the host, so that hosts don't need "
                 L"port forwarding (needs the UDP game connection on both sides). Empty = none.");
        ui.check(L"Publish hosted games at the matchmaker", {L"Net", L"Publish"}, g_config.netPublish)
            .enabledIf([](const Form& f) { return !f.value(L"Net", L"Matchmaker").empty(); })
            .tip(L"Hosting: list your games at the matchmaker. Its web page shows their name and players, not your "
                 L"address; the SacredBild players it lists them for get the address, as they need it to connect.");

        ui.page(L"HUD layout");
        ui.text(L"With \"Place the HUD windows at the screen edges\" (General tab), each in-game window's 1024 x 768 "
                L"layout moves to its position below. Windows at the same position keep their original arrangement.");
        ui.group(L"Positions");
        ui.beginEnabledIf([](const Form& f) { return f.on(L"UI", L"Anchor"); });
        ui.label(L"");
        ui.sameLine();
        ui.label(L"Horizontal").width(100);
        ui.sameLine();
        ui.label(L"Vertical").width(100);
        const auto position = [](const wchar_t* low, const wchar_t* high) {
            return [=](int v) {
                return v == 0 ? std::wstring(low) : v == 2048 ? std::wstring(L"Center") : v == 4096 ? std::wstring(high)
                                                                                                    : std::format(L"Custom ({})", v);
            };
        };
        for (const HudWindow& window : kHudWindows)
        {
            const Config::UiPosition& pos = g_config.*window.position;
            ui.label(window.label).tip(window.tip);
            ui.sameLine();
            ui.combo(nullptr, {L"UI.Layout", window.key, 0}, {0, 2048, 4096}, pos.x, position(L"Left", L"Right"))
                .width(100)
                .tip(L"Against the left edge, centered or against the right edge of the room the screen leaves around "
                     L"the 1024 x 768 layout at the current UI size.");
            ui.sameLine();
            ui.combo(nullptr, {L"UI.Layout", window.key, 1}, {0, 2048, 4096}, pos.y, position(L"Top", L"Bottom"))
                .width(100)
                .tip(L"Against the top edge, centered or against the bottom edge of the room the screen leaves around "
                     L"the 1024 x 768 layout at the current UI size.");
        }
        ui.button(L"Defaults", [](Form& f) {
              const Config defaults;
              for (const HudWindow& window : kHudWindows)
              {
                  const Config::UiPosition& pos = defaults.*window.position;
                  f.set(L"UI.Layout", window.key, std::format(L"{},{}", pos.x, pos.y));
              }
          })
            .tip(L"Back to SacredBild's default positions.");
        ui.endEnabledIf();

        ui.footer();
        ui.check(L"Don't show this window again", {L"Launcher", L"HideSettingsWindow"}, !g_config.settingsWindow)
            .tip(L"Start the game right away from now on. To see this window again, hold Shift while the game starts "
                 L"or set HideSettingsWindow=0 in SacredBild.ini.");
        ui.buttons(L"Play", L"Exit");
    }

    BOOL CALLBACK firstResourceName(HMODULE, LPCWSTR, LPWSTR name, LONG_PTR param)
    {
        auto& out = *reinterpret_cast<std::wstring*>(param);
        out = IS_INTRESOURCE(name) ? std::format(L"#{}", reinterpret_cast<uintptr_t>(name)) : name;
        return FALSE;
    }

    // The exe's first icon group (the one Explorer shows) at the large and small icon sizes.
    class ExeIcons
    {
    public:
        ExeIcons()
        {
            const HMODULE exe = GetModuleHandleW(nullptr);
            std::wstring name;
            EnumResourceNamesW(exe, reinterpret_cast<LPCWSTR>(RT_GROUP_ICON), firstResourceName, reinterpret_cast<LONG_PTR>(&name));
            if (!name.empty())
            {
                big = load(exe, name, SM_CXICON, SM_CYICON);
                small = load(exe, name, SM_CXSMICON, SM_CYSMICON);
            }
        }

        ~ExeIcons()
        {
            for (HICON icon : {big, small})
            {
                if (icon)
                {
                    DestroyIcon(icon);
                }
            }
        }

        ExeIcons(const ExeIcons&) = delete;
        ExeIcons& operator=(const ExeIcons&) = delete;

        HICON big = nullptr;
        HICON small = nullptr;

    private:
        static HICON load(HMODULE exe, const std::wstring& name, int cx, int cy)
        {
            return static_cast<HICON>(LoadImageW(exe, name.c_str(), IMAGE_ICON, GetSystemMetrics(cx), GetSystemMetrics(cy), LR_DEFAULTCOLOR));
        }
    };

    // Writes the changed settings to the ini; true if any were written.
    bool save(const std::vector<Ui::Change>& changes, const std::wstring& ini)
    {
        std::wstring written;
        for (const Ui::Change& change : changes)
        {
            if (!WritePrivateProfileStringW(change.section.c_str(), change.name.c_str(), change.value.c_str(), ini.c_str()))
            {
                const DWORD error = GetLastError();
                LOG("Settings window: writing [{}] {} failed: {}", ascii(change.section), ascii(change.name), error);
                MessageBoxW(nullptr,
                    std::format(L"SacredBild couldn't save the settings to {} (error {}).\n\n"
                                L"The game starts with the settings saved there before.", ini, error).c_str(),
                    L"SacredBild", MB_OK | MB_ICONWARNING);
                break;
            }
            written += std::format(L" [{}] {}={}", change.section, change.name, change.value);
        }
        LOG("Settings window: Play;{}", written.empty() ? " nothing changed" : ascii(written));
        return !written.empty();
    }
}

bool SettingsWindow::wanted()
{
    return g_config.settingsWindow || (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
}

bool SettingsWindow::show(HMODULE module, const std::wstring& gameDir)
{
    const std::wstring ini = gameDir + L"\\SacredBild.ini";
    Form form;
    declare(form, ini);
    const ExeIcons icons;
    form.icons(icons.big, icons.small);
    switch (form.run(module, L"SacredBild settings"))
    {
    case Form::Result::Failed:
        return true;
    case Form::Result::Cancel:
        LOG("Settings window: Exit");
        return false;
    case Form::Result::Ok:
        break;
    }
    if (save(form.changes(), ini))
    {
        ConfigFile::load(gameDir);
    }
    return true;
}
