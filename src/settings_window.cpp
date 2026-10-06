#include "settings_window.h"
#include "settings_window_res.h"
#include "config.h"
#include "log.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <algorithm>
#include <deque>
#include <format>
#include <functional>
#include <string>
#include <vector>

namespace
{
    // How a control maps to its ini key. Combo box items carry their ini value (an index into Window::values).
    enum class Kind
    {
        Check,
        Combo,
        Edit,
    };

    struct Field
    {
        int id;
        const wchar_t* section;
        const wchar_t* key;
        Kind kind;
        const wchar_t* on = L"1";   // Check: the values for checked / unchecked
        const wchar_t* off = L"0";
    };

    // Everything but the resolution (two keys) and [UI.Layout] (two combo boxes per key).
    constexpr Field kFields[] = {
        {IDC_FRAME, L"Display", L"Borderless", Kind::Combo},
        {IDC_FPS_LIMIT, L"Display", L"FpsLimit", Kind::Combo},
        {IDC_VSYNC, L"Display", L"VSync", Kind::Check},
        {IDC_CLIP_CURSOR, L"Display", L"ClipCursor", Kind::Check},
        {IDC_FPS_INACTIVE, L"Display", L"FpsLimitInactive", Kind::Combo},
        {IDC_FRAME_LATENCY, L"Display", L"MaxFrameLatency", Kind::Combo},
        {IDC_UI_SCALE, L"UI", L"Scale", Kind::Combo},
        {IDC_UI_SCALE_MENUS, L"UI", L"ScaleMode", Kind::Check, L"Full", L"InGame"},
        {IDC_UI_ANCHOR, L"UI", L"Anchor", Kind::Check},
        {IDC_UI_LINEAR_FILTER, L"UI", L"LinearFilter", Kind::Check},
        {IDC_TEXTURE_BUDGET, L"Render", L"TextureBudgetMB", Kind::Combo},
        {IDC_BATCH, L"Render", L"Batch", Kind::Check},
        {IDC_BATCH_NO_CLIP, L"Render", L"BatchNoClip", Kind::Check},
        {IDC_BATCH_VERTEX_BUFFER, L"Render", L"BatchVertexBuffer", Kind::Check},
        {IDC_BATCH_MODELS, L"Render", L"BatchModels", Kind::Check},
        {IDC_BATCH_GROUND, L"Render", L"BatchGround", Kind::Check},
        {IDC_GPU_SKINNING, L"Render", L"GpuSkinning", Kind::Check},
        {IDC_OFFSCREEN_POSES, L"Render", L"OffscreenPoses", Kind::Combo},
        {IDC_ASYNC_ANIMATION, L"Render", L"AsyncAnimation", Kind::Check},
        {IDC_RECORD_INDEX, L"Render", L"RecordIndex", Kind::Check},
        {IDC_ATLAS, L"Render", L"Atlas", Kind::Check},
        {IDC_ATLAS_PAGE_SIZE, L"Render", L"AtlasPageSize", Kind::Combo},
        {IDC_ATLAS_PAGES, L"Render", L"AtlasPages", Kind::Combo},
        {IDC_ATLAS_MAX_TEXTURE, L"Render", L"AtlasMaxTextureSize", Kind::Combo},
        {IDC_SCREENSHOT_FORMAT, L"Screenshot", L"Format", Kind::Combo},
        {IDC_NET_RELAY, L"Net", L"Relay", Kind::Check},
        {IDC_NET_PORT, L"Net", L"Port", Kind::Edit},
        {IDC_NET_HOSTS, L"Net", L"Hosts", Kind::Edit},
        {IDC_NET_NO_DELAY, L"Net", L"NoDelay", Kind::Check},
        {IDC_NET_JOIN_TIMEOUT, L"Net", L"JoinTimeout", Kind::Edit},
        {IDC_BACKEND, L"DDraw", L"Backend", Kind::Combo},
        {IDC_DDRAW_CHAIN, L"DDraw", L"Chain", Kind::Edit},
        {IDC_D3D9_PATH, L"DDraw", L"D3D9", Kind::Edit},
        {IDC_MEDIA_FOUNDATION, L"DDraw", L"MediaFoundation", Kind::Check},
        {IDC_D3D_STATS, L"Debug", L"D3DStats", Kind::Check},
        {IDC_PROFILER, L"Debug", L"Profiler", Kind::Check},
        {IDC_PROFILER_INTERVAL, L"Debug", L"ProfilerIntervalUs", Kind::Edit},
        {IDC_UI_TRACE, L"Debug", L"UiTrace", Kind::Check},
        {IDC_MOVIE_FALLBACK, L"Debug", L"MovieFallback", Kind::Check},
        {IDC_SKIN_CHECK, L"Debug", L"SkinCheck", Kind::Check},
        {IDC_CRASH_DUMP, L"Debug", L"CrashDump", Kind::Combo},
        {IDC_HIDE, L"Launcher", L"HideSettingsWindow", Kind::Check},
    };

    // [UI.Layout]: label, X and Y combo box at id, id + 1, id + 2.
    struct LayoutRow
    {
        int id;
        const wchar_t* key;
        Config::UiPosition Config::*position;
    };

