# Retained-mode RmlUi documents in Sacred Gear

Design and implementation notes for keeping the cost of RmlUi documents close to zero while nothing on them changes. Status: implemented in stages
(see "Stages" at the end); measured numbers are in "Results".

## The problem

Every mod that shows an RmlUi document calls `doc:draw()` from its `draw` handler every frame (src/gear/lua/kernel_ui.lua, `Doc:draw`). Before this
change one call did, for every document, every frame:

| Step | What it costs | Notes |
|---|---|---|
| `rml_rects`, `rml_scrolls`, `rml_scrollbars`, `rml_nav` | three to four walks of the whole element tree | the input regions, pad stops, scroll areas and scroll bars Lua registers |
| `rml_update` | `ensureButtonPrompts` (QuerySelectorAll), the CSS `content` walk, a mouse move, `Context::Update` | `Update` re-runs the hover chain, data models, style and layout checks of every element |
| `rml_draw` | `Context::Render`: every element's render path, one `Ui::Command` per mesh, each under the UI mutex | 600 commands per frame with 38 mods |
| replay (render thread) | `t_draw` copy, then per mesh: transform, pixel snap, clip, colour conversion per vertex; one `DrawPrimitive` per batch | the proxy (SacredBild) then copies and maps every vertex to the canvas again, the d3d9 backend copies it into its own dynamic buffer |

With 38 mods and 8 documents that was about 0.75 ms of RmlUi natives and 0.73 ms of replay per frame on a Ryzen 7 5700X3D (instrumented,
`SACREDGEAR_PROF=1`), while almost every document showed the same pixels as in the frame before.

A cache "skip `Update` and `Render` when nothing changed" was not done before because what changes the output is not only the layout: the mouse
(hover), scrolling, animations and transitions, data models, the caret's blink, textures that RmlUi lets go of. A stale cache would mean stale input
regions as well as stale pixels. The design below lists every source of change and where each one is caught.

## Principles

1. **A document is clean until something can have changed it.** Each document has two flags and a counter (`Doc` in src/gear/ui/rml.cpp):
   - `dirty`: `Context::Update` must run (an input reached RmlUi, a native changed the DOM, styles, attributes or data, the canvas changed).
   - `renderDirty`: the last recorded draw is out of date (set whenever `Update` ran, or a texture or mesh the draw used went away).
   - `gen`: counts every change (each `dirty` and each `Update` that ran). The tree walks (`rml_rects`, `rml_nav`, ...) remember the `gen` they were
     made at and give back the same rows while it has not moved.
2. **Every way into a document is a native, so the natives mark it.** Lua cannot reach RmlUi other than through `native.rml_*`. Each native is
   registered either as a reader (`guarded<F>`, marks nothing) or as a writer (`mutating<F>`, marks the document `dirty` after the call unless the
   native says the call changed nothing). New natives are writers unless they are added to the reader list on purpose.
3. **Time is RmlUi's own business.** RmlUi 6 tells how long its output stays valid: `Context::GetNextUpdateDelay()` after an `Update` is 0 while an
   animation, a transition or smooth scrolling runs, the caret's next blink while a text field has the focus, the key-repeat time of a held slider or
   scroll arrow, and infinity otherwise. A clean document is updated again when that time has come.
4. **One more update after any update.** A few things only settle on the frame after an update: the CSS `content` walk and `ensureButtonPrompts` act
   on the computed values and the elements the previous `Update` produced (a `data-for` row's button), and `Context::Update` runs the hover chain
   before the layout, so an element that moved under a still mouse gets its `:hover` one update later. So every `Update` that ran is followed by one
   more on the next frame, and if the content walk or the prompts changed the DOM the document stays `dirty`. An update that moved the hover (a
   `:hover` rule resized an element and another one came under the mouse) is followed by one more again, at most `kMaxSettle` (4) in a row:
   such a chain can take more than one step to settle, and a document whose `:hover` pushes the element away from the mouse never does.
5. **The output of a clean document is replayed, not rebuilt.** `rml_draw` keeps the commands of the last `Context::Render` (before `Ui::add`, so a
   `ui.scroll_begin` clip of the moment still applies on the way in). A clean document with the same layer, window slot, unit and origin gives them
   again. In stage 2 the replay becomes one command that refers to the list, and the render thread keeps the list's finished vertices (already
   transformed, snapped, clipped, mapped to the screen) in a vertex buffer: a clean document costs one draw call per texture run.
