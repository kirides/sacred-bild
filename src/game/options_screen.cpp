#include "game/options_screen.h"
#include "game/controller.h"
#include "game/language.h"
#include "game/sacred_addr.h"
#include "game/ui_canvas.h"
#include "input/bindings.h"
#include "input/gamepad.h"
#include "input/input_mode.h"
#include "overlay/overlay.h"
#include "config.h"
#include "log.h"
#include "patch.h"

#include <imgui.h>

#include <windows.h>
#include <initializer_list>
#include <string_view>
#include <array>
#include <atomic>
#include <string>

namespace
{
    using namespace Sacred;
    using Bindings::Action;
    using Bindings::Binding;

    // thiscall targets, called and wrapped as fastcall with an unused EDX.
    using ShowFn = void(__fastcall*)(void* self, void* edx, int show);
    using RenderFn = void(__fastcall*)(void* self, void* edx, void* device);
    using Render2Fn = void(__fastcall*)(void* self, void* edx, void* device, int a, int b);
    using FlagsFn = void(__fastcall*)(void* control, void* edx, uint32_t mask);
    using SliderGetFn = uint32_t(__fastcall*)(void* slider);
    using SliderSetFn = void(__fastcall*)(void* slider, void* edx, uint32_t value);
    using RectFn = void(__fastcall*)(void* control, void* edx, int32_t* out);

    ShowFn g_origShow = nullptr;
    RenderFn g_origRender = nullptr;
    Render2Fn g_origRender2 = nullptr;

    // The game's options window while the screen stands in for it, and until the game closes it after Accept /
    // Cancel (or a moment later, should the click not close it: then the game's own window shows).
    std::atomic<void*> g_window{nullptr};
    std::atomic<DWORD> g_hideUntil{0};
    std::atomic<bool> g_screen{false};          // the screen is up (not yet accepted or cancelled)
    std::atomic<DWORD> g_openTick{0};

    constexpr DWORD kCloseGraceMs = 750;
    constexpr DWORD kCaptureTimeoutMs = 8000;

    template <class T>
    T& member(void* obj, uintptr_t offset)
    {
        return *reinterpret_cast<T*>(static_cast<uint8_t*>(obj) + offset);
    }

    std::string utf8(const std::wstring& w)
    {
        if (w.empty())
        {
            return {};
        }
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
        return s;
    }

    // The game's own text for `key`, else the English one.
    std::string label(const char* key, const char* fallback)
    {
        const std::string s = utf8(Language::text(key));
        return s.empty() ? fallback : s;
    }

    // ---- The game window's controls ----

    void* control(void* window, uintptr_t offset)
    {
        return member<void*>(window, offset);
    }

    bool checked(void* c)
    {
        return c && (member<uint32_t>(c, UiControl::flags) & Options::checked);
    }

    bool shown(void* c)
    {
        return c && (member<uint32_t>(c, UiControl::flags) & 1);
    }

    void setChecked(void* c, bool on)
    {
        if (c)
        {
            reinterpret_cast<FlagsFn>(on ? Addr::cUI_Control2_setFlags : Addr::cUI_Control2_clearFlags)(c, nullptr, Options::checked);
        }
    }

    int radio(void* window, uintptr_t first, int count)
    {
        for (int i = 0; i < count; ++i)
        {
            if (checked(control(window, first + 4 * i)))
            {
                return i;
            }
        }
        return 0;
    }

    void setRadio(void* window, uintptr_t first, int count, int value)
    {
        for (int i = 0; i < count; ++i)
        {
            setChecked(control(window, first + 4 * i), i == value);
        }
    }

    struct Slider
    {
        int value = 0;
        int max = 0;    // 0: no slider
    };

    Slider readSlider(void* window, uintptr_t offset)
    {
        void* s = control(window, offset);
        if (!s)
        {
            return {};
        }
        const uint32_t count = member<uint32_t>(s, Sacred::Slider::count);
        if (count == 0 || count > 100000)
        {
            return {};
        }
        return {static_cast<int>(reinterpret_cast<SliderGetFn>(Addr::cUI_Slider_getValue)(s)), static_cast<int>(count - 1)};
    }

