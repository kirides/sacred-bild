# Controller

Sacred played with a gamepad, in the manner of Diablo 2 Resurrected: the left stick walks, the attack and combat
art buttons hit the nearest enemy in the stick's direction, potions, weapon slots and windows are buttons, and the
menus and windows get a cursor the sticks move and the D-pad jumps from button to button (with the game's own hover
effect).

Pads are read through [SDL3](https://www.libsdl.org): Xbox, PlayStation and Switch pads; under Wine / Proton through
Wine's own controller support. With `[Controller] Enabled=1` (the default) a controller plays the game; the first
input from it takes over from the keyboard and mouse, moving the mouse a few pixels or pressing a key hands it back.

## In game

- **Left stick**: walk (pushed less than halfway: walk instead of run, `Walk`). The hero walks straight in the
  stick's direction, to the pixel, as the game's own hold-to-walk does: SacredBild gives him the game's
  follow-the-cursor move order directly, without the click that would first path-find to the center of a ground
  cell. If something blocks his way, he walks on as soon as the stick points somewhere free. Walking never attacks,
  talks or picks something up on the way.
- **Attack** (A): attacks the nearest enemy in the left stick's direction (the right stick aims instead when
  pushed), as long as the button is held, and moves on to the next one when it dies. Without an enemy it talks to,
  opens or picks up what is nearest; with nothing there it attacks in place (Ctrl).
- **Combat art** (X): right click at the nearest enemy (the active combat art). The **art slot** buttons select
  slot 1-5 (keys 6-0) and do the same (`ArtClick`).
- Everything else presses the game's key: weapon slots 1-5, the potions, windows, quick save / load, zoom.

Default layout ("LT+" = with LT held):

| Button | Action | With LT |
|---|---|---|
| A | Attack / interact | Combat art slot 5 |
| X | Combat art (right click) | Weapon slot 1 |
| Y, B, RB, RT | Combat art slots 1, 2, 3, 4 | Weapon slots 2, 3, 4, 5 |
| LB | Stand still (Ctrl) | |
| D-pad up / down / left / right | Healing potion / heal hirelings / antidote / concentration | Undead Death / Mentor / zoom out / zoom in |
| Start / Back | Game menu (Esc) / inventory | Options / world map |
| L3 / R3 | Show names (Alt) / overview map (Tab) | Collect all / cursor mode on and off |

## Menus and windows

In menus and windows (and in game in cursor mode, e.g. for a conversation the game shows as a window SacredBild
doesn't recognize): the left stick moves the cursor, the D-pad jumps to the next button, menu entry, list or check
box that way, A and X are the left and right mouse button (A twice quickly: a double click, e.g. to load a
savegame), B is Esc, the right stick scrolls; Start and Back keep their window keys; LB / RB switch the inventory's
pages (backpack, combat arts, combos). While a message box or the game menu is open, the D-pad stays in it and the
cursor starts on its OK or first entry; in a message box B is its Cancel (or its only button). In an NPC dialog with
answers, A is Enter (the first answer) and B clicks the second one. R3 shows or hides the overview map (one press
each, where Tab on the keyboard shows it while held).

The log book (L) has no cursor: LB / RB switch the tabs across the top, the D-pad (up / down) the tabs down the
left edge; the left stick picks an entry of the list (up / down) and turns the list's pages (left / right), the
right stick turns the right page's (right or down: the next); B closes it. In cursor mode it works with the cursor
instead.

## Button prompts

`[Controller] Prompts=1` (the default; Direct3D 9 backend): icons of the pad's buttons, in the labels of its maker
(Xbox, PlayStation or Switch, as SDL reports the pad), next to what they do:

- in menus and windows, A left of the control under the cursor, B at a message box's Cancel, LB / RB above the
  inventory tabs they switch to;
- in the log book, LB / RB above the neighbouring tabs, the D-pad beside the side tabs;
- in an NPC dialog, A and B beside the first and second answer;
- in game, while Show names (L3) is held or the help screen (H) is open, each HUD slot's binding: above the weapon
  slots (1-5), the combat art slots (6-0) and the potion buttons (Space, Q, W, E, R; shown by the game's "show
  potions" option), two-button bindings as e.g. LT + X.

All prompts are drawn in one draw call from one small texture atlas. The images are the
[Input Prompts](https://github.com/meritite-union/input-prompts) by the Meritite Union (CC0);
`tools/gen_prompts.py` builds the atlas (`src/overlay/prompts.png`) from them.

## Options screen

Opening the Options with the controller shows SacredBild's own screen (Direct3D 9 backend): the game's settings
(with the game's own texts), and a Controller page with the settings below and every action's button: select one
and press a button, or hold one and press another for a two-button binding; X clears one. LB / RB switch pages,
Start accepts (the game saves and applies its settings as from its own window; the controller's go to
`SacredBild.ini`), B cancels. With the keyboard or mouse the game's own Options window opens.

## Settings

| Section | Key | Default | Meaning |
|---|---|---|---|
| Controller | Enabled | 1 | Play with a controller. |
| Controller | Deadzone | 24 | Percent of a stick's travel ignored around its center. |
| Controller | CursorSpeed | 900 | Cursor speed in menus and windows at full deflection, in 1024x768 pixels per second. |
| Controller | MoveRadius | 160 | How far ahead of the hero (screen pixels) the cursor the hero follows goes while walking. |
| Controller | AimRange | 450 | Aim assist: enemies within this many pixels of the hero. |
| Controller | AimCone | 90 | Aim assist: enemies within this angle (degrees) around the stick's direction come first. |
| Controller | ArtClick | 1 | Combat art slot buttons select the art (keys 6-0) and right-click the target; 0 = select only. |
| Controller | Walk | 1 | The left stick pushed less than halfway walks (Shift) instead of running. |
| Controller | Prompts | 1 | Button prompts (see above). |
| Controller.Bindings | *action* | see above | Button for each action: `A B X Y LB RB LT RT Back Start L3 R3 Up Down Left Right`, or `Modifier+Button` (e.g. `LT+A`); empty = none. |
