# Building screens with RmlUi in Sacred Gear

A short guide to the pieces that make mod screens easy to write, reuse and look right with or without a theme. The full reference is
[GearApi.md](GearApi.md#htmlcss-documents-gearuirml).

## The look contract

Every RmlUi document gets, in front of its own styles, either the **theme mod's** stylesheet (a running mod whose `mod.lua` has `theme = 'theme.rcss'`,
e.g. sacred-theme) or Gear's **plain base style**. Both style the same class names: `.window` (`.titlebar`, `.body`), `.tabbar` with `a.link.tab`,
`.row` (`.space`, `.right`), `.heading`, `.label`, `.muted`, `button` / `.game-button`, `input.game-check`, `input.game-slider`, `.bar .fill`, `.list .item`,
`.padkey`. A mod uses these names and adds only what is special about its screen; it then runs without the theme (plainer) and looks like the game with it.

## gear.kit: the core widgets

`gear.kit` returns the markup of a window, tabs, buttons, check boxes, sliders, rows, bars, lists and a modal confirm dialog, built only from that contract
(see `src/gear/examples/rmlkit`). Join the strings, wrap them with `kit.page`, give them to `gear.ui.rml.load_string`, listen with `doc:on('#id', 'click', fn)`.
Every button, link and input is a stop for the controller with no extra work. Adding a widget means giving the kit its markup and the base style and the theme
their classes.

**Moving windows.** A window with a `.titlebar` and a `data-panel="name"` (`kit.window` sets it from the `id` when it has a title; `panel = false` keeps it fixed) is dragged
by its title bar, with the mouse, and stays on the screen. The place is kept in `mods\gear_layout.json` under `<mod id>:<name>` like the layout editor's, so both
agree and "Reset" in the editor puts it back. A close button or any other control in the bar keeps its click. `data-no-drag` on the window turns the drag off
(for a window that sits at a game panel and must stay there); windows without a title bar or a `data-panel` (the keyboard, the confirm dialog) never move.
Native `gear.ui.panel`s have always been draggable by their title (`draggable = false` turns it off).

## Icons