    void writeSlider(void* window, uintptr_t offset, const Slider& slider)
    {
        if (void* s = control(window, offset); s && slider.max > 0)
        {
            reinterpret_cast<SliderSetFn>(Addr::cUI_Slider_setValue)(s, nullptr, static_cast<uint32_t>(slider.value));
        }
    }

    // A click at the center of one of the window's buttons, the way the player would.
    void press(void* window, uintptr_t offset)
    {
        void* button = control(window, offset);
        if (!button)
        {
            return;
        }
        int32_t rect[3] = {};
        reinterpret_cast<RectFn>(Addr::cUI_Control2_getAbsoluteRect)(button, nullptr, rect);
        const int w = static_cast<int16_t>(rect[2] & 0xFFFF), h = static_cast<int16_t>(rect[2] >> 16);
        Controller::clickAt(UiCanvas::toPhysicalX(rect[0] + w / 2), UiCanvas::toPhysicalY(rect[1] + h / 2));
    }

    // ---- What the screen edits ----

    struct GameOptions
    {
        Slider sfx, voice, music, mapAlpha;
        int detail = 0, pickupAuto = 0, soundQuality = 0;
        bool netSlow = false;
        bool pickupAnim = false, autoTrack = false, violence = false, sound = false, exploreMap = false;
        bool autosave = false, fsaa = false;
        bool violenceShown = false;
    };

    struct ControllerOptions
    {
        int deadzone, cursorSpeed, moveRadius, aimRange, aimCone;
        bool artClick, walk, prompts;
        std::array<Binding, Bindings::actionCount> bindings;

        bool operator==(const ControllerOptions&) const = default;
    };

    struct Labels
    {
        std::string volume, sfx, speech, music;
        std::string soundOptions, sound, quality, qualityLow, qualityMid, qualityHigh;
        std::string graphics, detail, low, medium, high, mapAlpha;
        std::string collectTitle, anim, collect, gold, uniques, everything;
        std::string other, follow, autosave, explore, fsaa, violence;
        std::string multiplayer, dataFlow, fast, slow;
    };

    GameOptions g_game;
    ControllerOptions g_pad, g_padOriginal;
    Labels g_labels;
    int g_tab = 0;              // 0 game, 1 controller
    int g_selectTab = -1;       // switch to this tab next frame (LB / RB)
    bool g_first = true;

    // Waiting for a button to bind: until the buttons are up, then the first button pressed, with a second one
    // pressed while it is held making the first its modifier.
    struct Capture
    {
        int action = -1;
        bool waitRelease = true;
        uint32_t first = 0;
        DWORD started = 0;
    };
    Capture g_capture;
    bool g_navCooldown = false;     // gamepad navigation comes back once all buttons are up
    std::string g_note;             // e.g. what a new binding took over

    GameOptions readGame(void* w)
    {
        GameOptions o;
        o.sfx = readSlider(w, Options::sfxVolume);
        o.voice = readSlider(w, Options::voiceVolume);
        o.music = readSlider(w, Options::musicVolume);
        o.mapAlpha = readSlider(w, Options::mapAlpha);
        o.detail = radio(w, Options::detail, 3);
        o.pickupAuto = radio(w, Options::pickupAuto, 3);
        o.soundQuality = radio(w, Options::soundQuality, 3);
        o.netSlow = checked(control(w, Options::netSlow));
        o.pickupAnim = checked(control(w, Options::pickupAnim));
        o.autoTrack = checked(control(w, Options::autoTrack));
        o.violence = checked(control(w, Options::violence));
        o.violenceShown = shown(control(w, Options::violence));
        o.sound = checked(control(w, Options::sound));
        o.exploreMap = checked(control(w, Options::exploreMap));
        o.autosave = checked(control(w, Options::autosave));
        o.fsaa = checked(control(w, Options::fsaa));
        return o;
    }

