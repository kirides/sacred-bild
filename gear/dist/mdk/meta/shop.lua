---@meta _
-- Sacred Gear: merchants' stock.

-- ## Shops (gear.shop)
--: A merchant's wares live in the merchant window: three pages (its tabs, "Waffen", "Rüstungen", "Verschiedenes"), each a stock grid of its
--: own, a container of `gear.inv` (numbers 20 to 22). The window shows the merchant it was opened for last. Opening another merchant empties
--: the pages and stocks them anew; opening the same merchant again keeps the wares, and the game only swaps a few of them (often none) when it does.
--: Nothing is rolled when you merely open the window again, and mods that follow the `shop.stock` event keep that: the event fires only after the
--: game itself stocked a page, so what a mod adds then stays until the game resets that page. A mod that rolled its own chance on every opening would
--: change the stock at will.
--:
--: `shop.stock` fires once per page, in the order of the containers, right after the game stocked or changed the page: `reason` is `'new'` (another
--: merchant: the page was emptied and stocked anew), `'refresh'` (the window's refresh message did the same for the same merchant) or `'restock'`
--: (the game swapped some wares of the same merchant: only when the page really changed). A handler adds wares with `gear.shop.add` (the game's own
--: stocking code: room in the grid, the item, its random properties) and takes them out with `gear.shop.remove`. The added items are real items:
--: the player buys and sells them like the game's own wares. Single player and network games alike (the shop is on this PC).

-- path gear.shop
--- Merchants' stock pages.
---@class GearShop
local GearShop = {}

--- One stock page of the merchant window.
---@class GearShopPage
---@field container integer The page's grid: its container number (`gear.inv.container`).
---@field visible boolean The page is showing.
---@field categories GearShopCategory[] What the game stocks the page with.

--- A kind of ware the game's stock generator draws for a page, with its weight.
---@class GearShopCategory
---@field category integer The equipment category of the item picker: 5 weapon, 6 chest armour, 8 ring, 13 shield, 17 helmet, 18 boots, 19 belt, 20 amulet, 21 shoulders, 22 arms, 23 legs, 24 gloves, 25 wings, 29 horse equipment; 9 potions.
---@field weight integer

--- What `gear.shop.info` returns.
---@class GearShopInfo
---@field open boolean A page of the window is showing (the player is trading).
---@field merchant integer The merchant's object id.
---@field hero_level integer The local hero's level.
---@field pages GearShopPage[] The pages by container number.

--- What the `shop.stock` event gets.
---@class GearShopStock
---@field reason 'new'|'refresh'|'restock' Why the game stocked the page: another merchant (`'new'`), the window's refresh message (`'refresh'`), or a swap of some wares of the same merchant (`'restock'`, only when the page changed).
---@field merchant integer The merchant's object id.
---@field container integer The page that was stocked (its container number); the event fires once per page.
---@field hero_level integer The local hero's level.

--- Options of `gear.shop.add`.
---@class GearShopAddOpts
---@field container? integer The page (a `GearShopPage.container`, or the event's). Default: the first page that sells `category`, else the first page.
---@field type? integer The item type to stock (an index into `gear.data.object_types`).
---@field category? integer Without `type`: the equipment category the game's item picker draws the item from.
---@field quality? integer The quality tier the item is made with, 0 (plain) to 14 (the rarest): 5 to 11 magic, 12 to 14 rare. Default 0. Also what the picker is given.
---@field level? integer The item's level (default: the game's own, the hero's level with some spread).
---@field hero_pref? integer Without `type`: the hero type the picker prefers (0 none).

--- The merchant window's pages, or nil before any merchant has been opened (nothing stocked yet).
---@return GearShopInfo?
function GearShop.info() end

--- A stock page's grid as an inventory container, to read the items with `container:items()` (the cells, sizes and `gear.world.item` fields).
---@param page integer A `GearShopPage.container`.
---@return GearContainer?
function GearShop.container(page) end

--- The wares of one page, or of all pages: `gear.world.item` tables with `col`, `row`, `w`, `h` and `page` (the page's container number).
---@param page? integer A `GearShopPage.container`; nil for every page.
---@return GearInvItem[]
function GearShop.items(page) end

--- The item type the game's own picker gives for an equipment category at a level and quality (what the shop's stock generator asks it), without
--- stocking anything. nil if it has none.
---@param opts {category: integer, level?: integer, quality?: integer, hero_pref?: integer}
---@return integer? type
function GearShop.pick(opts) end

--- Stocks one ware into a page through the game's own code: the item is made, gets its random properties for `level` and `quality`, and takes the
--- first free place of the page's grid. Without `type` the picker chooses one of `category`.
---@param opts GearShopAddOpts
---@return integer? item_id
---@return string? error E.g. when the page is full.
function GearShop.add(opts) end

--- Takes a ware out of its page and destroys it.
---@param item_id integer
---@return boolean? ok
---@return string? error
function GearShop.remove(item_id) end
