-- Items. The kind (the type record's +0x2E) -> category; quality from the rarity (as the label colours).
local ITEM_CATEGORY = {
    [5] = 'weapon', [0x21] = 'weapon', [0xD] = 'shield', [8] = 'jewelry', [0x14] = 'jewelry', [9] = 'potion',
    [0x1C] = 'rune', [0x10] = 'scroll', [7] = 'special', [0xF] = 'special', [0x1D] = 'special', [0x1E] = 'special',
    [0x1F] = 'special', [6] = 'armor', [0x11] = 'armor', [0x12] = 'armor', [0x13] = 'armor', [0x15] = 'armor',
    [0x16] = 'armor', [0x17] = 'armor', [0x18] = 'armor', [0x19] = 'armor',
}
local QUALITIES = {'normal', 'magic', 'rare', 'set', 'unique'}   -- ascending
local QUALITY_RANK = {}
for i, q in ipairs(QUALITIES) do QUALITY_RANK[q] = i end
world.QUALITIES = QUALITIES

local function quality_of(rarity, set)
    if set then return 'set' end
    if rarity >= 15 then return 'unique' end
    if rarity >= 12 then return 'rare' end
    if rarity >= 5 then return 'magic' end
    return 'normal'
end

-- Potions by type id (their type names are only model files: BOTTLE01.GRN, potion_large.grn ...): the three
-- sizes of health (0x13C0..), mana (0x13C3..) and stamina (0x13C6..) potions, and the coloured potions in three
-- sizes of five colours each (black, blue, green, red, yellow).
local POTION_COLORS = {'black', 'blue', 'green', 'red', 'yellow'}
local function potion_kind(type_id)
    if type_id >= 0x13C0 and type_id <= 0x13C2 then return 'health' end
    if type_id >= 0x13C3 and type_id <= 0x13C5 then return 'mana' end
    if type_id >= 0x13C6 and type_id <= 0x13C8 then return 'stamina' end
    for _, first in ipairs({0x1411, 0x1416, 0x1430}) do
        if type_id >= first and type_id < first + 5 then return 'colored', POTION_COLORS[type_id - first + 1] end
    end
    return 'other'
end

-- world.item(id) -> {id, type, type_name, kind, category, rarity, quality, quality_rank, set, classes (names; empty:
-- every class), class_mask, gold, quest, potion ('health', 'mana', 'stamina', 'colored', 'other') and potion_color,
-- on_ground, x, y, layer} | nil
function world.item(id)
    local it = type(id) == 'number' and native.item(id)
    if not it then return nil end
    it.type_name = data.object_types:name(it.type)
    it.category = it.gold and 'gold' or ITEM_CATEGORY[it.kind] or 'other'
    it.quality = quality_of(it.rarity, it.set)
    it.quality_rank = QUALITY_RANK[it.quality]
    it.class_mask = it.classes
    local classes = {}
    if it.class_mask ~= 0 and bit.band(it.class_mask, CLASS_INFO.all) ~= CLASS_INFO.all then
        for _, c in ipairs(CLASSES) do
            if bit.band(it.class_mask, c.bit) ~= 0 then classes[#classes + 1] = c.name end
        end
    end
    it.classes = classes
    it.quest = bit.band(it.type_flags, 0x1000000) ~= 0
    if it.category == 'potion' then it.potion, it.potion_color = potion_kind(it.type) end
    return it
end

-- The game's item label colours by quality (ENG 0x5C13C0): rarity 0-4 white, 5-11 blue, 12-14 yellow, 15 gold, set green.
world.QUALITY_COLORS = {normal = 0xFFEFEFEF, magic = 0xFFA0A0FF, rare = 0xFFF0E040, unique = 0xFFC19A33, set = 0xFF15FF25}

-- world.quality_color(quality | rarity, set) -> ARGB: a quality name or a rarity 0..15 (with `set` for set items).
function world.quality_color(q, set)
    if type(q) == 'number' then q = quality_of(q, set) end
    return world.QUALITY_COLORS[q] or world.QUALITY_COLORS.normal
end

-- world.item_color(item | id) -> ARGB | nil: the colour the game gives the item's label (as quality_color, but class items
-- take their class's colour unless the game's UNIQUE_COLOR is on); nil when the item is gone.
function world.item_color(item)
    local id = type(item) == 'table' and item.id or item
    if type(id) ~= 'number' then return nil end
    return native.item_color(id)
end

-- Whether an item (world.item's table) can be used by class `name` ('darkelf' ...).
function world.usable_by(item, name)
    if #item.classes == 0 then return true end
    for _, c in ipairs(item.classes) do if c == name then return true end end
    return false
end

-- Items lying in the world within `radius` of (x, y) on opts.layer (default 0), nearest first: world.item tables.
function world.items_near(x, y, radius, opts)
    assert(type(x) == 'number' and type(y) == 'number' and type(radius) == 'number', 'x, y, radius must be numbers')
    opts = opts or {}
    local out = {}
    for _, id in ipairs(native.items_near(x, y, opts.layer or 0, radius, opts.limit or 64)) do
        local it = world.item(id)
        if it then out[#out + 1] = it end
    end
    return out
end

-- Inventories: gear.inv. The game keeps a hero's backpack, the chest and so on as grids of cells ("containers", 32 of
-- them: the backpack of the hero with object id n is container n, the chest is container 17).
local inv = {CHEST = 17}

local Container = {}
Container.__index = Container

local function make_container(index)
    local info = type(index) == 'number' and native.inv_container(index)
    if not info then return nil end
    return setmetatable({index = index, columns = info.columns, rows = info.rows}, Container)
end

-- inv.container(which) -> container | nil. `which`: 'chest', 'backpack' (the local hero's) or the number (0..31).
function inv.container(which)
    if which == 'chest' then which = inv.CHEST
    elseif which == 'backpack' then which = native.hero()
    end
    if type(which) ~= 'number' then return nil end
    return make_container(which)
end

-- container:items() -> the items in it, in cell order: world.item's table plus `col`, `row` (top-left cell, from 0),
-- `w`, `h` (size in cells).
function Container:items()
    local out = {}
    for _, e in ipairs(native.inv_items(self.index)) do
        local it = world.item(e.id)
        if it then
            it.col, it.row, it.w, it.h = e.x, e.y, e.w, e.h
            out[#out + 1] = it
        end
    end
    return out
end

-- container:free_spot(w, h) -> col, row | nil: room for a w x h item.
function Container:free_spot(w, h)
    return native.inv_find_space(self.index, w, h)
end

-- container:put(item_id, col, row) -> true | nil, message: puts an item that lies in the world or was taken out of a
-- container at that cell.
function Container:put(item_id, col, row)
    return native.inv_put(self.index, item_id, col, row)
end

-- container:take(item_id) -> true | nil, message: takes the item out; the item object stays (put it somewhere or
-- inv.destroy it).
function Container:take(item_id)
    return native.inv_remove(self.index, item_id)
end

-- inv.can_change() -> allowed, network: whether the local hero's inventory may be changed now (single player as
-- world.can_change; in a network game too, through the engine's announcing calls, for the hero's own backpack and the
-- chest), and whether a network game runs.
function inv.can_change()
    return native.inv_can_change()
end

-- Merchants: gear.shop (native side src/gear/items/shop.cpp). A merchant's wares are spread over three pages (the window's tabs), each a stock grid of its own.
kernel.shop_api = {}
do
    local S = kernel.shop_api

    -- shop.CATEGORY: the equipment categories of the item picker by name.
    S.CATEGORY = {WEAPON = 5, CHEST_ARMOUR = 6, RING = 8, POTION = 9, SHIELD = 13, HELMET = 17, BOOTS = 18, BELT = 19, AMULET = 20,
                  SHOULDERS = 21, ARMS = 22, LEGS = 23, GLOVES = 24, WINGS = 25, HORSE = 29}

    -- shop.info() -> {open, merchant, hero_level, pages = {{container, visible, categories = {{category, weight}, ...}}, ...}} | nil
    function S.info() return native.shop_info() end

    -- shop.container(page) -> a stock page's grid as a gear.inv container | nil (`page`: the container number from info().pages or the event)
    function S.container(page)
        return type(page) == 'number' and make_container(page) or nil
    end

    -- shop.items(page) -> the wares of one page (container:items()) or, without `page`, of all pages (each with `page` = its container number)
    function S.items(page)
        local out = {}
        local info = native.shop_info()
        if not info then return out end
        for _, p in ipairs(info.pages) do
            if not page or page == p.container then
                local c = make_container(p.container)
                for _, it in ipairs(c and c:items() or {}) do
                    it.page = p.container
                    out[#out + 1] = it
                end
            end
        end
        return out
    end

    -- shop.pick{category, level, quality, hero_pref} -> item type | nil: what the engine's picker gives for an equipment category.
    function S.pick(opts)
        assert(type(opts) == 'table' and type(opts.category) == 'number', 'shop.pick needs a table with a category')
        local info = native.shop_info()
        local level = opts.level or (info and info.hero_level) or 1
        return native.shop_pick(opts.category, level, opts.quality or 0, opts.hero_pref or 0)
    end

    -- shop.add{container, type | category, quality, level} -> item id | nil, message: stocks one ware through the engine into a page (default: the
    -- first page that sells `category`, else the first). Without `type` the engine's picker chooses one of `category` at `level` and `quality` (0..14).
    function S.add(opts)
        assert(type(opts) == 'table', 'shop.add needs a table')
        local info = native.shop_info()
        if not info then return nil, 'no merchant has stocked the window yet' end
        local container = opts.container
        if not container then
            for _, p in ipairs(info.pages) do
                for _, c in ipairs(p.categories) do
                    if c.category == opts.category then container = container or p.container end
                end
            end
            container = container or info.pages[1].container
        end
        local quality = opts.quality or 0
        local item_type = opts.type
        if not item_type then
            assert(type(opts.category) == 'number', 'shop.add needs a type or a category')
            item_type = native.shop_pick(opts.category, opts.level or info.hero_level, quality, opts.hero_pref or 0)
            if not item_type then return nil, 'the picker found no item' end
        end
        return native.shop_add(container, item_type, quality, opts.level or 0)
    end

    -- shop.remove(item_id) -> true | nil, message: takes a ware out of its page and destroys it.
    function S.remove(item_id)
        for _, it in ipairs(S.items()) do
            if it.id == item_id then return native.shop_remove(it.page, item_id) end
        end
        return nil, 'the item is not in a shop page'
    end
end

-- inv.size(item_id) -> w, h | nil: the size in cells.
function inv.size(item_id)
    return native.inv_size(item_id)
end

-- inv.socket(item_id, upgrade_id) -> true | nil, message: puts the upgrade item (a rune) into a free socket of the
-- item, as the blacksmith does; the rune is consumed.
function inv.socket(item_id, upgrade_id)
    return native.inv_socket(item_id, upgrade_id)
end

-- inv.export(item_id) -> bytes | nil, message: the item's type and own data as a binary string (not where it is).
function inv.export(item_id)
    return native.inv_export(item_id)
end

-- inv.values(item_id_or_bytes) -> table | nil, message: the item's resolved value record decoded (its own 0x82-byte block of
-- sWeaponInfo: level, requirements, damage, attack / defense / protection, the eight bonus slots) plus the records its sockets
-- and random properties merged into it, from `export`'s bytes (or the item's id). Numbers as stored: what the item's tooltip
-- derives its lines from (a bonus scaled by a hero attribute shows its stored percentage times the hero's attribute).
do      -- (a block: the chunk's local variables are nearly used up)
local function u8(s, i) return s:byte(i + 1) or 0 end
local function u16(s, i) return u8(s, i) + u8(s, i + 1) * 256 end
local function i16(s, i) local v = u16(s, i) return v >= 32768 and v - 65536 or v end
local function u32(s, i) return u16(s, i) + u16(s, i + 2) * 65536 end
local function decode_record(s, at)
    local r = {
        type = u32(s, at), flags = u32(s, at + 4), price = u32(s, at + 8),
        level = u8(s, at + 25),
        requirements = {level = u8(s, at + 20), strength = u8(s, at + 21), dexterity = u8(s, at + 22), charisma = u8(s, at + 23),
            endurance = u8(s, at + 24), skill = u8(s, at + 26), skill_level = u8(s, at + 27)},
        attack = i16(s, at + 44), defense = i16(s, at + 46), bw = i16(s, at + 48),
        protection = {physical = i16(s, at + 50), fire = i16(s, at + 52), magic = i16(s, at + 54), poison = i16(s, at + 56)},
        damage = {min = {}, max = {}}, sockets = {}, bonuses = {},
        blacksmith_level = u8(s, at + 129),
    }
    for i = 0, 3 do
        r.damage.min[i + 1] = u16(s, at + 28 + i * 2)
        r.damage.max[i + 1] = i16(s, at + 36 + i * 2)
    end
    for i = 0, 7 do
        r.sockets[i + 1] = u8(s, at + 12 + i)
        local group, kind, value = u32(s, at + 74 + i * 4), u16(s, at + 58 + i * 2), i16(s, at + 106 + i * 2)
        if group ~= 0 or kind ~= 0 then
            r.bonuses[#r.bonuses + 1] = {slot = i, code = group % 65536, attribute = math.floor(group / 65536) % 256,
                flags = kind, value = value}
        end
    end
    return r
end
function inv.values(item)
    local bytes = item
    local id
    if type(item) == 'number' then id = item bytes = native.inv_export(item) end
    if type(bytes) ~= 'string' then return nil, 'no such item' end
    if #bytes < 6 + 130 then return nil, 'not an item export' end
    local n = u16(bytes, 4)
    if #bytes < 6 + (n + 1) * 130 then return nil, 'not an item export' end
    local v = decode_record(bytes, 6 + n * 130)
    v.id = id
    v.entries = {}
    -- A socket record i (1-based) counts when the item's socket kind i is 1..4 (the engine's rule for merging them).
    for i = 1, n do
        local e = decode_record(bytes, 6 + (i - 1) * 130)
        e.entry = i
        e.active = v.sockets[i] ~= nil and v.sockets[i] >= 1 and v.sockets[i] <= 4
        v.entries[i] = e
        for _, b in ipairs(e.bonuses) do b.entry = i end
    end
    for _, b in ipairs(v.bonuses) do b.entry = 0 end
    return v
end
end

-- inv.bonus_text(item_id, entry, slot) -> text | nil: the game's own line for a bonus: slot 0..7 of the item's record (entry 0) or of
-- one of its merged records (1..n), UTF-8 with the game's colour codes, as the tooltip shows it (the hero's attributes included).
function inv.bonus_text(item_id, entry, slot)
    return native.item_bonus_text(item_id, entry or 0, slot or 0)
end

-- inv.create(bytes) -> item_id | nil, message: a new item from export's bytes, lying at the hero's feet.
function inv.create(bytes)
    return native.inv_import(bytes)
end

-- inv.destroy(item_id) -> true | nil, message: deletes an item that is in no container.
function inv.destroy(item_id)
    return native.inv_destroy(item_id)
end

-- inv.add(container, bytes) -> item_id, col, row | nil, message: a new item from export's bytes straight into the
-- container (the first spot with room). Nothing changes when it fails.
function inv.add(container, bytes)
    local id, err = native.inv_import(bytes)
    if not id then return nil, err end
    local w, h = native.inv_size(id)
    local col, row
    if w then col, row = container:free_spot(w, h) end
    if not col then
        native.inv_destroy(id)
        return nil, 'no room'
    end
    local ok, perr = container:put(id, col, row)
    if not ok then
        native.inv_destroy(id)
        return nil, perr
    end
    return id, col, row
end

-- inv.remove(container, item_id) -> bytes | nil, message: takes the item out of the container, destroys it and
-- returns its export. If it fails the item is back where it was.
function inv.remove(container, item_id)
    local bytes, err = native.inv_export(item_id)
    if not bytes then return nil, err end
    local found
    for _, e in ipairs(native.inv_items(container.index)) do
        if e.id == item_id then found = e break end
    end
    if not found then return nil, 'the item is not in that container' end
    local ok, terr = native.inv_remove(container.index, item_id)
    if not ok then return nil, terr end
    local dok, derr = native.inv_destroy(item_id)
    if not dok then
        native.inv_put(container.index, item_id, found.x, found.y)
        return nil, derr
    end
    return bytes
end

-- inv.set_type_texture(type, spec) -> true | nil, message: other textures for the inventory icon of every item of an item type (as creature:set_texture).
function inv.set_type_texture(type_id, spec)
    assert(type(type_id) == 'number', 'set_type_texture wants an item type')
    if spec == nil then
        for part = 0, 8 do local ok, err = native.item_type_texture(type_id, part, 0) if not ok then return nil, err end end
        return true
    end
    if type(spec) == 'number' then spec = {all = spec} end
    assert(type(spec) == 'table', 'the texture is an image id, a table or nil')
    local parts = spec.parts or {}
    local ok, err = native.item_type_texture(type_id, 0, spec.all or 0)
    if not ok then return nil, err end
    for part = 1, 8 do
        ok, err = native.item_type_texture(type_id, part, parts[part] or 0)
        if not ok then return nil, err end
    end
    return true
end

-- inv.type_record(type) -> hex | nil, message: the raw item record (0x102 bytes) of an item type (for the look and the size: +0x1C, +0x1D).
function inv.type_record(type_id) return native.item_record(type_id) end

-- inv.define_type(type, base, name_text) -> true | nil, message: an item type of Gear's own (ids INV_TYPE_FIRST..INV_TYPE_LAST) that is a copy of
-- the type record of `base`, named by the text id `name_text` (gear.text.key / add; 0: the base's name). Looks and works like the base item.
inv.TYPE_FIRST, inv.TYPE_LAST = 6150, 6199
function inv.define_type(type_id, base, name_text, opts)
    assert(type(type_id) == 'number' and type(base) == 'number', 'define_type wants an item type and a base type')
    if type_id < inv.TYPE_FIRST or type_id > inv.TYPE_LAST then
        return nil, ('item types of a mod are %d to %d'):format(inv.TYPE_FIRST, inv.TYPE_LAST)
    end
    local ok, err = native.item_define_type(type_id, base, 0, name_text or 0)
    if ok and opts and opts.look then
        -- the look of another item type: its model variant (+0x08), item record (size, price: +0x18), +0x1C and the model's file name (+0x34..+0x5B)
        local words = {0x08, 0x18, 0x1C}
        for off = 0x34, 0x58, 4 do words[#words + 1] = off end
        ok, err = native.item_copy_words(type_id, opts.look, words)
    end
    if ok and opts and opts.size then ok, err = native.item_set_size(type_id, opts.size[1] or opts.size.w, opts.size[2] or opts.size.h, opts.icon_scale) end
    return ok, err
end

-- inv.hex(bytes) -> text, inv.unhex(text) -> bytes | nil: for keeping the bytes in JSON.
function inv.hex(bytes)
    return (bytes:gsub('.', function(c) return string.format('%02x', c:byte()) end))
end

function inv.unhex(text)
    if type(text) ~= 'string' or #text % 2 ~= 0 or text:find('[^0-9a-fA-F]') then return nil end
    return (text:gsub('%x%x', function(h) return string.char(tonumber(h, 16)) end))
end

-- Pickup rules per mod (gear.world.pickup_rules): merged after each tick and handed to the native side.
local pickup_rules = {}     -- mod -> {block = {id = true}, dim = {id = true}, dim_alpha}
local pickup_dirty = false

local function publish_pickup_rules()
    if not pickup_dirty then return end
    pickup_dirty = false
    local blocked, dimmed, alpha = {}, {}, 255
    for mod, r in pairs(pickup_rules) do
        if not mod.disabled then
            for id in pairs(r.block) do blocked[#blocked + 1] = id end
            for id in pairs(r.dim) do dimmed[#dimmed + 1] = id end
            alpha = math.min(alpha, r.dim_alpha)
        end
    end
    native.pickup_rules(blocked, dimmed, alpha)
end

-- A random point between `min` and `max` world units from (x, y).
function world.around(x, y, min, max)
    assert(type(x) == 'number' and type(y) == 'number', 'x and y must be numbers')
    min, max = min or 0, max or min or 0
    local a = math.random() * 2 * math.pi
    local r = min + math.random() * (max - min)
    return math.floor(x + math.cos(a) * r + 0.5), math.floor(y + math.sin(a) * r + 0.5)
end

-- world.clock() -> minutes since midnight (0..1439) of the game's day and night clock, hour, minute | nil.
function world.clock()
    local m = native.clock()
    if not m then return nil end
    return m, math.floor(m / 60), m % 60
end

-- spawn(type, x, y, opts) -> creature | nil, message. `type` is a type id or an object type name;
-- opts.layer (default: the hero's), opts.like (a creature whose faction, aggression, level and health share the new one takes), opts.label (a text kept for the creature, see creature:label()), opts.modifiers (see creature:set_modifiers).
local function spawn(mod, kind, x, y, opts)
    opts = opts or {}
    assert(type(x) == 'number' and type(y) == 'number', 'x and y must be numbers')
    local id = kind
    if type(kind) == 'string' then
        id = data.object_types:find(kind)
        if not id then return nil, 'no object type named ' .. kind end
    end
    assert(type(id) == 'number', 'type must be a type id or name')
    local layer = opts.layer
    if not layer then
        local h = native.hero()
        local o = h and native.object(h)
        layer = o and o.layer or 0
    end
    local oid, err = native.spawn(id, math.floor(x), math.floor(y), layer)
    if not oid then return nil, err end
    if opts.like ~= nil then
        local like = type(opts.like) == 'table' and opts.like.id or opts.like
        local ok, why = native.copy_allegiance(oid, like)
        if ok then ok, why = native.copy_stats(oid, like) end
        if not ok then
            native.remove(oid)
            return nil, why
        end
    end
    if opts.label ~= nil then
        labels[oid] = {text = tostring(opts.label), mod = mod.id}
    end
    if opts.modifiers ~= nil then
        local c = wrap(oid)
        local ok, err = c:set_modifiers(opts.modifiers)
        if not ok then
            c:remove()
            return nil, err
        end
    end
    log('[%s] spawned %s (type %d) as %d at %d,%d', mod.id, data.object_types:name(id) or '?', id, oid, x, y)
    return wrap(oid)
end