    void writeGame(void* w, const GameOptions& o)
    {
        writeSlider(w, Options::sfxVolume, o.sfx);
        writeSlider(w, Options::voiceVolume, o.voice);
        writeSlider(w, Options::musicVolume, o.music);
        writeSlider(w, Options::mapAlpha, o.mapAlpha);
        setRadio(w, Options::detail, 3, o.detail);
        setRadio(w, Options::pickupAuto, 3, o.pickupAuto);
        setRadio(w, Options::soundQuality, 3, o.soundQuality);
        setChecked(control(w, Options::netFast), !o.netSlow);
        setChecked(control(w, Options::netSlow), o.netSlow);
        setChecked(control(w, Options::pickupAnim), o.pickupAnim);
        setChecked(control(w, Options::autoTrack), o.autoTrack);
        if (o.violenceShown)
        {
            setChecked(control(w, Options::violence), o.violence);
        }
        setChecked(control(w, Options::sound), o.sound);
        setChecked(control(w, Options::exploreMap), o.exploreMap);
        setChecked(control(w, Options::autosave), o.autosave);
        setChecked(control(w, Options::fsaa), o.fsaa);
    }

    ControllerOptions readController()
    {
        ControllerOptions o{g_config.controllerDeadzone, g_config.controllerCursorSpeed, g_config.controllerMoveRadius,
            g_config.controllerAimRange, g_config.controllerAimCone, g_config.controllerArtClick, g_config.controllerWalk,
            g_config.controllerPrompts, {}};
        for (int i = 0; i < Bindings::actionCount; ++i)
        {
            o.bindings[i] = Bindings::get(static_cast<Action>(i));
        }
        return o;
    }

    ControllerOptions defaultController()
    {
        const Config d;
        ControllerOptions o{d.controllerDeadzone, d.controllerCursorSpeed, d.controllerMoveRadius, d.controllerAimRange,
            d.controllerAimCone, d.controllerArtClick, d.controllerWalk, d.controllerPrompts, {}};
        for (int i = 0; i < Bindings::actionCount; ++i)
        {
            o.bindings[i] = Bindings::info(static_cast<Action>(i)).defaults;
        }
        return o;
    }

    // Applies the controller settings and stores them in SacredBild.ini.
    void saveController(const ControllerOptions& o)
    {
        g_config.controllerDeadzone = o.deadzone;
        g_config.controllerCursorSpeed = o.cursorSpeed;
        g_config.controllerMoveRadius = o.moveRadius;
        g_config.controllerAimRange = o.aimRange;
        g_config.controllerAimCone = o.aimCone;
        g_config.controllerArtClick = o.artClick;
        g_config.controllerWalk = o.walk;
        g_config.controllerPrompts = o.prompts;
        for (int i = 0; i < Bindings::actionCount; ++i)
        {
            Bindings::set(static_cast<Action>(i), o.bindings[i]);
        }
        const wchar_t* ini = g_config.iniPath.c_str();
        const auto put = [&](const wchar_t* key, int value) {
            return WritePrivateProfileStringW(L"Controller", key, std::to_wstring(value).c_str(), ini) != 0;
        };
        const bool ok = put(L"Deadzone", o.deadzone) & put(L"CursorSpeed", o.cursorSpeed) &
            put(L"MoveRadius", o.moveRadius) & put(L"AimRange", o.aimRange) & put(L"AimCone", o.aimCone) &
            put(L"ArtClick", o.artClick) & put(L"Walk", o.walk) & put(L"Prompts", o.prompts) &
            Bindings::save(g_config.iniPath);
        LOG("Controller: settings and bindings {} SacredBild.ini", ok ? "saved to" : "could not all be written to");
    }