    constexpr LayoutRow kLayout[] = {
        {IDC_LAYOUT_TASKBAR, L"Taskbar", &Config::uiTaskbar},
        {IDC_LAYOUT_CHAT, L"Chat", &Config::uiChat},
        {IDC_LAYOUT_INVENTORY, L"Inventory", &Config::uiInventory},
        {IDC_LAYOUT_STATS, L"Stats", &Config::uiStats},
        {IDC_LAYOUT_EQUIPMENT, L"Equipment", &Config::uiEquipment},
        {IDC_LAYOUT_MINIMAP, L"Minimap", &Config::uiMinimap},
        {IDC_LAYOUT_PORTRAITS, L"Portraits", &Config::uiPortraits},
        {IDC_LAYOUT_SHOPS, L"Shops", &Config::uiShops},
    };

    constexpr const wchar_t* kPageNames[] = {L"General", L"Advanced", L"HUD layout"};
    constexpr int kPageIds[] = {IDD_PAGE_GENERAL, IDD_PAGE_ADVANCED, IDD_PAGE_LAYOUT};

    struct Entry
    {
        const wchar_t* section;
        const wchar_t* key;
        std::wstring value;
    };

    struct Choice
    {
        std::wstring text;
        std::wstring value;
    };

    struct Window
    {
        HMODULE module = nullptr;
        std::wstring ini;
        HWND dlg = nullptr;
        HWND tabs = nullptr;
        HWND pages[std::size(kPageIds)] = {};
        HWND tooltip = nullptr;
        HICON icons[2] = {};
        std::deque<std::wstring> values;   // combo box item values
        std::vector<Entry> initial;        // the ini values the controls showed at the start
        bool saved = false;                // settings written to the ini
    };

    Window* g_window = nullptr;

    // Controls by ID, on whichever page they are.
    HWND item(int id)
    {
        for (HWND page : g_window->pages)
        {
            if (HWND control = page ? GetDlgItem(page, id) : nullptr)
            {
                return control;
            }
        }
        return GetDlgItem(g_window->dlg, id);
    }

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
    bool iniMediaFoundation()
    {
        wchar_t buf[16] = {};
        GetPrivateProfileStringW(L"DDraw", L"MediaFoundation", L"1", buf, static_cast<DWORD>(std::size(buf)), g_window->ini.c_str());
        for (const wchar_t* no : {L"0", L"false", L"no", L"off"})
        {
            if (_wcsicmp(buf, no) == 0)
            {
                return false;
            }
        }
        return true;
    }

