#pragma once
#include <bitset>
#include <cstdint>
#include <string>
#include <string_view>

// What the controller's buttons do in game: one binding per action, a button alone or held together with a
// modifier button (a second layer, as LT in Diablo 2 Resurrected). Most actions press one of the game's keys; the
// attack and combat art actions click at the target AimAssist picks (Controller). [Controller.Bindings] in
// SacredBild.ini, e.g. Art5=LT+A; the options screen edits them.
namespace Bindings
{
    enum class Action : uint8_t
    {
        Primary,        // left click at the nearest enemy, else at what is nearby (talk, open, pick up)
        Secondary,      // right click at the nearest enemy: the active combat art
        Art1, Art2, Art3, Art4, Art5,           // keys 6-0 select the combat art slot, then as Secondary
        Weapon1, Weapon2, Weapon3, Weapon4, Weapon5,    // keys 1-5
        Heal, UndeadDeath, Mentor, Antidote, Concentration, HealHirelings,  // Space Q W E R B
        StandStill,     // Ctrl held: attack without moving
        ShowItems,      // Alt held: names of the objects on the ground
        PickUpAll,      // A
        Inventory, CombatArts, Combos, LogBook, WorldMap, Minimap, Options, SaveMenu, Escape, Help, Network,
        QuickSave, QuickLoad, Pause, ZoomIn, ZoomOut,
        Cursor,         // switches the sticks to moving the cursor in game (and back)
        Count
    };
    constexpr int actionCount = static_cast<int>(Action::Count);
    using Set = std::bitset<actionCount>;

    struct Binding
    {
        uint32_t button = 0;    // Gamepad::Button, 0 = none
        uint32_t modifier = 0;  // a button held with it, 0 = none
        bool operator==(const Binding&) const = default;
    };

    // How the action's key is pressed.
    enum class Press : uint8_t
    {
        Tap,        // down and up again (like a quick key press)
        Hold,       // down as long as the button
        Toggle,     // down on one press, up on the next: the overview map (Tab) shows only while its key is down
    };

    struct Info
    {
        const char* id;         // ini key
        const char* label;
        const char* group;
        int vk;                 // the game's key for it, 0 = none
        Press press;
        Binding defaults;
    };
    const Info& info(Action action);

    Binding get(Action action);
    void set(Action action, Binding binding);
    void resetDefaults();

    // "LT+A", "" for none.
    std::string format(Binding binding);
    bool parse(std::string_view text, Binding& binding);

    // Buttons some binding uses as a modifier; they have no action of their own.
    uint32_t modifiers();

    void load(const std::wstring& ini);
    bool save(const std::wstring& ini);

    // The actions held with the pad's buttons. A button that went down together with a modifier belongs to that
    // binding until it is released, so letting go of the modifier first doesn't trigger its plain action.
    class Resolver
    {
    public:
        Set update(uint32_t buttons);
        void reset() { m_consumed = 0; }

    private:
        uint32_t m_consumed = 0;
    };
}
