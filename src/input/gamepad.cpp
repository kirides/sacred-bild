#include "input/gamepad.h"
#include "log.h"

#include <windows.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace
{
    // A pad as the SDL thread last read it: buttons without the triggers, sticks y up.
    struct Reading
    {
        SDL_JoystickID id = 0;
        Gamepad::Style style = Gamepad::Style::Xbox;
        uint32_t buttons = 0;
        int16_t lx = 0, ly = 0, rx = 0, ry = 0;
        int16_t lt = 0, rt = 0;     // 0..32767

        bool operator==(const Reading&) const = default;
    };

    std::once_flag g_start;
    HANDLE g_wake = nullptr;        // a frame wants readings
    std::mutex g_lock;
    std::vector<Reading> g_readings;
    // Rumble requests for the SDL thread: a new sequence number, its strength (0..0xFFFF) and length.
    std::atomic<uint32_t> g_rumbleRequest{0}, g_rumbleStrength{0}, g_rumbleMs{0};

    // The presenting thread's.
    std::vector<Reading> g_previous;
    SDL_JoystickID g_current = 0;   // the pad in use
    Gamepad::State g_state;
    uint32_t g_pressed = 0;
    uint32_t g_released = 0;
    bool g_active = false;
    float g_deadzone = 0.24f;

    constexpr DWORD kIdleWaitMs = 100;      // the SDL thread's pace without frames (hot-plugging)
    constexpr float kTriggerThreshold = 1.0f / 3.0f;

    struct Name
    {
        uint32_t button;
        const char* name;
    };
    constexpr Name kNames[] = {
        {Gamepad::A, "A"}, {Gamepad::B, "B"}, {Gamepad::X, "X"}, {Gamepad::Y, "Y"},
        {Gamepad::LB, "LB"}, {Gamepad::RB, "RB"}, {Gamepad::LT, "LT"}, {Gamepad::RT, "RT"},
        {Gamepad::Back, "Back"}, {Gamepad::Start, "Start"}, {Gamepad::L3, "L3"}, {Gamepad::R3, "R3"},
        {Gamepad::Up, "Up"}, {Gamepad::Down, "Down"}, {Gamepad::Left, "Left"}, {Gamepad::Right, "Right"},
    };

    // SDL's buttons by position (south = A on an Xbox pad, cross on a PlayStation one).
    constexpr std::pair<SDL_GamepadButton, uint32_t> kButtons[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, Gamepad::A}, {SDL_GAMEPAD_BUTTON_EAST, Gamepad::B},
        {SDL_GAMEPAD_BUTTON_WEST, Gamepad::X}, {SDL_GAMEPAD_BUTTON_NORTH, Gamepad::Y},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, Gamepad::LB}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, Gamepad::RB},
        {SDL_GAMEPAD_BUTTON_BACK, Gamepad::Back}, {SDL_GAMEPAD_BUTTON_START, Gamepad::Start},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, Gamepad::L3}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, Gamepad::R3},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, Gamepad::Up}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, Gamepad::Down},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, Gamepad::Left}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, Gamepad::Right},
    };

    int16_t flip(int16_t v)
    {
        return static_cast<int16_t>(std::clamp(-static_cast<int>(v), -32768, 32767));
    }

    Gamepad::Style styleOf(SDL_Gamepad* pad)
    {
        switch (SDL_GetGamepadType(pad))
        {
        case SDL_GAMEPAD_TYPE_PS3:
        case SDL_GAMEPAD_TYPE_PS4:
        case SDL_GAMEPAD_TYPE_PS5:
            return Gamepad::Style::PlayStation;
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
            return Gamepad::Style::Switch;
        default:
            return Gamepad::Style::Xbox;
        }
    }

    Reading read(SDL_Gamepad* pad)
    {
        Reading r;
        r.id = SDL_GetGamepadID(pad);
        r.style = styleOf(pad);
        for (const auto& [button, bit] : kButtons)
        {
            if (SDL_GetGamepadButton(pad, button))
            {
                r.buttons |= bit;
            }
        }
        r.lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
        r.ly = flip(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY));
        r.rx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX);
        r.ry = flip(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY));
        r.lt = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
        r.rt = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        return r;
    }

    // SDL lives on this thread: initialized here, and asked for the pads' state once per frame (or now and then
    // without frames, for hot-plugging).
    DWORD WINAPI run(void*)
    {
        // There is no SDL window to have the focus; Controller ignores the pad while the game is in the background.
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        // Under Wine, SDL's defaults (XInput, which Wine feeds from the host's SDL). On Windows also raw input: it
        // reads an Xbox pad's HID interface directly, where drivers in front of the pad (Steam's Xbox driver with
        // Steam closed, controller emulators) can hide it from XInput.
        const bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
        if (!wine)
        {
            SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "1");
        }
        if (!SDL_Init(SDL_INIT_GAMEPAD))
        {
            LOG("Controller: SDL could not start ({}); no controllers", SDL_GetError());
            return 0;
        }
        const int v = SDL_GetVersion();
        LOG("Controller: SDL {}.{}.{}{}", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v),
            wine ? " (Wine: XInput)" : " (raw input, XInput, HID)");
        std::vector<SDL_Gamepad*> pads;
        for (;;)
        {
            WaitForSingleObject(g_wake, kIdleWaitMs);
            SDL_Event e;
            while (SDL_PollEvent(&e))
            {
                if (e.type == SDL_EVENT_GAMEPAD_ADDED)
                {
                    if (SDL_Gamepad* pad = SDL_OpenGamepad(e.gdevice.which))
                    {
                        pads.push_back(pad);
                        LOG("Controller: {} connected ({}, {:04x}:{:04x})", SDL_GetGamepadName(pad) ? SDL_GetGamepadName(pad) : "pad",
                            SDL_GetGamepadStringForType(SDL_GetGamepadType(pad)), SDL_GetGamepadVendor(pad), SDL_GetGamepadProduct(pad));
                        char* mapping = SDL_GetGamepadMapping(pad);
                        LOG("Controller: mapping {}; device {}", mapping ? mapping : "(none)",
                            SDL_GetGamepadPath(pad) ? SDL_GetGamepadPath(pad) : "(no path)");
                        SDL_free(mapping);
                    }
                }
                else if (e.type == SDL_EVENT_GAMEPAD_REMOVED)
                {
                    const auto it = std::find_if(pads.begin(), pads.end(),
                        [&](SDL_Gamepad* p) { return SDL_GetGamepadID(p) == e.gdevice.which; });
                    if (it != pads.end())
                    {
                        LOG("Controller: {} disconnected", SDL_GetGamepadName(*it) ? SDL_GetGamepadName(*it) : "pad");
                        SDL_CloseGamepad(*it);
                        pads.erase(it);
                    }
                }
            }
            static uint32_t rumbled = 0;
            if (const uint32_t request = g_rumbleRequest.load(); request != rumbled)
            {
                rumbled = request;
                const auto low = static_cast<Uint16>(g_rumbleStrength.load());
                for (SDL_Gamepad* pad : pads)
                {
                    static int failures = 0;
                    if (!SDL_RumbleGamepad(pad, low, static_cast<Uint16>(low * 3 / 5), g_rumbleMs.load()) &&
                        failures++ < 3)
                    {
                        LOG("Controller: no rumble ({})", SDL_GetError());
                    }
                }
            }
            std::vector<Reading> readings;
            readings.reserve(pads.size());
            for (SDL_Gamepad* pad : pads)
            {
                readings.push_back(read(pad));
            }
            std::scoped_lock lock(g_lock);
            g_readings.swap(readings);
        }
    }

    // A stick with a radial deadzone: inside it 0, beyond it rescaled so the edge of the deadzone is 0.
    void stick(int16_t rawX, int16_t rawY, float& x, float& y)
    {
        const float fx = std::max(-1.0f, rawX / 32767.0f);
        const float fy = std::max(-1.0f, rawY / 32767.0f);
        const float length = std::sqrt(fx * fx + fy * fy);
        if (length <= g_deadzone)
        {
            x = y = 0.0f;
            return;
        }
        const float scaled = std::min(1.0f, (length - g_deadzone) / (1.0f - g_deadzone));
        x = fx / length * scaled;
        y = fy / length * scaled;
    }

    Gamepad::State convert(const Reading& r)
    {
        Gamepad::State s;
        s.connected = true;
        s.style = r.style;
        s.buttons = r.buttons;
        s.lt = std::clamp(r.lt / 32767.0f, 0.0f, 1.0f);
        s.rt = std::clamp(r.rt / 32767.0f, 0.0f, 1.0f);
        if (s.lt > kTriggerThreshold)
        {
            s.buttons |= Gamepad::LT;
        }
        if (s.rt > kTriggerThreshold)
        {
            s.buttons |= Gamepad::RT;
        }
        stick(r.lx, r.ly, s.lx, s.ly);
        stick(r.rx, r.ry, s.rx, s.ry);
        return s;
    }

    bool hasInput(const Gamepad::State& s)
    {
        return s.buttons || s.lx || s.ly || s.rx || s.ry;
    }
}