    // Adds the choices (and the current value as "Custom" if it isn't one of them) and selects the current one.
    void fillCombo(int id, std::vector<Choice> choices, const std::wstring& current)
    {
        if (std::ranges::none_of(choices, [&](const Choice& c) { return c.value == current; }))
        {
            choices.push_back({std::format(L"Custom ({})", current), current});
        }
        const HWND combo = item(id);
        for (const Choice& choice : choices)
        {
            g_window->values.push_back(choice.value);
            const LRESULT index = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choice.text.c_str()));
            SendMessageW(combo, CB_SETITEMDATA, index, static_cast<LPARAM>(g_window->values.size() - 1));
            if (choice.value == current)
            {
                SendMessageW(combo, CB_SETCURSEL, index, 0);
            }
        }
    }

    // Numeric choices with the current value among them, ascending.
    void fillNumbers(int id, std::vector<int> numbers, int current, const std::function<std::wstring(int)>& label)
    {
        numbers.push_back(current);
        std::ranges::sort(numbers);
        numbers.erase(std::unique(numbers.begin(), numbers.end()), numbers.end());
        std::vector<Choice> choices;
        for (int n : numbers)
        {
            choices.push_back({label(n), std::to_wstring(n)});
        }
        fillCombo(id, choices, std::to_wstring(current));
    }

    std::wstring comboValue(int id)
    {
        const HWND combo = item(id);
        const LRESULT index = SendMessageW(combo, CB_GETCURSEL, 0, 0);
        const LRESULT value = index == CB_ERR ? CB_ERR : SendMessageW(combo, CB_GETITEMDATA, index, 0);
        return value >= 0 && static_cast<size_t>(value) < g_window->values.size() ? g_window->values[value] : L"";
    }

    void selectValue(int id, const std::wstring& value)
    {
        const HWND combo = item(id);
        const LRESULT count = SendMessageW(combo, CB_GETCOUNT, 0, 0);
        for (LRESULT i = 0; i < count; ++i)
        {
            if (g_window->values[SendMessageW(combo, CB_GETITEMDATA, i, 0)] == value)
            {
                SendMessageW(combo, CB_SETCURSEL, i, 0);
                return;
            }
        }
    }

    bool checked(int id)
    {
        return SendMessageW(item(id), BM_GETCHECK, 0, 0) == BST_CHECKED;
    }

    void check(int id, bool on)
    {
        SendMessageW(item(id), BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    std::wstring text(int id)
    {
        const HWND edit = item(id);
        std::wstring s(GetWindowTextLengthW(edit), L'\0');
        GetWindowTextW(edit, s.data(), static_cast<int>(s.size() + 1));
        const size_t first = s.find_first_not_of(L' ');
        return first == std::wstring::npos ? L"" : s.substr(first, s.find_last_not_of(L' ') - first + 1);
    }

    void setText(int id, const std::wstring& s)
    {
        SetWindowTextW(item(id), s.c_str());
    }

    // The ini values the controls show now, in a fixed order.
    std::vector<Entry> collect()
    {
        std::vector<Entry> entries;
        for (const Field& field : kFields)
        {
            entries.push_back({field.section, field.key,
                field.kind == Kind::Check ? (checked(field.id) ? field.on : field.off)
                    : field.kind == Kind::Combo ? comboValue(field.id)
                                                : text(field.id)});
        }
        const std::wstring resolution = comboValue(IDC_RESOLUTION);
        const size_t comma = resolution.find(L',');
        entries.push_back({L"Display", L"Width", resolution.substr(0, comma)});
        entries.push_back({L"Display", L"Height", comma == std::wstring::npos ? L"" : resolution.substr(comma + 1)});
        for (const LayoutRow& row : kLayout)
        {
            entries.push_back({L"UI.Layout", row.key, comboValue(row.id + 1) + L"," + comboValue(row.id + 2)});
        }
        return entries;
    }

    void fillResolutions()
    {
        // The primary display's modes the UI fits into, largest first, and the ini's own size if it isn't one.
        std::vector<std::pair<int, int>> sizes;
        DEVMODEW mode = {};
        mode.dmSize = sizeof(mode);
        for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &mode); ++i)
        {
            if (mode.dmPelsWidth >= 1024 && mode.dmPelsHeight >= 768)
            {
                sizes.emplace_back(mode.dmPelsWidth, mode.dmPelsHeight);
            }
        }
        sizes.emplace_back(1024, 768);
        const bool desktop = g_config.width <= 0 || g_config.height <= 0;
        if (!desktop)
        {
            sizes.emplace_back(g_config.width, g_config.height);
        }
        std::ranges::sort(sizes, std::greater{});
        sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());

        std::vector<Choice> choices{
            {std::format(L"Desktop ({} x {})", GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)), L"0,0"}};
        for (const auto [w, h] : sizes)
        {
            choices.push_back({std::format(L"{} x {}{}", w, h, w == 1024 && h == 768 ? L" (original)" : L""), std::format(L"{},{}", w, h)});
        }
        fillCombo(IDC_RESOLUTION, choices, desktop ? L"0,0" : std::format(L"{},{}", g_config.width, g_config.height));
    }

    void fillGeneral()
    {
        fillResolutions();
        fillCombo(IDC_FRAME, {{L"Automatic", L"auto"}, {L"Borderless", L"1"}, {L"Window frame", L"0"}},
            g_config.frame == Config::Frame::Auto ? L"auto" : g_config.frame == Config::Frame::Never ? L"1" : L"0");

        DEVMODEW mode = {};
        mode.dmSize = sizeof(mode);
        const int refresh = EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1
            ? static_cast<int>(mode.dmDisplayFrequency) : 0;
        std::vector<int> limits{0, 30, 60, 75, 90, 120, 144, 165, 240};
        if (refresh)
        {
            limits.push_back(refresh);
        }
        fillNumbers(IDC_FPS_LIMIT, limits, std::max(g_config.fpsLimit, 0), [&](int fps) {
            return fps == 0      ? std::wstring(L"Off")
                : fps == 60      ? std::format(L"60 fps (original{})", fps == refresh ? L", display refresh" : L"")
                : fps == refresh ? std::format(L"{} fps (display refresh)", fps)
                                 : std::format(L"{} fps", fps);
        });
        check(IDC_VSYNC, g_config.vsync);
        check(IDC_CLIP_CURSOR, g_config.clipCursor);

        // Percent in the list, the ini's factor as the value.
        std::vector<Choice> scales{{L"Fit to the screen height", L"0"}};
        for (int percent : {100, 125, 150, 175, 200, 250, 300})
        {
            scales.push_back({percent == 100 ? std::wstring(L"100 % (native pixels)") : std::format(L"{} %", percent),
                std::format(L"{}", percent / 100.0)});
        }
        fillCombo(IDC_UI_SCALE, scales, g_config.uiScale > 0.0f ? std::format(L"{}", g_config.uiScale) : L"0");
        check(IDC_UI_SCALE_MENUS, g_config.uiScaleMenus);
        check(IDC_UI_ANCHOR, g_config.uiAnchor);
        check(IDC_UI_LINEAR_FILTER, g_config.uiLinearFilter);

        fillCombo(IDC_BACKEND, {{L"SacredBild (Direct3D 9)", L"d3d9"}, {L"Chain-loaded ddraw", L"chain"}},
            g_config.ddrawD3D9 ? L"d3d9" : L"chain");
        check(IDC_GPU_SKINNING, g_config.gpuSkinning);
    }

    void fillAdvanced()
    {
        fillNumbers(IDC_FPS_INACTIVE, {0, 10, 15, 20, 30, 60}, std::max(g_config.fpsLimitInactive, 0),
            [](int fps) { return fps ? std::format(L"{} fps", fps) : std::wstring(L"Off"); });
        fillNumbers(IDC_FRAME_LATENCY, {1, 2, 3}, g_config.maxFrameLatency, [](int frames) {
            return frames <= 0 ? std::wstring(L"Driver default")
                : frames == 1  ? std::wstring(L"1 (lowest latency)")
                               : std::to_wstring(frames);
        });
        fillNumbers(IDC_TEXTURE_BUDGET, {0, 256, 512, 1024, 2048}, g_config.textureBudgetMB,
            [](int mb) { return mb ? std::format(L"{} MB", mb) : std::wstring(L"Automatic"); });
        fillNumbers(IDC_OFFSCREEN_POSES, {1, 2, 4, 8}, g_config.offscreenPoses, [](int n) {
            return n <= 1 ? std::wstring(L"Every frame") : n == 2 ? std::wstring(L"Every 2nd frame") : std::format(L"Every {}th frame", n);
        });
        check(IDC_BATCH, g_config.batch);
        check(IDC_BATCH_NO_CLIP, g_config.batchNoClip);
        check(IDC_BATCH_VERTEX_BUFFER, g_config.batchVertexBuffer);
        check(IDC_BATCH_MODELS, g_config.batchModels);
        check(IDC_BATCH_GROUND, g_config.batchGround);
        check(IDC_ATLAS, g_config.atlas);
        fillNumbers(IDC_ATLAS_PAGE_SIZE, {2048, 4096, 8192, 16384}, g_config.atlasPageSize,
            [](int px) { return std::format(L"{} px", px); });
        fillNumbers(IDC_ATLAS_PAGES, {1, 2, 3, 4}, g_config.atlasPages,
            [](int n) { return std::format(L"{} page{}", n, n == 1 ? L"" : L"s"); });
        fillNumbers(IDC_ATLAS_MAX_TEXTURE, {128, 256, 512, 1024}, g_config.atlasMaxTextureSize,
            [](int px) { return std::format(L"up to {} px", px); });
        check(IDC_ASYNC_ANIMATION, g_config.asyncAnimation);
        check(IDC_RECORD_INDEX, g_config.recordIndex);

        check(IDC_NET_RELAY, g_config.netRelay);
        check(IDC_NET_NO_DELAY, g_config.netNoDelay);
        setText(IDC_NET_PORT, std::to_wstring(g_config.netPort));
        setText(IDC_NET_JOIN_TIMEOUT, std::to_wstring(g_config.netJoinTimeout));
        setText(IDC_NET_HOSTS, widen(g_config.netHosts));

        setText(IDC_DDRAW_CHAIN, g_config.ddrawChain);
        setText(IDC_D3D9_PATH, g_config.d3d9Path);
        check(IDC_MEDIA_FOUNDATION, iniMediaFoundation());

        fillCombo(IDC_SCREENSHOT_FORMAT, {{L"PNG", L"png"}, {L"JPEG (smaller)", L"jpg"}}, g_config.screenshotJpeg ? L"jpg" : L"png");
        fillCombo(IDC_CRASH_DUMP, {{L"Off", L"0"}, {L"Small", L"1"}, {L"All memory (large)", L"2"}}, std::to_wstring(g_config.crashDump));
        check(IDC_D3D_STATS, g_config.d3dStats);
        check(IDC_UI_TRACE, g_config.uiTrace);
        check(IDC_MOVIE_FALLBACK, g_config.movieFallback);
        check(IDC_SKIN_CHECK, g_config.skinCheck);
        check(IDC_PROFILER, g_config.profiler);
        setText(IDC_PROFILER_INTERVAL, std::to_wstring(g_config.profilerIntervalUs));
    }

    void fillLayout()
    {
        const auto label = [](const wchar_t* low, const wchar_t* high) {
            return [=](int v) {
                return v == 0      ? std::wstring(low)
                    : v == 2048    ? std::wstring(L"Center")
                    : v == 4096    ? std::wstring(high)
                                   : std::format(L"Custom ({})", v);
            };
        };
        for (const LayoutRow& row : kLayout)
        {
            const Config::UiPosition& pos = g_config.*row.position;
            fillNumbers(row.id + 1, {0, 2048, 4096}, pos.x, label(L"Left", L"Right"));
            fillNumbers(row.id + 2, {0, 2048, 4096}, pos.y, label(L"Top", L"Bottom"));
        }
    }

    void layoutDefaults()
    {
        const Config defaults;
        for (const LayoutRow& row : kLayout)
        {
            selectValue(row.id + 1, std::to_wstring((defaults.*row.position).x));
            selectValue(row.id + 2, std::to_wstring((defaults.*row.position).y));
        }
    }

    // Settings that do nothing with the current choices are grayed out.
    void updateEnabled()
    {
        const auto enable = [](std::initializer_list<int> ids, bool on) {
            for (int id : ids)
            {
                EnableWindow(item(id), on);
            }
        };
        const bool d3d9 = comboValue(IDC_BACKEND) == L"d3d9";
        const bool batch = checked(IDC_BATCH);
        enable({IDC_VSYNC, IDC_GPU_SKINNING, IDC_FRAME_LATENCY_LABEL, IDC_FRAME_LATENCY, IDC_D3D9_PATH_LABEL, IDC_D3D9_PATH}, d3d9);
        enable({IDC_DDRAW_CHAIN_LABEL, IDC_DDRAW_CHAIN, IDC_MEDIA_FOUNDATION}, !d3d9);
        enable({IDC_OFFSCREEN_POSES_LABEL, IDC_OFFSCREEN_POSES}, d3d9 && checked(IDC_GPU_SKINNING));
        enable({IDC_UI_SCALE_MENUS}, comboValue(IDC_UI_SCALE) != L"0");
        enable({IDC_BATCH_NO_CLIP, IDC_BATCH_VERTEX_BUFFER, IDC_BATCH_MODELS, IDC_BATCH_GROUND, IDC_ATLAS}, batch);
        enable({IDC_ATLAS_PAGE_SIZE, IDC_ATLAS_PAGES, IDC_ATLAS_MAX_TEXTURE}, batch && checked(IDC_ATLAS));
        enable({IDC_PROFILER_INTERVAL, IDC_PROFILER_INTERVAL_UNIT}, checked(IDC_PROFILER));
        const bool anchor = checked(IDC_UI_ANCHOR);
        for (const LayoutRow& row : kLayout)
        {
            enable({row.id, row.id + 1, row.id + 2}, anchor);
        }
        enable({IDC_LAYOUT_DEFAULTS}, anchor);
    }

    void addTooltips()
    {
        INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_WIN95_CLASSES};
        InitCommonControlsEx(&icc);
        const HWND tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, g_window->dlg, nullptr, g_window->module, nullptr);
        g_window->tooltip = tooltip;
        if (!tooltip)
        {
            return;
        }
        // Wrap at the width of 220 dialog units (follows the dialog's DPI scaling), stay up while the mouse rests.
        RECT wrap = {0, 0, 220, 0};
        MapDialogRect(g_window->dlg, &wrap);
        SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, wrap.right);
        SendMessageW(tooltip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);

        static const struct
        {
            std::vector<int> ids;
            const wchar_t* text;
        } kTips[] = {
            // General
            {{IDC_RESOLUTION_LABEL, IDC_RESOLUTION},
                L"The resolution the game renders at. Desktop = the screen's resolution; a smaller one plays in a "
                L"window. 1024 x 768 runs the original game without patches."},
            {{IDC_FRAME_LABEL, IDC_FRAME},
                L"Automatic: a frame with a title bar (to move the window) when the window is smaller than the screen, "
                L"none when it fills the screen.\nBorderless: never a frame.\nWindow frame: always a frame.\n"
                L"The area inside the frame has the chosen resolution either way."},
            {{IDC_FPS_LIMIT_LABEL, IDC_FPS_LIMIT},
                L"The game's own frame limit in game (it uses 60), e.g. 144 for a 144 Hz display. The menus are unchanged."},
            {{IDC_VSYNC},
                L"Show each frame on the display's refresh (no tearing). Off: show frames right away. "
                L"Needs the SacredBild (Direct3D 9) renderer."},
            {{IDC_CLIP_CURSOR},
                L"Keep the mouse inside the game window while it is in the foreground (multiple monitors, a smaller "
                L"window on a wide screen). Hold Alt to move the mouse out of the window."},
            {{IDC_UI_SCALE_LABEL, IDC_UI_SCALE},
                L"The game's 1024 x 768 interface is drawn centered and scaled. Fit = as large as fits the screen "
                L"height; a percentage = a fixed size (100 % = native pixels), capped at what fits."},
            {{IDC_UI_SCALE_MENUS},
                L"Use the UI size for the menus, the full-screen windows in game (options, save/load, map) and the "
                L"loading screen too. Off: those always fill the screen height."},
            {{IDC_UI_ANCHOR},
                L"In game, place the taskbar, minimap, inventory and the other HUD windows at the screen edges "
                L"(HUD layout tab). Off: the whole interface stays in the centered 1024 x 768 area."},
            {{IDC_UI_LINEAR_FILTER},
                L"Bilinear filtering for the scaled interface. Off: the game's sharp point sampling."},
            {{IDC_BACKEND_LABEL, IDC_BACKEND},
                L"SacredBild (Direct3D 9): SacredBild runs the game's DirectDraw / Direct3D 7 on Direct3D 9 itself.\n"
                L"Chain-loaded ddraw: the ddraw.dll set on the Advanced tab (DDrawCompat or another wrapper), else "
                L"Windows' own."},
            {{IDC_GPU_SKINNING},
                L"Animate (skin) characters and their shadows in a vertex shader instead of on the CPU. "
                L"Needs the SacredBild (Direct3D 9) renderer."},
            {{IDC_HIDE},
                L"Start the game right away from now on. To see this window again, hold Shift while the game starts "
                L"or set HideSettingsWindow=0 in SacredBild.ini."},
            // Advanced
            {{IDC_FPS_INACTIVE_LABEL, IDC_FPS_INACTIVE},
                L"Frame limit while the game is in the background (another window has the focus), in game and in the menus."},
            {{IDC_FRAME_LATENCY_LABEL, IDC_FRAME_LATENCY},
                L"Frames the CPU may prepare ahead of the GPU. 1 = lowest input latency. "
                L"Needs the SacredBild (Direct3D 9) renderer."},
            {{IDC_TEXTURE_BUDGET_LABEL, IDC_TEXTURE_BUDGET},
                L"Texture memory the game may keep loaded. A zoomed-out view at a high resolution shows far more "
                L"different ground textures than the original 1024 x 768. Automatic = the game's own value, at least 256 MB."},
            {{IDC_OFFSCREEN_POSES_LABEL, IDC_OFFSCREEN_POSES},
                L"Characters not drawn in the last frames get their skeleton posed this often instead of every frame "
                L"(most animated characters are off screen). Every frame = as the game does. Needs GPU animation."},
            {{IDC_BATCH},
                L"Merge the world view's thousands of small draw calls into few large ones. Off = draw as the game does."},
            {{IDC_BATCH_NO_CLIP}, L"Let the GPU clip merged draws instead of Direct3D 7 on the CPU."},
            {{IDC_BATCH_VERTEX_BUFFER}, L"Hand merged draws to Direct3D in vertex buffers instead of plain memory."},
            {{IDC_BATCH_MODELS},
                L"Send 3D models (characters and their shadows) through the same vertex buffers, merged where they can be."},
            {{IDC_BATCH_GROUND},
                L"Hand the ground's quads to the merging directly instead of through three device calls per quad."},
            {{IDC_ATLAS},
                L"Copy small textures into large shared pages so draws with different textures can be merged too."},
            {{IDC_ATLAS_PAGE_SIZE}, L"Atlas page size in texels (clamped to what the GPU supports)."},
            {{IDC_ATLAS_PAGES}, L"Atlas pages per texture format; the least recently used one is reused when full."},
            {{IDC_ATLAS_MAX_TEXTURE}, L"The largest texture copied into the atlas; larger ones are used directly."},
            {{IDC_ASYNC_ANIMATION},
                L"Advance the 3D animations on a second thread while the frame starts drawing (needs a second CPU core)."},
            {{IDC_RECORD_INDEX},
                L"Hash index in front of the game's record caches (looked up for every ground tile and object)."},
            {{IDC_NET_RELAY},
                L"Hosting: the gameserver Sacred starts for your game announces it on every network adapter (VPN "
                L"adapters included) with that adapter's address, and to players who list your PC under Hosts."},
            {{IDC_NET_NO_DELAY},
                L"Send each message to the server right away in both data flow modes. With MODEM/ISDN the game holds "
                L"small messages back until the previous one is acknowledged (Nagle's algorithm), adding latency. "
                L"Off = as the game does."},
            {{IDC_NET_PORT_LABEL, IDC_NET_PORT},
                L"UDP port for players who list the host under Hosts (allow it in the firewall on the host)."},
            {{IDC_NET_JOIN_TIMEOUT_LABEL, IDC_NET_JOIN_TIMEOUT},
                L"Hosting: seconds a joining player has to send its first message to the gameserver. The game allows "
                L"5, which slow or distant connections can miss (\"connect timed out\"). Minimum 5."},
            {{IDC_NET_HOSTS_LABEL, IDC_NET_HOSTS},
                L"Joining: PCs whose LAN games should show up although their broadcasts don't reach you (VPNs like "
                L"WireGuard or Tailscale). Comma-separated IP addresses or names, e.g. 10.8.0.2, 10.8.0.3:2105"},
            {{IDC_DDRAW_CHAIN_LABEL, IDC_DDRAW_CHAIN},
                L"Chain-loaded ddraw renderer: the ddraw.dll to load behind SacredBild, relative to the game folder "
                L"(DDrawCompat or another wrapper). Empty = Windows' own ddraw.dll."},
            {{IDC_D3D9_PATH_LABEL, IDC_D3D9_PATH},
                L"SacredBild (Direct3D 9) renderer: the d3d9.dll to use, e.g. DXVK's (relative to the game folder or "
                L"absolute). Empty: a d3d9.dll next to the game exe if there is one, else Windows' own."},
            {{IDC_MEDIA_FOUNDATION},
                L"Play the intro and cutscene movies through Media Foundation instead of the game's DirectShow path "
                L"(the window stays responsive). Always on with the SacredBild (Direct3D 9) renderer."},
            {{IDC_SCREENSHOT_FORMAT_LABEL, IDC_SCREENSHOT_FORMAT},
                L"Print Screen saves the whole screen as Capture\\shotNNNN.png (or .jpg) in the game folder."},
            {{IDC_CRASH_DUMP_LABEL, IDC_CRASH_DUMP},
                L"When the game crashes, write SacredBild-crash-<date>-<time>.dmp next to the game exe (please attach "
                L"it to bug reports). Small = threads and the memory they point to; all memory = hundreds of MB."},
            {{IDC_D3D_STATS}, L"Log draw-call statistics and frame timings once per second to SacredBild.log."},
            {{IDC_UI_TRACE},
                L"Press Scroll Lock in game to log one frame of UI drawing (what is drawn where, and by which game "
                L"code) and the tooltips shown during the next 5 seconds to SacredBild.log."},
            {{IDC_MOVIE_FALLBACK},
                L"Play the movies through the fallback (DirectShow into a system memory surface) even where Media "
                L"Foundation works, as on systems without it (Windows 7, Windows N editions, Wine)."},
            {{IDC_SKIN_CHECK}, L"Compare Granny's character skinning with SacredBild's own, logged every 10 seconds."},
            {{IDC_PROFILER, IDC_PROFILER_INTERVAL},
                L"Sample the presenting thread at this interval and write SacredBild-profile.txt every 15 seconds."},
            // HUD layout
            {{IDC_LAYOUT_PORTRAITS}, L"Party portraits (multiplayer)."},
            {{IDC_LAYOUT_SHOPS}, L"Blacksmith, merchant, combat art master, chest, cube and trade windows."},
            {{IDC_LAYOUT_DEFAULTS}, L"Back to SacredBild's default positions."},
        };
        const auto add = [&](int id, const wchar_t* text) {
            const HWND control = item(id);
            TTTOOLINFOW tool = {sizeof(tool)};
            tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            tool.hwnd = GetParent(control);
            tool.uId = reinterpret_cast<UINT_PTR>(control);
            tool.lpszText = const_cast<wchar_t*>(text);
            SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        };
        for (const auto& tip : kTips)
        {
            for (int id : tip.ids)
            {
                add(id, tip.text);
            }
        }
        for (const LayoutRow& row : kLayout)
        {
            add(row.id + 1, L"Against the left edge, centered or against the right edge of the room the screen leaves "
                            L"around the 1024 x 768 layout at the current UI size.");
            add(row.id + 2, L"Against the top edge, centered or against the bottom edge of the room the screen leaves "
                            L"around the 1024 x 768 layout at the current UI size.");
        }
    }

    BOOL CALLBACK firstResourceName(HMODULE, LPCWSTR, LPWSTR name, LONG_PTR param)
    {
        auto& out = *reinterpret_cast<std::wstring*>(param);
        out = IS_INTRESOURCE(name) ? std::format(L"#{}", reinterpret_cast<uintptr_t>(name)) : name;
        return FALSE;
    }

    // The exe's first icon group, the one Explorer shows.
    void setIcons()
    {
        const HMODULE exe = GetModuleHandleW(nullptr);
        std::wstring name;
        EnumResourceNamesW(exe, reinterpret_cast<LPCWSTR>(RT_GROUP_ICON), firstResourceName, reinterpret_cast<LONG_PTR>(&name));
        if (name.empty())
        {
            return;
        }
        const int sizes[2][2] = {{SM_CXICON, SM_CYICON}, {SM_CXSMICON, SM_CYSMICON}};
        for (int i = 0; i < 2; ++i)
        {
            g_window->icons[i] = static_cast<HICON>(LoadImageW(exe, name.c_str(), IMAGE_ICON,
                GetSystemMetrics(sizes[i][0]), GetSystemMetrics(sizes[i][1]), LR_DEFAULTCOLOR));
            SendMessageW(g_window->dlg, WM_SETICON, i == 0 ? ICON_BIG : ICON_SMALL, reinterpret_cast<LPARAM>(g_window->icons[i]));
        }
    }

    void showPage(int index)
    {
        for (int i = 0; i < static_cast<int>(std::size(g_window->pages)); ++i)
        {
            ShowWindow(g_window->pages[i], i == index ? SW_SHOW : SW_HIDE);
        }
    }

    // Pages pass their commands on to the window.
    INT_PTR CALLBACK pageProc(HWND page, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_INITDIALOG)
        {
            // The tab control's themed background behind the page.
            using EnableThemeDialogTextureFn = HRESULT(WINAPI*)(HWND, DWORD);
            if (const HMODULE uxtheme = GetModuleHandleW(L"uxtheme.dll"))
            {
                if (auto enable = reinterpret_cast<EnableThemeDialogTextureFn>(GetProcAddress(uxtheme, "EnableThemeDialogTexture")))
                {
                    enable(page, ETDT_ENABLETAB);
                }
            }
            return FALSE;
        }
        if (msg == WM_COMMAND && g_window->dlg)
        {
            SendMessageW(g_window->dlg, WM_COMMAND, wp, lp);
            return TRUE;
        }
        return FALSE;
    }

    void init(HWND dlg)
    {
        g_window->dlg = dlg;
        g_window->tabs = GetDlgItem(dlg, IDC_TABS);
        LoadLibraryW(L"uxtheme.dll");
        for (int i = 0; i < static_cast<int>(std::size(kPageIds)); ++i)
        {
            TCITEMW tab = {TCIF_TEXT};
            tab.pszText = const_cast<wchar_t*>(kPageNames[i]);
            SendMessageW(g_window->tabs, TCM_INSERTITEMW, i, reinterpret_cast<LPARAM>(&tab));
        }
        RECT area;
        GetClientRect(g_window->tabs, &area);
        TabCtrl_AdjustRect(g_window->tabs, FALSE, &area);
        for (int i = 0; i < static_cast<int>(std::size(kPageIds)); ++i)
        {
            g_window->pages[i] = CreateDialogParamW(g_window->module, MAKEINTRESOURCEW(kPageIds[i]), g_window->tabs, pageProc, 0);
            SetWindowPos(g_window->pages[i], HWND_TOP, area.left, area.top, area.right - area.left, area.bottom - area.top, 0);
        }
        showPage(0);

        setIcons();
        fillGeneral();
        fillAdvanced();
        fillLayout();
        check(IDC_HIDE, !g_config.settingsWindow);
        updateEnabled();
        addTooltips();
        g_window->initial = collect();
        SetForegroundWindow(dlg);
    }

    // Writes the settings changed in the window to the ini; true if any were written.
    bool save()
    {
        const std::vector<Entry> now = collect();
        std::wstring written;
        for (size_t i = 0; i < now.size(); ++i)
        {
            const Entry& entry = now[i];
            if (entry.value == g_window->initial[i].value)
            {
                continue;
            }
            if (!WritePrivateProfileStringW(entry.section, entry.key, entry.value.c_str(), g_window->ini.c_str()))
            {
                const DWORD error = GetLastError();
                LOG("Settings window: writing [{}] {} failed: {}", ascii(entry.section), ascii(entry.key), error);
                MessageBoxW(g_window->dlg,
                    std::format(L"SacredBild couldn't save the settings to {} (error {}).\n\n"
                                L"The game starts with the settings saved there before.", g_window->ini, error).c_str(),
                    L"SacredBild", MB_OK | MB_ICONWARNING);
                break;
            }
            written += std::format(L" [{}] {}={}", entry.section, entry.key, entry.value);
        }
        LOG("Settings window: Play;{}", written.empty() ? " nothing changed" : ascii(written));
        return !written.empty();
    }

    INT_PTR CALLBACK dialogProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_INITDIALOG:
            init(dlg);
            return TRUE;
        case WM_NOTIFY:
            if (const auto* header = reinterpret_cast<const NMHDR*>(lp); header->idFrom == IDC_TABS && header->code == TCN_SELCHANGE)
            {
                showPage(TabCtrl_GetCurSel(g_window->tabs));
                return TRUE;
            }
            break;
        case WM_COMMAND:
            switch (LOWORD(wp))
            {
            case IDOK:
                g_window->saved = save();
                EndDialog(dlg, IDOK);
                return TRUE;
            case IDCANCEL:
                EndDialog(dlg, IDCANCEL);
                return TRUE;
            case IDC_LAYOUT_DEFAULTS:
                layoutDefaults();
                return TRUE;
            }
            if (HIWORD(wp) == BN_CLICKED || HIWORD(wp) == CBN_SELCHANGE)
            {
                updateEnabled();
            }
            return TRUE;
        }
        return FALSE;
    }

    // Common Controls 6 (themed controls, tooltips) for the window, from the manifest in SacredBild's resources:
    // the game exe has no manifest asking for it.
    class VisualStyles
    {
    public:
        explicit VisualStyles(HMODULE module)
        {
            wchar_t path[MAX_PATH] = {};
            GetModuleFileNameW(module, path, MAX_PATH);
            ACTCTXW ctx = {sizeof(ctx)};
            ctx.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
            ctx.lpSource = path;
            ctx.hModule = module;
            ctx.lpResourceName = MAKEINTRESOURCEW(IDR_SETTINGS_MANIFEST);
            m_context = CreateActCtxW(&ctx);
            if (m_context == INVALID_HANDLE_VALUE || !ActivateActCtx(m_context, &m_cookie))
            {
                LOG("Settings window: no Common Controls 6 activation context: {}", GetLastError());
            }
        }

        ~VisualStyles()
        {
            if (m_cookie)
            {
                DeactivateActCtx(0, m_cookie);
            }
            if (m_context != INVALID_HANDLE_VALUE)
            {
                ReleaseActCtx(m_context);
            }
        }

        VisualStyles(const VisualStyles&) = delete;
        VisualStyles& operator=(const VisualStyles&) = delete;

    private:
        HANDLE m_context = INVALID_HANDLE_VALUE;
        ULONG_PTR m_cookie = 0;
    };
}

bool SettingsWindow::wanted()
{
    return g_config.settingsWindow || (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
}

bool SettingsWindow::show(HMODULE module, const std::wstring& gameDir)
{
    Window window;
    window.module = module;
    window.ini = gameDir + L"\\SacredBild.ini";
    g_window = &window;
    INT_PTR result;
    {
        VisualStyles styles(module);
        result = DialogBoxParamW(module, MAKEINTRESOURCEW(IDD_SETTINGS), nullptr, dialogProc, 0);
    }
    g_window = nullptr;
    for (HICON icon : window.icons)
    {
        if (icon)
        {
            DestroyIcon(icon);
        }
    }
    if (result == -1)
    {
        LOG("Settings window: DialogBoxParam failed: {}", GetLastError());
        return true;
    }
    if (result == IDCANCEL)
    {
        LOG("Settings window: Exit");
        return false;
    }
    if (window.saved)
    {
        ConfigFile::load(gameDir);
    }
    return true;
}
