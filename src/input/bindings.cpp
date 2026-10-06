#include "input/bindings.h"
#include "input/gamepad.h"
#include "log.h"

#include <windows.h>
#include <array>
#include <cctype>

namespace
{
    using Bindings::Action;
    using Bindings::Binding;
    using Bindings::Info;
    using namespace Gamepad;
    using Bindings::Press;

    constexpr Binding plain(uint32_t button) { return {button, 0}; }
    constexpr Binding layer(uint32_t button) { return {button, LT}; }
    constexpr Binding none{};

    // In the order of Action.
    constexpr Info kInfo[] = {
        {"Primary", "Attack / interact", "Combat", 0, Press::Tap, plain(A)},
        {"Secondary", "Combat art (right click)", "Combat", 0, Press::Tap, plain(X)},
        {"Art1", "Combat art slot 1", "Combat", '6', Press::Tap, plain(Y)},
        {"Art2", "Combat art slot 2", "Combat", '7', Press::Tap, plain(B)},
        {"Art3", "Combat art slot 3", "Combat", '8', Press::Tap, plain(RB)},
        {"Art4", "Combat art slot 4", "Combat", '9', Press::Tap, plain(RT)},
        {"Art5", "Combat art slot 5", "Combat", '0', Press::Tap, layer(A)},
        {"Weapon1", "Weapon slot 1", "Weapons", '1', Press::Tap, layer(X)},
        {"Weapon2", "Weapon slot 2", "Weapons", '2', Press::Tap, layer(Y)},
        {"Weapon3", "Weapon slot 3", "Weapons", '3', Press::Tap, layer(B)},
        {"Weapon4", "Weapon slot 4", "Weapons", '4', Press::Tap, layer(RB)},
        {"Weapon5", "Weapon slot 5", "Weapons", '5', Press::Tap, layer(RT)},
        {"Heal", "Healing potion", "Potions", VK_SPACE, Press::Tap, plain(Up)},
        {"UndeadDeath", "Potion of Undead Death", "Potions", 'Q', Press::Tap, layer(Up)},
        {"Mentor", "Potion of the Mentor", "Potions", 'W', Press::Tap, layer(Down)},
        {"Antidote", "Viper's Antidote", "Potions", 'E', Press::Tap, plain(Left)},
        {"Concentration", "Potion of Concentration", "Potions", 'R', Press::Tap, plain(Right)},
        {"HealHirelings", "Heal hirelings", "Potions", 'B', Press::Tap, plain(Down)},
        {"StandStill", "Stand still (Ctrl)", "Combat", VK_CONTROL, Press::Hold, plain(LB)},
        {"ShowItems", "Show names (Alt)", "Game", VK_MENU, Press::Hold, plain(L3)},
        {"PickUpAll", "Collect all visible objects", "Game", 'A', Press::Tap, layer(L3)},
        {"Inventory", "Inventory", "Windows", 'I', Press::Tap, plain(Back)},
        {"CombatArts", "Combat arts", "Windows", 'F', Press::Tap, none},
        {"Combos", "Combo menu", "Windows", 'C', Press::Tap, none},
        {"LogBook", "Log book", "Windows", 'L', Press::Tap, none},
        {"WorldMap", "World map", "Windows", 'M', Press::Tap, layer(Back)},
        {"Minimap", "Overview map (Tab)", "Windows", VK_TAB, Press::Toggle, plain(R3)},
        {"Options", "Options", "Windows", 'O', Press::Tap, layer(Start)},
        {"SaveMenu", "Save menu", "Windows", 'S', Press::Tap, none},
        {"Escape", "Game menu / close (Esc)", "Windows", VK_ESCAPE, Press::Tap, plain(Start)},
        {"Help", "Help screen", "Windows", 'H', Press::Tap, none},
        {"Network", "Party information", "Windows", 'N', Press::Tap, none},
        {"QuickSave", "Quick save", "Game", VK_F9, Press::Tap, none},
        {"QuickLoad", "Quick load", "Game", VK_F8, Press::Tap, none},
        {"Pause", "Pause", "Game", 'P', Press::Tap, none},
        {"ZoomIn", "Zoom in", "Game", VK_ADD, Press::Hold, layer(Right)},
        {"ZoomOut", "Zoom out", "Game", VK_SUBTRACT, Press::Hold, layer(Left)},
        {"Cursor", "Cursor mode on / off", "Game", 0, Press::Tap, layer(R3)},
    };
    static_assert(std::size(kInfo) == Bindings::actionCount);