void Gamepad::poll()
{
    std::call_once(g_start, [] {
        g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (HANDLE thread = g_wake ? CreateThread(nullptr, 0, &run, nullptr, 0, nullptr) : nullptr)
        {
            CloseHandle(thread);
        }
        else
        {
            LOG("Controller: no thread for the controllers");
        }
    });
    std::vector<Reading> readings;
    {
        std::scoped_lock lock(g_lock);
        readings = g_readings;
    }
    if (g_wake)
    {
        SetEvent(g_wake);   // fresh readings for the next frame
    }

    // The pad in use stays until another one gives input (or it goes away).
    const auto find = [](const std::vector<Reading>& list, SDL_JoystickID id) {
        return std::find_if(list.begin(), list.end(), [&](const Reading& r) { return r.id == id; });
    };
    if (find(readings, g_current) == readings.end())
    {
        g_current = readings.empty() ? 0 : readings.front().id;
    }
    for (const Reading& r : readings)
    {
        const auto before = find(g_previous, r.id);
        if (r.id != g_current && before != g_previous.end() && !(*before == r) && hasInput(convert(r)))
        {
            g_current = r.id;
        }
    }
    const uint32_t previous = g_state.buttons;
    const auto current = find(readings, g_current);
    g_state = current != readings.end() ? convert(*current) : State{};
    g_previous = std::move(readings);
    g_pressed = g_state.buttons & ~previous;
    g_released = previous & ~g_state.buttons;
    g_active = hasInput(g_state);
}

const Gamepad::State& Gamepad::state()
{
    return g_state;
}

uint32_t Gamepad::pressed()
{
    return g_pressed;
}

uint32_t Gamepad::released()
{
    return g_released;
}

bool Gamepad::active()
{
    return g_active;
}

void Gamepad::rumble(float strength, uint32_t milliseconds)
{
    g_rumbleStrength = static_cast<uint32_t>(std::clamp(strength, 0.0f, 1.0f) * 0xFFFF);
    g_rumbleMs = milliseconds;
    g_rumbleRequest.fetch_add(1);
    if (g_wake)
    {
        SetEvent(g_wake);
    }
}

void Gamepad::setDeadzone(float deadzone)
{
    g_deadzone = std::clamp(deadzone, 0.0f, 0.9f);
}

const char* Gamepad::name(uint32_t button)
{
    for (const Name& n : kNames)
    {
        if (n.button == button)
        {
            return n.name;
        }
    }
    return nullptr;
}

uint32_t Gamepad::fromName(std::string_view name)
{
    for (const Name& n : kNames)
    {
        if (name.size() == std::strlen(n.name) && _strnicmp(name.data(), n.name, name.size()) == 0)
        {
            return n.button;
        }
    }
    return 0;
}