    Labels readLabels()
    {
        Labels l;
        l.volume = label("UI_CFG_SOUND_VOLUME", "Sound Volume");
        l.sfx = label("UI_CFG_SFX", "SFX");
        l.speech = label("UI_CFG_SPEECH", "Speech");
        l.music = label("UI_CFG_SOUND", "Music");
        l.soundOptions = label("UI_CFG_SOUND_QUALITY_TITLE", "Sound Options");
        l.sound = label("UI_CFG_SOUNDPLAYBACK", "Sound");
        l.quality = label("UI_CFG_SOUND_QUALITY", "Sound Quality");
        l.qualityLow = label("UI_BTN_SNDQ_LOW", "2D - Low effects");
        l.qualityMid = label("UI_BTN_SNDQ_MID", "2D - All effects");
        l.qualityHigh = label("UI_BTN_SNDQ_HIGH", "3D - All effects");
        l.graphics = label("UI_CFG_GFX_SETTINGS", "Graphic Settings");
        l.detail = label("UI_CFG_GFX_DETAIL", "Graphic Details");
        l.low = label("UI_CFG_LOW", "Low");
        l.medium = label("UI_CFG_MEDIUM", "Medium");
        l.high = label("UI_CFG_HIGH", "High");
        l.mapAlpha = label("UI_CFG_MAPTRANS", "Map Transparency");
        l.collectTitle = label("UI_CFG_AUTOPICKUP", "Auto-collect");
        l.anim = label("UI_CFG_ANIM", "Atmospheric Animation");
        l.collect = label("UI_CFG_COLLECT", "Collect");
        l.gold = label("UI_CFG_AUTO_GOLD", "Gold");
        l.uniques = label("UI_CFG_AUTO_UNIQUE", "Gold / Uniques");
        l.everything = label("UI_CFG_AUTO_ALL", "Everything");
        l.other = label("UI_CFG_VARIOUS", "Other Options");
        l.follow = label("UI_CFG_AUTOFOLLOW", "Mouse: Follow opponents");
        l.autosave = label("UI_CFG_AUTOSAVE", "Autosave");
        l.explore = label("UI_CFG_EXPLORE", "Mini-Map Fog of War");
        l.fsaa = label("UI_CFG_FSAA_FILTER", "FSAA Filter");
        l.violence = label("UI_CFG_VIOLENCE", "Display of violence");
        l.multiplayer = label("UI_CFG_WAN", "Multiplayer Options");
        l.dataFlow = label("UI_CFG_LANSPEED", "Data flow");
        l.fast = label("UI_CFG_AB_DSL", "For DSL / Cable / LAN");
        l.slow = label("UI_CFG_AB_ISDN", "For MODEM / ISDN");
        return l;
    }

    // ---- Drawing ----

    void heading(const std::string& text)
    {
        ImGui::Spacing();
        ImGui::SeparatorText(text.c_str());
    }