    std::array<Binding, Bindings::actionCount> g_bindings = [] {
        std::array<Binding, Bindings::actionCount> b{};
        for (int i = 0; i < Bindings::actionCount; ++i)
        {
            b[i] = kInfo[i].defaults;
        }
        return b;
    }();

    std::string_view trim(std::string_view s)
    {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        {
            s.remove_prefix(1);
        }
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        {
            s.remove_suffix(1);
        }
        return s;
    }
}

const Info& Bindings::info(Action action)
{
    return kInfo[static_cast<int>(action)];
}

Binding Bindings::get(Action action)
{
    return g_bindings[static_cast<int>(action)];
}

void Bindings::set(Action action, Binding binding)
{
    g_bindings[static_cast<int>(action)] = binding;
}

void Bindings::resetDefaults()
{
    for (int i = 0; i < actionCount; ++i)
    {
        g_bindings[i] = kInfo[i].defaults;
    }
}

std::string Bindings::format(Binding binding)
{
    const char* button = Gamepad::name(binding.button);
    if (!button)
    {
        return {};
    }
    const char* modifier = Gamepad::name(binding.modifier);
    return modifier ? std::string(modifier) + "+" + button : std::string(button);
}

bool Bindings::parse(std::string_view text, Binding& binding)
{
    text = trim(text);
    if (text.empty())
    {
        binding = {};
        return true;
    }
    const size_t plus = text.find('+');
    if (plus == std::string_view::npos)
    {
        const uint32_t button = Gamepad::fromName(text);
        binding = {button, 0};
        return button != 0;
    }
    const uint32_t modifier = Gamepad::fromName(trim(text.substr(0, plus)));
    const uint32_t button = Gamepad::fromName(trim(text.substr(plus + 1)));
    if (!modifier || !button || modifier == button)
    {
        return false;
    }
    binding = {button, modifier};
    return true;
}

uint32_t Bindings::modifiers()
{
    uint32_t mask = 0;
    for (const Binding& b : g_bindings)
    {
        if (b.button)
        {
            mask |= b.modifier;
        }
    }
    return mask;
}

void Bindings::load(const std::wstring& ini)
{
    int changed = 0;
    for (int i = 0; i < actionCount; ++i)
    {
        wchar_t key[64] = {};
        MultiByteToWideChar(CP_ACP, 0, kInfo[i].id, -1, key, static_cast<int>(std::size(key)));
        wchar_t value[64] = {};
        // \x01: the key is missing (keeps the default); empty: unbound.
        GetPrivateProfileStringW(L"Controller.Bindings", key, L"\x01", value, static_cast<DWORD>(std::size(value)), ini.c_str());
        if (value[0] == L'\x01')
        {
            continue;
        }
        char text[64] = {};
        WideCharToMultiByte(CP_ACP, 0, value, -1, text, static_cast<int>(std::size(text)), nullptr, nullptr);
        Binding b;
        if (parse(text, b))
        {
            changed += !(b == g_bindings[i]);
            g_bindings[i] = b;
        }
        else
        {
            LOG("Controller: [Controller.Bindings] {}={} is not a button (A B X Y LB RB LT RT Back Start L3 R3 Up "
                "Down Left Right, optionally Modifier+Button); kept {}", kInfo[i].id, text, format(g_bindings[i]));
        }
    }
    if (changed)
    {
        LOG("Controller: {} bindings from [Controller.Bindings]", changed);
    }
}

bool Bindings::save(const std::wstring& ini)
{
    bool ok = true;
    for (int i = 0; i < actionCount; ++i)
    {
        wchar_t key[64] = {};
        MultiByteToWideChar(CP_ACP, 0, kInfo[i].id, -1, key, static_cast<int>(std::size(key)));
        const std::string text = format(g_bindings[i]);
        const std::wstring value(text.begin(), text.end());
        ok &= WritePrivateProfileStringW(L"Controller.Bindings", key, value.c_str(), ini.c_str()) != 0;
    }
    return ok;
}

Bindings::Set Bindings::Resolver::update(uint32_t buttons)
{
    Set held;
    const uint32_t modifierMask = modifiers();
    m_consumed &= buttons;
    // Layered bindings first: their button is the binding's until released.
    for (int i = 0; i < actionCount; ++i)
    {
        const Binding& b = g_bindings[i];
        if (b.button && b.modifier && (buttons & b.button) && (buttons & b.modifier))
        {
            held[i] = true;
            m_consumed |= b.button;
        }
    }
    for (int i = 0; i < actionCount; ++i)
    {
        const Binding& b = g_bindings[i];
        if (b.button && !b.modifier && (buttons & b.button) && !(m_consumed & b.button) && !(modifierMask & b.button))
        {
            held[i] = true;
        }
    }
    return held;
}