Every document can use the icon font of Sacred Gear (a 10 KB subset of Google's Material Symbols Sharp, Apache License 2.0, bundled in the DLL): `<span class="icon icon-refresh"></span>`
or `kit.icon('refresh')`. It takes the text colour and sizes with the text. 85 icons, among them `refresh`, `restart-alt`, `arrow-back`, `arrow-forward`,
`chevron-left`, `chevron-right`, `first-page`, `last-page`, `delete`, `close`, `check`, `add`, `remove`, `settings`, `search`, `content-copy`, `content-paste`, `backspace`,
`save`, `edit`, `undo`, `sort`, `keyboard`, `warning`, `info`; the whole list is `src/gear/ui/icons_css.inc`, more can be added with `pyftsubset` (see `src/gear/res/README-icons.txt`).

## Sharing between mods

A mod lists what it shares in `mod.lua`: `exports = {'templates/', 'styles/*.rcss', 'images/', 'fonts/'}` (a folder, `folder/*.ext`, a file, or `'*'`).
While that mod runs, documents of other mods name the files as `@mod-id/path`: `<link type="text/template" href="@lib/templates/panel.rml"/>`,
`<link type="text/rcss" href="@lib/styles/panel.rcss"/>`, `src="@lib/images/a.png"`, `rml.load_font('@lib/fonts/x.ttf')`. Nothing else of that mod is
reachable and no path leaves its folder. Use `requires` in the user mod so the library runs first (`src/gear/examples/rmlshare-lib`, `rmlshare`).

The theme mod uses this itself: sacred-theme exports `templates/` (`sacred-screen`, a window as a body template; `sacred-group`, a framed section; `sacred-dropdown`, the game's
drop-down and its list). A template takes only content and the data model, no attributes, so what a mod has to set on its own element (`data-panel`, ids) stays in its markup.
The text of a template file is searched for tags as plain text: a comment there must not contain the angle-bracket form of `head`, `body`, `template`, nor a slash followed by `template`.

**A big stylesheet costs memory in every document.** RmlUi keeps its own copy of every rule per document that links the sheet. The theme therefore keeps the `.sprite-N` classes (a
rule per game picture, nine tenths of its rules) in a second sheet (`theme_sprites` / `theme_sprites_dp` in its `mod.lua`) that Gear links only into documents whose markup mentions
`sprite-` (or that load with `{sprites = true}`; `{sprites = false}` never). A class that Lua adds later, with the name nowhere in the markup, needs `sprites = true`.

## On-screen keyboard

`gear.kit.osk{layout = 'qwertz'}` makes a keyboard for text a controller player has to type (a search, a name, an amount): a small window of its own at the bottom of the
screen with the typed text, rows of bordered keys (not game buttons: there are too many of them) and Shift, Space, Backspace, Clear, Paste and Done; `digits = true` is a
number pad. Both looks style it (`.kit-osk`, `button.osk-key`), so it works without a theme too. The mod keeps its own field and gives the keyboard `on_change`:

```lua
local kb = gear.kit.osk{}                                   -- once; call kb:draw() every frame
kb:toggle{title = 'Search', text = q, max = 30, on_change = function(t) q = t; doc.model.q = t end, get = function() return q end}
kb:toggle{title = 'Gold', digits = true, text = gold, max = 8, on_change = set_gold}   -- a number pad
```

The button that opens it is `kit.ibtn('kbd', 'keyboard', {tip = 'On-screen keyboard'})` (or, in an `.rml` file, `<button class="ibtn" id="kbd"><span class="padkey a"></span><span class="icon icon-keyboard"></span></button>`).
**Text fields for a pad:** `kit.field{id = 'name', value = name, max = 24}` is an input with its keyboard button, which both looks show only while the controller is in use; `kit.field_attach(kb, doc, {id = 'name', title = 'Name', on_change = ...})` wires it, and then A on the field opens the keyboard too. A mod with its own `.rml` gives its keyboard button the classes `ibtn kit-kbd` and calls `kb:attach(doc, {field = '#search', button = '#kbd', open = fn})`. With the mouse and keys a field stays a plain field.

Every key is a stop for the D-pad (A types); the keyboard is modal, so the pad stays on it. B and Escape close it (`kb:back()` for a mod that handles B itself, with `back = false`).
Paste takes the clipboard without line breaks and other control characters, digits only on a number pad, cut to `max`. Demo and self test: `gear/examples/rmlosk`.

## Web-style CSS

`@font-face { font-family: "Icons"; src: url("icons.ttf") format("truetype"); }` works (the file is looked for next to the stylesheet, in its `fonts/`
folder, in the mod's `fonts/` folder, or as `@mod-id/...`); `.home::before { content: "\e88a"; }` puts generated text in the element itself
(`::after` is not supported). See `src/gear/examples/rmlcss`.

## Hot reload (developers)

Create an empty file `mods\.rml-hot` (or set the environment variable `GEAR_RML_HOT`) and every document reloads in place when one of the files it read
changes (the `.rml`, its `.rcss`, templates, shared files, images). The data model, the `doc:on` listeners and the scroll positions of boxes with an `id`
carry over; a document that fails to load stays as it was. What Lua changed by hand (`doc:text`, `doc:class`, ...) is gone: pass `on_reload = function(doc, err) ... end`
in the load options to put it back. Fonts load once per session. Players never have the switch on.

## Not done: filters and drop shadows

`box-shadow`, `filter`, `backdrop-filter` and `mask-image` need render layers (offscreen targets and clip masks) in RmlUi's render interface. Gear's
render interface records plain draw commands for the game's own UI pass and has no layers, so these need a new layer path through the game's device
(a larger change than a mod-UI feature). Until then a screen fakes depth with borders and translucent fills.
