#pragma once
// Key remapping for the game's own actions (gear.input): a key or mouse button the player chose is turned into the key
// the game reacts to, before the game reads its messages. The game's keys are fixed (I opens the inventory, 6 selects
// combat art 1 ...), so a rule says "this input is that game key"; a rule without target swallows an input (the
// original key of an action the player moved elsewhere).
//
// Works on the messages the game reads (PeekMessage / GetMessage, hooked in keyblock.cpp), so it comes before any
// handler of the game and the game never sees the original. Only while the game itself has the keyboard: in game, no
// text entry (chat line, edit field), no message box or escape menu, no Gear screen that keeps the keyboard.

#include <string>
#include <vector>

#include <windows.h>

namespace Input
{
    // Inputs are VK_ codes; the mouse buttons are VK_MBUTTON, VK_XBUTTON1 and VK_XBUTTON2.
    struct Rule
    {
        std::string id;         // reported with the events ("": none)
        int from = 0;           // the key or mouse button
        bool ctrl = false;      // modifiers that must be held (and no others), unless anyModifiers
        bool shift = false;
        bool alt = false;
        bool anyModifiers = false;
        int to = 0;             // the game's key; 0: swallow the input
        // The game reads some keys by polling (Alt shows names, Ctrl stands still, numpad +/- zoom), not through messages.
        // poll with `to`: the game's poll of key `to` sees `from` (the messages of `from` are swallowed); poll without `to`:
        // the game's poll of key `from` sees it released.
        bool poll = false;
    };

    // Replaces all rules (any thread).
    void setRules(std::vector<Rule> rules);

    struct Event
    {
        std::string id;
        bool down;
        bool repeat;            // a held key's auto repeat
    };
    // The presses and releases of inputs taken by rules with an id since the last call (any thread).
    std::vector<Event> takeEvents();

    // Whether remapping applies now: in game, nobody types, no message box, escape menu, savegame, options or character screen.
    bool context();
    // Whether the chat line is open (keys go to text).
    bool typing();

    // keyblock.cpp's hooks, for every message the game fetches: may turn it into another (a mouse button into a key)
    // or into WM_NULL. `consume`: the message is being taken off the queue.
    void filter(MSG& msg, bool consume);
    // keyblock.cpp's hooks of the game's key state queries: what the game should see for `vk` (`real`: the key's state).
    bool polled(int vk, bool real);
    // While the message the game handles now was made by a rule, the modifiers look released to its key queries.
    bool hidingModifiers();
}
