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
