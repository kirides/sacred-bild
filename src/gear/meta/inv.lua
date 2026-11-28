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

--- First and last item type id a mod may define (6150 to 6199).
---@type integer
GearInv.TYPE_FIRST = 6150
---@type integer
GearInv.TYPE_LAST = 6199

--- The options of `define_type`.
---@class GearItemTypeOpts
---@field size integer[]? The size in cells, `{width, height}` (1 to 8), written into the item record the type points to. Every type that shares that record changes too, so use it with a `look` type no other item uses (refused when the record is shared by more than one other type).
---@field icon_scale number? With `size`: the scale the icon is drawn at in the item record (a game scroll uses 3.0 for 1 x 2 cells; a smaller number fits one cell).
---@field look integer? Another item type whose look the new type takes (its model, inventory icon and size, price record): a potion that looks like a scroll is `define_type(id, potion_type, name, {look = scroll_type})`. What the item does stays the base's.

--- Draws the inventory icon of every item of an item type with other textures (a merchant's window, the backpack, the chest and the windows of mods that draw the item with `gear.ui.item_icon`): `spec` is an image id for the whole model, or `{all = id, parts = {[n] = id}}` for single meshes (in the order the game draws them, as `creature:set_texture`). An id is `gear.ui.texture_image(name)` (a texture of the game's Texture.pak) or the `picture` of a `gear.ui.texture.load(path)` handle (an image file of the mod: PNG, TGA, JPG or DDS, both sides a power of two; keep the handle). The texture replaces the mesh's own one, so it needs the same texture layout (a type made with `define_type{look = ...}` has the layout of its look's model). nil clears it. Needs SacredBild 0.5.0 (every renderer: Direct3D 9, NVRHI on Direct3D 12 or Vulkan); set it again each time the mod starts.
---@param type integer
---@param spec integer|{all: integer?, parts: table<integer, integer>?}?
---@return boolean? ok
---@return string? error
function GearInv.set_type_texture(type, spec) end

--- The raw item record (0x102 bytes, as hex) an item type points to: the icon's scale and angles, the size in cells (bytes 0x1C and 0x1D), what `look` and `size` of `define_type` work with. nil and a message if there is none.
---@param type integer
---@return string? hex
---@return string? error
function GearInv.type_record(type) end

--- Defines an item type of Gear's own, `type` (6150 to 6199, free in the game's type table): a copy of the type record of `base` (any item type, e.g. a potion
--- or a rune) with the name text `name_text` (`gear.text.key` / `gear.text.add`; 0 keeps the base's name). The item looks and works like the base item
--- (a potion: its model and size; drinking it goes to the `item.use` event), and is a plain unknown item in a game without Sacred Gear. Define it every
--- time the mod starts, before a savegame loads: a savegame keeps the id. Spawn it with `gear.world.spawn(type, x, y)`.
---@param type integer
---@param base integer
---@param name_text integer? Default 0.
---@param opts GearItemTypeOpts?
---@return boolean? ok
---@return string? error
function GearInv.define_type(type, base, name_text, opts) end

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

---@class GearItemRequirements
---@field level integer
---@field strength integer
---@field dexterity integer
---@field charisma integer
---@field endurance integer
---@field skill integer The required skill's number (0: none).
---@field skill_level integer

---@class GearItemBonus
---@field slot integer 0..7, the bonus slot in its record (what `GearInv.bonus_text` takes).
---@field entry integer 0 for the item's own record, 1..n for a merged record (see `GearItemValues.entries`).
---@field code integer The bonus's number: 801..808 elemental damage / resistance, 809 attack, 810 defense, 600..798 a skill, 811..868 the named bonuses (`global.res` texts), 1..~140 a combat art, 0x321..0x364 protection groups.
---@field attribute integer The hero attribute the stored value is scaled by (1 strength, 2 dexterity, 3 charisma, 4 endurance ... 0: not scaled).
---@field flags integer The bonus's type word (low byte: the target kind, bits 8..11 the attribute / element selector, 0x2000 spells only, 0x4000 weapon damage only, 0x8000 resistance for codes 801..808).
---@field value integer The stored value (percent or points, by the bonus), as the record keeps it.

---@class GearItemRecord
---@field type integer The object type the record was made for.
---@field flags integer Raw flags (class bits among them).
---@field price integer
---@field level integer The item's level.
---@field requirements GearItemRequirements
---@field attack integer
---@field defense integer
---@field bw integer
---@field protection {physical: integer, fire: integer, magic: integer, poison: integer}
---@field damage {min: integer[], max: integer[]} Four values each (the four damage kinds), as stored.
---@field sockets integer[] Eight socket kinds (0 none, 1..4 sockets that hold a rune or gem).
---@field bonuses GearItemBonus[] The record's own bonus slots that are in use.
---@field blacksmith_level integer

---@class GearItemSocketRecord : GearItemRecord
---@field entry integer 1..n, the order of `export`.
---@field active boolean The engine merges this record into the item (its socket kind is 1..4).

---@class GearItemValues : GearItemRecord
---@field id? integer The item's id when read by id.
---@field entries GearItemSocketRecord[] The merged records of the item's sockets and random properties.

--- The item's resolved value record decoded (`export`'s block: level, requirements, damage, attack / defense / protection, the
--- eight bonus slots) with the records its sockets merged into it. The numbers are as stored, which is what the game derives its
--- tooltip lines from: bonuses scaled by a hero attribute show the stored percentage times the hero's attribute (use `bonus_text`
--- for the game's own line). Pass an item id or `export`'s bytes.
---@param item integer|string
---@return GearItemValues? values
---@return string? message
function GearInv.values(item) end

--- The game's own tooltip line of one bonus: slot 0..7 of the item's own record (`entry` 0) or of one of its merged records
--- (1..n), UTF-8 with the game's colour codes, in the game's language, as the tooltip builds it (the controlled hero's attributes
--- included). nil when the item or entry does not exist; empty when the slot has no bonus.
---@param item_id integer
---@param entry? integer Default 0.
---@param slot? integer Default 0.
---@return string?
function GearInv.bonus_text(item_id, entry, slot) end

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