6. **No new behaviour for mods.** `doc:draw()` is called as before and returns the same count; the events, regions, stops and scroll areas are the same.
   Mods that set the same model value, class, attribute or text every frame do not make the document dirty any more (stage 3), so their documents
   become clean without changes in the mods.

## Sources of change and where each is caught

| Source | Caught by |
|---|---|
| Mouse moved over the document, entered or left it | `rml_update`: the integer mouse position or the hover flag differs from the last call (no `ProcessMouseMove` at all when it is the same) |
| Mouse buttons, wheel, keys, typed text, pad click, nudge, pad scroll and reveal | the input natives (`rml_mouse`, `rml_wheel`, `rml_key`, `rml_text`, `rml_click`, `rml_nudge`, `rml_pad_scroll` and `rml_pad_reveal` when they scrolled) are writers |
| DOM, attributes, classes, inline styles, values, focus, show / hide, scroll position | writers: `rml_html` (set), `rml_attr` (set), `rml_class`, `rml_style`, `rml_value` (set), `rml_append`, `rml_remove`, `rml_focus`, `rml_blur`, `rml_show`, `rml_scroll` (set), `rml_move`, `rml_resize`, `rml_origin`, `rml_listen`, `rml_query` / `rml_relatives` (they may add a `data-gh` attribute) |
| Data model values set by Lua | `rml_model_set` (a writer; stage 3: not when the new value equals the old one) |
| Data model values set by the document (`data-value`) | happen inside input processing, which a writer started |
| Animations, transitions, smooth scrolling, caret blink, slider and scroll arrow repeat | `GetNextUpdateDelay()` after each `Update` (principle 3) |
| Generated content (`content:`), button prompts, hover after a layout change | the trailing update (principle 4) |
| Canvas size, UI scale, screen origin, density | `fitContext` compares unit, dimensions, origin and the canvas scale with the last values and marks the document dirty |
| Fonts loaded later (`rml_font`), packs mounted or unmounted (`vfs_configure`, `vfs_unmount`) | all documents dirty |
| Hot reload | `hotReload` marks the document dirty |
| A texture or mesh used by the last draw is released (RmlUi lets go of an idle file image, a font atlas is rebuilt by another document, `gear.ui.texture` purges a shared texture) | `RenderInterface::ReleaseTexture` / `ReleaseGeometry` look the handle up in each document's set of handles used by its last draw and mark those documents `renderDirty` (and `dirty`, so RmlUi loads the texture again) |
| Idle file textures (minimap tiles) are swept after 8 s without a draw | a replayed draw refreshes `lastDraw` of the textures it uses, so a clean document keeps its images |
| A different layer, window slot, unit or origin in `rml_draw` | part of the key of the recorded draw |
| An item icon (`gear:item/...`) | recorded as an `ItemIcon` command like a mesh; the game draws the model every frame from the replayed command |

Everything else RmlUi does (layout, styles, data views, events) happens inside `Context::Update` or inside the processing of an input, so it is covered
by the rows above.

## The tree walks

`rml_rects`, `rml_nav`, `rml_scrolls` and `rml_scrollbars` fill a rows table that Lua hands in again every frame (`self.nav_rows` ...). When the
document's `gen` equals the `gen` of the last fill and the table is the same one (compared with `lua_topointer`), the native returns it unchanged:
no walk, no table writes. Lua writes into the rows of `rml_rects` after a move (`r.x, r.y = x, y`), but only after `rml_move` / `rml_resize`, which are
writers and move `gen`, so the next call refills them.

`rml_click` and `rml_nudge` used to rebuild the nav list themselves; they keep doing that (they are rare and must see the current tree).

## Replay (stage 2): retained geometry

