---@meta _
-- Sacred Gear: menus that mods add items to (gear.menu).

-- ## Menus (gear.menu)

--- What a mod registers with `add_item`.
---@class GearMenuItemSpec
---@field id string Unique within the mod; registering the same id again replaces the item.
---@field label string|fun(): string The text (a function is asked each time the menu is drawn).
---@field on_click fun() Runs when the player picks the item.
---@field order? number Where it goes: smaller first (default 100; the Mods item is 900).
---@field tooltip? string
---@field enabled? boolean|fun(): boolean Default `true`.

--- An item as the mod that draws the menu gets it.
---@class GearMenuItem
---@field id string
---@field mod string The id of the mod that registered it.
---@field label string
---@field tooltip? string
---@field enabled boolean
---@field order number
---@field click fun() Runs the item's `on_click` (errors are the registering mod's, not the caller's).

-- path gear.menu
--- Menus a mod can add items to, and the mod that draws them. `'esc'` is the game menu (Escape in game). Items are kept
--- whether or not any mod draws the menu, so a mod registers its item once and draws a button of its own only while
--- `available('esc')` is false: with a native menu (a mod that replaces the game's menu with its own screen, `host`) the item is part of
--- it; without one the game's menu is untouched. See `gear.ui.replace_window`, `gear.ui.menu_entries` and `gear.ui.rml`.
---@class GearMenu
local GearMenu = {}

--- Whether a mod draws `menu` now (the native menu mod is on and called `host`).
---@param menu? string Default `'esc'`.
---@return boolean
function GearMenu.available(menu) end

--- This mod draws `menu` (`on` false: no longer). Only one mod can; it also replaces the game's window with `gear.ui.replace_window`.
---@param menu string
---@param on? boolean Default `true`.
---@return boolean ok
---@return string? message
function GearMenu.host(menu, on) end

--- Adds an item to `menu`.
---@param menu string
---@param spec GearMenuItemSpec
---@return boolean ok
function GearMenu.add_item(menu, spec) end

--- Removes this mod's item.
---@param menu string
---@param id string
---@return boolean found
function GearMenu.remove_item(menu, id) end

--- The items of the mods that are on, by `order`: what the mod that draws the menu lists after the game's own entries.
---@param menu string
---@return GearMenuItem[]
function GearMenu.items(menu) end
