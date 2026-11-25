## v0.3.0 (2026-11-25)

- New: `Fullscreen` in the Display settings (`[Display] Fullscreen`): the window covers the whole screen at any resolution, e.g. 1024x768 or 1280x720 on a 1920x1080 screen, and the picture is scaled up to it. `Fit` keeps the picture's proportions with black bars, `Stretch` fills the screen. The mouse follows the scaled picture.
- Fixed: rain (and the other weather overlays) covered only part of the screen, e.g. two thirds of the width at 1920x1080 with the right third dry. The weather effects keep the screen size they saw when they were created; SacredBild now sets the real size every frame.

## v0.2.15 (2026-11-25)

- Under the hood: no ddraw.dll code changed; only the build files changed, to compile the new Sacred Gear sources (DDS images, packs) and link zlib.

## v0.2.14 (2026-11-25)

- New: pets for Sacred Gear: creatures drawn larger with their shadow, and optionally left out of world clicks and hover.
- Under the hood: new exports `SacredBild_PetSet` and `SacredBild_PetClear`, used by Sacred Gear.

## v0.2.13 (2026-11-24)

- Under the hood: no ddraw.dll code changed; the build only tracks the SacredGear icon font as a rebuild input.

## v0.2.12 (2026-11-24)

- Under the hood: no ddraw.dll code changed; the version bump hook can be skipped with SACRED_NO_BUMP=1.

## v0.2.11 (2026-11-22)

- Fixed: world view fields are checked against the game's code; on a moved layout the features stay off.
- Improved: on such exes the ground mesh, zoom, aim assist and water list switch off, with a log line saying why.

## v0.2.10 (2026-11-22)

- Fixed: ddraw.dll's file properties showed version 0.1.0; they now show the real version and git commit.
- Under the hood: version resources are generated at build time from the version files and the git commit.

## v0.2.9 (2026-11-22)

- Under the hood: version bump only; ddraw.dll code is unchanged since 0.2.8, the release's work was in SacredGear.

## v0.2.8 (2026-11-22)

- Fixed: zooming out over water crashed ReBorn exes (water tile list at the wrong offset).

## v0.2.7 (2026-11-22)

- Under the hood: no ddraw.dll code changed; the build only adds the SacredGear particle-effect module.

## v0.2.6 (2026-11-22)

- Fixed: menus were capped near 30 fps by a 20 ms sleep; they now follow [Display] FpsLimitMenu.
- Improved: the README recommends Gear Mod Manager for installation, manual steps kept below.
- Under the hood: the release no longer builds or attaches the matchmaker server; CI checks sacred-dsoal.json.

## v0.2.5 (2026-11-21)

- Improved: Menu screens laid out beyond 1024x768 are drawn and clicked at their native size, on any exe.
- Fixed: ReBorn's character selection screen uses the native full-screen layout at HD sizes.

## v0.2.4 (2026-11-21)

- Improved: Controller button prompts show only on the button the pad is on, not on every control.
- Improved: The pad leaves a scroll list at its top or bottom and moves on to the control beyond it.

## v0.2.3 (2026-11-21)

- Under the hood: vcpkg dependencies pinned to a fixed builtin-baseline for reproducible builds.

## v0.2.2 (2026-11-21)

- New: The Sacred Gear Mods screen is an RmlUi document, paged with LB / RB.
- New: LB / RB page any open Sacred Gear screen or window; Select presses the escape menu's Mods button.
- Improved: Controls that draw their own button prompt no longer get a second prompt beside them.
- Improved: The D-pad scrolls RmlUi scroll areas, and pad focus skips controls out of sight.
- Improved: Sacred Gear button icons use nearest-texel filtering, so they stay sharp.
- Under the hood: New SacredBild_UiPostScroll, UiTakePage, UiPostPage, UiTakeSelect, UiSetNativePrompts exports.

## v0.2.1 (2026-11-21)

- Fixed: Controller input lost after a game started, as Sacred Gear took over the window's message chain.
- Under the hood: New SacredBild_SubclassWindow export, called by Sacred Gear before its own window procedure.
- Under the hood: Controller context changes are logged with their reasons; the walk trace logs are removed.

## v0.2.0 (2026-11-21)

- New: FpsLimitMenu setting in SacredBild.ini (default 30) and an "In menu:" option in settings.
- Improved: Menu and background frame limits now hold in the main menu, even for frames that skip the game's limiter.
- Improved: In the menus, the background limit (FpsLimitInactive) applies as the lower of it and the menu limit.

## v0.1.2 (2026-11-20)

- New: Sacred Gear RmlUi documents, HTML and CSS mod screens (prototype, gear.ui.rml).
- New: Controller button prompts in RmlUi documents through gear:pad/<button> images and the body.pad class.
- New: Xbox, PlayStation and Switch style controller icons for Sacred Gear (SacredBild_PadGlyph).
- Fixed: RmlUi documents load without URL errors, and body class and tag defaults work.
- Improved: A pad press from Sacred Gear switches the prompts to controller style, as a real press does.
- Under the hood: RmlUi and stb_image added as build dependencies in vcpkg.json and CMake.

## v0.1.1 (2026-11-19)

- Fixed: Crash with many creatures coming and going, as the GPU skinning stats read freed skeletons.

## v0.1.0 (2026-11-19)

- New: Direct3D 9 renderer behind the game's DirectDraw calls, shipped as the SacredBild ddraw.dll.
- New: SMAA anti-aliasing and sharpening filters for the game image.
- New: GPU skinning for characters and creatures.
- New: Frame rate limit for the game, set with FpsLimit in SacredBild.ini.
- New: Controller support with gamepad bindings, click-to-walk and aim assist.
- New: Resolution, window fit and zoom handling for the game window.
- New: Faster loading through a file cache, a map cache and parallel model loading.
- New: Screenshots and crash dump files written when the game crashes.
- New: Controller navigation of Sacred Gear mod screens and windows.