The RmlUi render interface already hands Gear compiled geometry once (`CompileGeometry` -> `Ui::meshCreate`) and refers to it by id. What was
rebuilt every frame is the per-frame vertex data: each mesh command's vertices are transformed by its CSS matrix, snapped to screen pixels, clipped to
its scissor, converted to the colour format of the game's TL vertices, and then the SacredBild proxy copies and maps every vertex from the
1024 x 768 layout to the canvas, and the d3d9 backend copies them again into its own dynamic buffer.

`Ui::Kind::Retained` is a command that carries a `std::shared_ptr<const Ui::RetainedList>`: the mesh and item icon commands of one recorded draw.
`rml_draw` of a clean document adds this one command instead of hundreds. On the render thread the list keeps what it built:

- the commands split into runs: consecutive meshes with the same picture, sampling and blending form one run; an item icon is a run of its own;
- the finished vertices of all mesh runs in screen pixels (the layout vertices times SacredBild's placement, exactly what the proxy's
  `mapToCanvas` would do) in one `IDirect3DVertexBuffer7` made with the device's own `IDirect3D7::CreateVertexBuffer`, each run a range of it. Through
  SacredBild that is a Direct3D 9 buffer; the proxy passes vertex-buffer draws through unmapped (and, for pretransformed vertices, without the 3D
  overlay's viewport and projection), which is why the vertices are mapped already. Without SacredBild the placement is the identity and the buffer
  is the game's own Direct3D 7 one (a list of more than 65535 vertices is drawn from memory there);
- the key it was built for: the placement (scale, origin) and, per run, the texture's atlas rectangle. When the key differs the list is built again;
  when a buffer cannot be made or a draw fails, the run is drawn from its layout-unit vertices with `DrawPrimitive` as before
  (`GEAR_UI_NOVB=1` in the environment does that always).

Per frame a clean document then costs one `SetTexture` and one `DrawPrimitiveVB` per run. A document that changes every frame (an animation, the
minimap while the hero walks) is replayed the old way, command by command, and builds nothing; it is retained from the first clean frame on.

Buffers are released only on the render thread: a list that is freed on the Lua thread hands its buffer to a queue that the next `drawLayer` empties,
after the frames in flight (`Ui::setFramesInFlight`) have passed, as with pictures.

Only canvas draws (`slot == -1`) are retained; a document drawn into a game window's frame keeps the command path (the frame is pushed per command
there and confined by SacredBild).

## Mods that rebuild their models every frame (stage 3)

art-bars, bounty, custom-classes, sessiontracker and companions build their data tables in every `draw` and hand them to `doc:set` / `rml_model_set`,
or set the same text and classes again. Stage 3 makes the writers notice a call that changes nothing:

- `rml_model_set`: the new value is converted to a `Node` as before and compared with the old one; equal: no `DirtyVariable`, the document stays clean;
- `rml_class`: nothing when the class is already in that state;
- `rml_attr`, `rml_value`: nothing when the attribute / value already has that text;
- `rml_html`: nothing when the element's inner markup already is that text;
- `rml_style`: nothing when the inline property already holds the same parsed value;
- `rml_show`, `rml_focus`: nothing when the document / element already is in that state.

The Lua cost of building those tables stays in the mods (20 to 60 us each); caching them is mod work in the sacred-gear-mods repository, the same
pattern as the minimap's.

## Correctness checks

Automation runs on main and on this branch in the same test copy (1600 x 900 unless noted), the test mods of `src/gear/examples` and the self tests
of sacred-gear-mods:

| Test | main | retained |
|---|---|---|
| `rmltest-windrag` (title-bar drag), `rmltest-transform`, `rmltest-sliders`, `rmltest-checks`, `rmltest-itemscroll`, `rmltest-rings`, `rmltest-drag` | | all pass |
| `rmlosk` (`kit.osk`: every kind of key) | | 22 pass |
| layout mod self test (the layout editor) | | 28 pass |
| minimap self test | | 2 pass |
| key-bindings self test | 43 pass | 43 pass (one earlier run: 2 fails in the log book key step, not seen again) |
| `rmltest` | 27 pass, 1 fail | the same (`dom: query finds the appended element`) |
| better-saveload self test | 34 pass, 11 fail | the same (its load screen does not open in this copy) |
| art-bars self test | 4 pass, 1 fail | the same, plus its slot check is timing dependent |
| `padnavtest` (also at 1024 x 768) | 5 pass, 2 fail, then a script error at the slider stage | the same |

