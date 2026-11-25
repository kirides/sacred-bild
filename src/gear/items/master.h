#pragma once
#include <cstdint>

// The combo master's rune exchange tab (cUI_MasterLotto: the page of UI_WND_MASTER where four runes are exchanged for one the player chooses with
// the arrows). The tab asks a table by the number of runes in its slots for the gold the exchange costs, enables its button with two or more runes
// and shows the selector only with four. Mods change what the tab does through three events (src/gear/lua/kernel.lua `kernel.master_event`):
//
//   * master.price (rune, ability, count): the gold the exchange costs for the chosen `rune` (an item type) that teaches `ability`, with `count` runes in
//     the slots. Gear writes the answer into the engine's table entry for `count` before the tab draws and before it reads the entry (the exchange
//     button), and puts the engine's own value back when no mod answers. 0 hides the price.
//   * master.layout (): true takes the runes away: the four slots get no size, so nothing can be dropped on them.
//   * master.exchange (rune, ability, count): true when the mod did the exchange (the engine has paid by then): the engine's own exchange, which takes
//     the runes and puts the chosen rune into the result box, is skipped.
//
//   * master.describe (rune, ability, count): a level (not nil or false) makes Gear write the engine's own description of the shown rune's art or spell
//     at that level, "<name> - Level <n>", at the left of the page (font 3, at 0x10, 0x8C in the pixels of the header's own rect), after the engine drew the page.
//   * master.maximum (rune, ability, count): true makes Gear write the text "Maximum Level Attained" (the game's text 0x9327CD45) in red under the result
//     box, centered on it.
//
// The conditional jumps that tie the selector, the button and the price label to the rune count and the price are site hooks (`master.select`,
// `master.ready`, `master.gold` of the event `combat.branch`, src/gear/sitehook.cpp).
namespace Master
{
    // Hooks the tab's render, layout and exchange functions (inside the Patch transaction of Hooks::install).
    bool install();

    // Whether some mod handles that event (`master.price`, `master.layout`, `master.exchange`, `master.describe`, `master.maximum`): the hooks are pass-throughs while not.
    void setActive(const char* event, bool active);
}