    void sliderRow(const std::string& text, Slider& s, const char* id)
    {
        if (s.max <= 0)
        {
            return;
        }
        ImGui::TextUnformatted(text.c_str());
        ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.4f);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderInt(id, &s.value, 0, s.max);
    }

    void radioRow(const std::string& text, int& value, std::initializer_list<const std::string*> choices, const char* id)
    {
        ImGui::TextUnformatted(text.c_str());
        ImGui::PushID(id);
        int i = 0;
        for (const std::string* choice : choices)
        {
            ImGui::SameLine(i == 0 ? ImGui::GetContentRegionAvail().x * 0.4f : 0.0f);
            ImGui::PushID(i);
            ImGui::RadioButton(choice->c_str(), &value, i);
            ImGui::PopID();
            ++i;
        }
        ImGui::PopID();
    }

    void drawGame()
    {
        GameOptions& o = g_game;
        const Labels& l = g_labels;
        heading(l.volume);
        sliderRow(l.sfx, o.sfx, "##sfx");
        sliderRow(l.speech, o.voice, "##voice");
        sliderRow(l.music, o.music, "##music");
        heading(l.soundOptions);
        ImGui::Checkbox((l.sound + "##sound").c_str(), &o.sound);
        radioRow(l.quality, o.soundQuality, {&l.qualityLow, &l.qualityMid, &l.qualityHigh}, "quality");
        heading(l.graphics);
        radioRow(l.detail, o.detail, {&l.low, &l.medium, &l.high}, "detail");
        sliderRow(l.mapAlpha, o.mapAlpha, "##mapAlpha");
        heading(l.collectTitle);
        ImGui::Checkbox((l.anim + "##anim").c_str(), &o.pickupAnim);
        radioRow(l.collect, o.pickupAuto, {&l.gold, &l.uniques, &l.everything}, "collect");
        heading(l.other);
        ImGui::Checkbox((l.follow + "##follow").c_str(), &o.autoTrack);
        ImGui::Checkbox((l.autosave + "##autosave").c_str(), &o.autosave);
        ImGui::Checkbox((l.explore + "##explore").c_str(), &o.exploreMap);
        ImGui::Checkbox((l.fsaa + "##fsaa").c_str(), &o.fsaa);
        if (o.violenceShown)
        {
            ImGui::Checkbox((l.violence + "##violence").c_str(), &o.violence);
        }
        heading(l.multiplayer);
        int slow = o.netSlow ? 1 : 0;
        radioRow(l.dataFlow, slow, {&l.fast, &l.slow}, "net");
        o.netSlow = slow == 1;
    }

    uint32_t lowestBit(uint32_t v)
    {
        return v & (~v + 1);
    }

    void finishCapture(Binding b)
    {
        const int action = g_capture.action;
        g_capture = {};
        g_navCooldown = true;
        g_note.clear();
        for (int i = 0; i < Bindings::actionCount; ++i)
        {
            if (i != action && b.button && g_pad.bindings[i] == b)
            {
                g_pad.bindings[i] = {};
                g_note = std::string(Bindings::format(b)) + " no longer does: " + Bindings::info(static_cast<Action>(i)).label;
            }
        }
        g_pad.bindings[action] = b;
    }

    void updateCapture()
    {
        const uint32_t held = Gamepad::state().buttons;
        if (g_navCooldown && g_capture.action < 0)
        {
            if (held == 0)
            {
                g_navCooldown = false;
                Overlay::setGamepadNavigation(true);
            }
            return;
        }
        if (g_capture.action < 0)
        {
            return;
        }
        if (GetTickCount() - g_capture.started > kCaptureTimeoutMs)
        {
            g_capture = {};
            g_navCooldown = true;
            return;
        }
        if (g_capture.waitRelease)
        {
            g_capture.waitRelease = held != 0;
            return;
        }
        if (!g_capture.first)
        {
            g_capture.first = lowestBit(Gamepad::pressed());
            return;
        }
        if (const uint32_t second = Gamepad::pressed() & ~g_capture.first)
        {
            finishCapture({lowestBit(second), g_capture.first});
        }
        else if (!(held & g_capture.first))
        {
            finishCapture({g_capture.first, 0});
        }
    }

    void drawController()
    {
        ControllerOptions& o = g_pad;
        heading("Controller");
        const auto slider = [](const char* text, int& v, int lo, int hi, const char* format, const char* tip) {
            ImGui::TextUnformatted(text);
            ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.4f);
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::PushID(text);
            ImGui::SliderInt("##v", &v, lo, hi, format);
            ImGui::SetItemTooltip("%s", tip);
            ImGui::PopID();
        };
        slider("Stick deadzone", o.deadzone, 0, 60, "%d %%", "How far a stick moves before it counts.");
        slider("Cursor speed", o.cursorSpeed, 200, 3000, "%d", "Cursor speed in menus and windows at full deflection.");
        slider("Walk distance", o.moveRadius, 60, 400, "%d px", "How far ahead of the hero the left stick walks to.");
        slider("Aim range", o.aimRange, 100, 1200, "%d px", "Enemies within this distance are targeted.");
        slider("Aim cone", o.aimCone, 20, 180, "%d deg", "Enemies within this angle around the stick come first.");
        ImGui::Checkbox("Combat art slots also cast at the target", &o.artClick);
        ImGui::SetItemTooltip("On: the slot button selects the combat art and right-clicks the target.\nOff: it only selects it.");
        ImGui::Checkbox("Walk when the stick is pushed less than halfway", &o.walk);
        ImGui::Checkbox("Show button prompts", &o.prompts);
        ImGui::SetItemTooltip("Button icons beside the control a button presses, and the bindings at the HUD's slots\n"
            "while Show names is held or the help screen (H) is up.");

        heading("Buttons");
        ImGui::TextDisabled("A: change   X: clear   hold a button and press another for a two-button binding");
        if (!g_note.empty())
        {
            ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f), "%s", g_note.c_str());
        }
        uint32_t modifiers = 0;
        for (const Binding& b : o.bindings)
        {
            if (b.button)
            {
                modifiers |= b.modifier;
            }
        }
        const char* group = "";
        if (ImGui::BeginTable("bindings", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            for (int i = 0; i < Bindings::actionCount; ++i)
            {
                const Bindings::Info& info = Bindings::info(static_cast<Action>(i));
                if (std::string_view(info.group) != group)
                {
                    group = info.group;
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextDisabled("%s", group);
                }
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(info.label);
                ImGui::TableSetColumnIndex(1);
                ImGui::PushID(i);
                const Binding& b = o.bindings[i];
                std::string text = g_capture.action == i ? "press a button..." : Bindings::format(b);
                if (text.empty())
                {
                    text = "-";
                }
                if (ImGui::Button((text + "##bind").c_str(), ImVec2(-1.0f, 0.0f)) && g_capture.action < 0)
                {
                    g_capture = {i, true, 0, GetTickCount()};
                    Overlay::setGamepadNavigation(false);
                }
                if (ImGui::IsItemFocused() && g_capture.action < 0 && (Gamepad::pressed() & Gamepad::X))
                {
                    o.bindings[i] = {};
                }
                if (b.button && !b.modifier && (b.button & modifiers))
                {
                    ImGui::SetItemTooltip("%s is held for two-button bindings, so this one never fires.", Bindings::format(b).c_str());
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (ImGui::Button("Restore controller defaults"))
        {
            o = defaultController();
            g_note.clear();
        }
    }

    void close(bool accept)
    {
        void* window = g_window.load();
        g_screen = false;
        g_capture = {};
        Overlay::setGamepadNavigation(true);
        if (!window)
        {
            return;
        }
        if (accept)
        {
            writeGame(window, g_game);
            if (!(g_pad == g_padOriginal))
            {
                saveController(g_pad);
            }
        }
        g_hideUntil = GetTickCount() + kCloseGraceMs;
        press(window, accept ? Options::ok : Options::cancel);
    }

    bool draw()
    {
        void* window = g_window.load();
        if (!window || !g_screen.load())
        {
            return false;
        }
        updateCapture();
        ImGuiIO& io = ImGui::GetIO();
        const bool capturing = g_capture.action >= 0;

        // LB / RB: tabs; Start: accept; B: cancel (not while binding or editing).
        const uint32_t pressed = capturing || g_navCooldown ? 0 : Gamepad::pressed();
        if (pressed & (Gamepad::LB | Gamepad::RB))
        {
            g_selectTab = g_tab == 0 ? 1 : 0;
        }
        bool accept = (pressed & Gamepad::Start) != 0;
        bool cancel = (pressed & Gamepad::B) && !ImGui::IsAnyItemActive();
        if (!capturing && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            cancel = true;
        }

        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 200));
        const float s = Overlay::scale();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(760.0f * s, 640.0f * s), ImGuiCond_Always);
        if (g_first)
        {
            ImGui::SetNextWindowFocus();
        }
        ImGui::Begin("Options##SacredBild", nullptr,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
        if (ImGui::BeginTabBar("tabs"))
        {
            const char* names[] = {"Game", "Controller"};
            for (int t = 0; t < 2; ++t)
            {
                const ImGuiTabItemFlags flags = g_selectTab == t ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem(names[t], nullptr, flags))
                {
                    g_tab = t;
                    ImGui::BeginChild("page", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() * 1.6f));
                    t == 0 ? drawGame() : drawController();
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }
        g_selectTab = -1;
        g_first = false;
        ImGui::Separator();
        const float buttonWidth = 160.0f * s;
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 2.0f * buttonWidth - ImGui::GetStyle().ItemSpacing.x - ImGui::GetStyle().WindowPadding.x);
        accept |= ImGui::Button("Accept", ImVec2(buttonWidth, 0.0f));
        ImGui::SetItemTooltip("Start");
        ImGui::SameLine();
        cancel |= ImGui::Button("Cancel", ImVec2(buttonWidth, 0.0f));
        ImGui::SetItemTooltip("B");
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetFrameHeightWithSpacing());
        ImGui::TextDisabled("LB / RB: page   Start: accept   B: cancel");
        ImGui::End();

        if (capturing)
        {
            accept = cancel = false;
        }
        if (accept || cancel)
        {
            close(accept);
            return false;
        }
        return true;
    }

    void open(void* window)
    {
        g_window = window;
        g_game = readGame(window);
        g_pad = g_padOriginal = readController();
        g_labels = readLabels();
        g_tab = 0;
        g_selectTab = -1;
        g_first = true;
        g_capture = {};
        g_navCooldown = true;       // the button that opened the options is still down
        g_note.clear();
        g_openTick = GetTickCount();
        g_screen = true;
        Overlay::setGamepadNavigation(false);
        Overlay::open(&draw);
        LOG("Controller: options as SacredBild's screen");
    }

    bool hidden(void* self)
    {
        if (self != g_window.load())
        {
            return false;
        }
        if (g_screen.load())
        {
            if (Overlay::isOpen() || GetTickCount() - g_openTick.load() < 500)
            {
                return true;
            }
            g_screen = false;   // the overlay couldn't show the screen: the game's own window it is
        }
        else if (static_cast<int>(GetTickCount() - g_hideUntil.load()) < 0)
        {
            return true;
        }
        g_window = nullptr;     // the click didn't close it: the game's own window takes over
        return false;
    }

    void __fastcall hookShow(void* self, void* edx, int show)
    {
        g_origShow(self, edx, show);
        if (show & 0xFF)
        {
            if (InputMode::controller() && Overlay::available() && !Overlay::isOpen())
            {
                open(self);
            }
        }
        else if (self == g_window.load())
        {
            g_screen = false;
            g_window = nullptr;
        }
    }

    void __fastcall hookRender(void* self, void* edx, void* device)
    {
        if (!hidden(self))
        {
            g_origRender(self, edx, device);
        }
    }

    void __fastcall hookRender2(void* self, void* edx, void* device, int a, int b)
    {
        if (!hidden(self))
        {
            g_origRender2(self, edx, device, a, b);
        }
    }
}

void OptionsScreen::install()
{
    if (!g_config.controller || !g_config.ddrawD3D9)
    {
        return;
    }
    auto** table = reinterpret_cast<void**>(Addr::cUI_Options_vtable);
    g_origShow = reinterpret_cast<ShowFn>(table[UiWindowSlot::show / 4]);
    g_origRender = reinterpret_cast<RenderFn>(table[UiWindowSlot::render / 4]);
    g_origRender2 = reinterpret_cast<Render2Fn>(table[UiWindowSlot::render2 / 4]);
    const bool ok = Patch::value(reinterpret_cast<uintptr_t>(&table[UiWindowSlot::show / 4]), reinterpret_cast<void*>(&hookShow)) &&
        Patch::value(reinterpret_cast<uintptr_t>(&table[UiWindowSlot::render / 4]), reinterpret_cast<void*>(&hookRender)) &&
        Patch::value(reinterpret_cast<uintptr_t>(&table[UiWindowSlot::render2 / 4]), reinterpret_cast<void*>(&hookRender2));
    LOG("Controller: options window {}", ok ? "wrapped" : "could not be wrapped");
}