Screenshots the tests take (hover and pressed states of the theme's buttons and links, modal, layout editor, art bars, cool-downs) match main's
apart from the animated world. `GEAR_RML_NOCACHE=1` in the environment turns retained mode off (every document dirty every frame), for comparing
and for finding a missed source of change.

The UI sandbox's differential fuzzer ([Sandbox.md](Sandbox.md), `tools/sandbox/fuzz/rml_fuzz.lua`) compares each random document with an
`rml_nocache` twin of itself through random inputs and setters (events, tree walks, commands, pixels). It found the hover chains above and a
font atlas released while commands of the same frame still named it (see Sandbox.md, "Findings").

## Stages

1. Per-document dirty tracking: skip `Update`, `Render` and the tree walks of clean documents; replay the recorded commands.
2. Retained geometry: `Ui::Kind::Retained`, vertex buffers per document on the render thread.
3. Writers that change nothing leave the document clean.

## Results

1600 x 900 window, UI scale fit, `FpsLimit=0`, `VSync=0`, SMAA High, all 38 non-test mods of sacred-gear-mods main, savegame slot 10, a throwaway
copy of the game. The hero stands still for 40 s ("idle"), then walks for 38 s ("walk"); frame time = wall time / frames. Ryzen 7 5700X3D, RX 7900 XTX.

| Build | idle ms/frame | walk ms/frame |
|---|---|---|
| main (fce599b), 5 runs | 3.15 - 3.21 | 3.36 - 3.47 |
| stage 1 (dirty tracking), 2 runs | 2.95 - 2.96 | 3.14 - 3.22 |
| stage 2 (retained vertex buffers), 1 run | 2.77 | 2.99 |
| stage 3 (unchanged setters), 3 runs | **2.09 - 2.18** | **2.27 - 2.28** |
| + one buffer per list, no overlay mapping for it, 1 run | 2.04 | 2.26 |
| no mods, main / final | 1.20 / 1.11 | 1.26 / 1.21 |

What the mods cost per frame standing still went from about 2.0 ms to about 1.0 ms. Instrumented (`SACREDGEAR_PROF=1`, idle, microseconds per frame):

| | main | final |
|---|---|---|
| RmlUi natives (`rml_update`, `rml_draw`, `rml_nav`, `rml_scrolls`, `rml_scrollbars`, `rml_rects`) | 760 | 28 |
| replay of the HUD layer (`draw.hud`) | 735 | 419 |
| commands replayed per frame | 601 | 56 |
| Lua tick (`LuaHost::frame`) | 1546 | 818 |
| minimap `draw` | 514 | 42 |

Per document (the `RMLDOC` lines, 5 s idle, about 1950 frames): beastmaster, sacred-theme, both bounty boards, companions, custom-classes and the
minimap are updated and rendered 0 times; sessiontracker 14 times (its own changes and the mouse), art-bars about 90 times (updates RmlUi asked for
through `GetNextUpdateDelay`; not looked into further), world-events every frame (its ring and timer values change every frame, the mod already writes only what
changed). The HUD screenshot of the idle phase is the same as main's apart from the animated world and the randomly rolled bounties.

The vertex buffers themselves are not where the time went: drawing the same kept vertices from memory (`GEAR_UI_NOVB=1`) measured 2.03 and 2.20 ms
in two runs, inside the run-to-run spread. The gain of stage 2 is that the vertices are no longer made (and copied and mapped) every frame.

What is left is mostly the mods' own Lua (bounty 107 us, art-bars 107, custom-classes 80, sessiontracker 45, companions 34 per frame): they build
their model tables every frame; caching them (the minimap's pattern) is mod work in sacred-gear-mods.
