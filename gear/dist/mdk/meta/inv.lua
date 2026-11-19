---@meta _
-- Sacred Gear: inventories.

-- ## Inventories (gear.inv)
--: The game keeps the hero's backpack, the chest and the shops' stock as grids of cells, "containers" (32 of them). The
--: backpack of the hero with object id n is container n (the local hero in single player: 1), the chest is container
--: 17. An item takes w x h cells; `col` and `row` are its top-left cell, counted from 0. Everything that changes an
--: inventory works in single player (`gear.world.can_change()`) and, in a network game, on the local hero's backpack
--: and the chest (`can_change()` below); the calls go through the game's own inventory and object code, so the cells,
--: the items' owners and the object lists stay as when the player moves an item.
--: `export` copies an item to bytes (its type and own data: the random properties), `add` makes it again, so that an
--: item can leave the game and come back unchanged (see the `shared-stash` mod).

-- path container
--- One inventory grid, from `gear.inv.container`.
---@class GearContainer
---@field index integer The container's number (0..31).
---@field columns integer Cells across.
---@field rows integer Cells down.
local GearContainer = {}

--- The items in the container, in cell order: `gear.world.item` tables plus the cell and size.
---@return GearInvItem[]
function GearContainer:items() end

--- Room for a `w` x `h` item: its top-left cell, or nil.
---@param w integer
---@param h integer
---@return integer? col
---@return integer? row
function GearContainer:free_spot(w, h) end

--- Puts an item that lies in the world or was taken out of a container at the cell (the cells must be free).
---@param item_id integer
---@param col integer
---@param row integer
---@return boolean? ok
---@return string? error
function GearContainer:put(item_id, col, row) end

--- Takes the item out; the item object stays (put it somewhere or `gear.inv.destroy` it).
---@param item_id integer
---@return boolean? ok
---@return string? error
function GearContainer:take(item_id) end

--- An item in a container: a `GearItem` with its place in the grid.
---@class GearInvItem : GearItem
---@field col integer Top-left cell (from 0).
---@field row integer
---@field w integer Width in cells.
---@field h integer Height in cells.

-- path gear.inv
--- Inventories: containers, the items in them, and items copied to bytes and made again.
---@class GearInv
---@field CHEST integer The chest's container number (17).
local GearInv = {}

--- A container: `'chest'`, `'backpack'` (the local hero's) or its number (0..31). Nil if there is none.
---@param which 'chest'|'backpack'|integer
---@return GearContainer?
function GearInv.container(which) end

--- Whether the local hero's inventory may be changed now, and whether a network game runs. Single player: as
--- `gear.world.can_change()`. In a network game (LAN, open net) the changes go through the engine's own announcing calls
--- (an item made is announced as created and updated, put in and taken out as inventory moves, destroyed as a
--- destruction) and only the hero's own backpack and the chest may be changed there; spawning stays refused. The temporary
--- items of icons and tooltips are made local-only there: not announced, put in and taken out without the announcing flag
--- and destroyed through the engine's local path.
---@return boolean allowed
---@return boolean network
function GearInv.can_change() end

--- The size of an item in cells, or nil.
---@param item_id integer
---@return integer? w
---@return integer? h
function GearInv.size(item_id) end

--- Puts an upgrade item (a rune lying in the world, e.g. made with `create`, or in no container) into a free socket of
--- an item, as the blacksmith does: the item gains the rune's bonuses and an entry, the rune is consumed. Single
--- player only.
---@param item_id integer
---@param upgrade_id integer
---@return boolean? ok
---@return string? error
function GearInv.socket(item_id, upgrade_id) end

--- Copies an item to a binary string: its type and its own data, not where it is. `hex` makes it text for JSON.
---@param item_id integer
---@return string? bytes
---@return string? error
function GearInv.export(item_id) end

--- Makes a new item from `export`'s bytes, lying at the hero's feet (single player only). Put it into a container
--- or `destroy` it.
---@param bytes string
---@return integer? item_id
---@return string? error
function GearInv.create(bytes) end

--- Deletes an item that is in no container.
---@param item_id integer
---@return boolean? ok
---@return string? error
function GearInv.destroy(item_id) end

--- Makes a new item from `export`'s bytes straight into the container (the first spot with room). Nothing changes
--- when it fails.
---@param container GearContainer
---@param bytes string
---@return integer? item_id
---@return integer|string? col The cell, or the error message when `item_id` is nil.
---@return integer? row
function GearInv.add(container, bytes) end

--- Takes the item out of the container, destroys it and returns its export. If that fails the item is where it was.
---@param container GearContainer
---@param item_id integer
---@return string? bytes
---@return string? error
function GearInv.remove(container, item_id) end

--- Bytes as lowercase hex text.
---@param bytes string
---@return string
function GearInv.hex(bytes) end

--- Hex text back to bytes, or nil if it is not hex.
---@param text string
---@return string?
function GearInv.unhex(text) end
