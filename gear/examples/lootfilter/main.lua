-- A loot filter: the hero picks up only the items the rules allow (config.lua, or the in-game settings panel: F8 or
-- the overlay's button, saved to settings.json); the labels of the others are dimmed on the ground, and a small overlay
-- shows the rules in force. Gold is always picked up.
--
-- How it works: every quarter second the mod looks at the items lying around the hero (gear.world.items_near),
-- decides for each and hands the ones to refuse to Gear (gear.world.pickup_rules), which turns the game's pickup
-- down for them, for clicks and for the automatic collecting alike. Single player (and the host of a network game);
-- a network client's pickups go through the host and are not filtered.
--
-- Self test (no player needed): Sacred.exe -gear-auto -gear-load=1 -gear-mods=lootfilter

local cfg = gear.config
local ui = gear.ui
local W = gear.world

local QUALITY_RANK = {}
for i, q in ipairs(W.QUALITIES) do QUALITY_RANK[q] = i end
-- The game's label colours, for the overlay.
local QUALITY_COLOR = {normal = 0xFFEFEFEF, magic = 0xFFA0A0FF, rare = 0xFFF0E040, set = 0xFF15FF25, unique = 0xFFC19A33}
local CATEGORIES = {'weapon', 'armor', 'shield', 'jewelry', 'rune', 'scroll', 'special', 'other'}
local POTIONS = {'health', 'mana', 'stamina', 'colored', 'other'}

-- Checks the settings once; a bad value is logged and replaced by its default.
local function check_config()
    if cfg.classes ~= 'own' and cfg.classes ~= false and type(cfg.classes) ~= 'table' then
        print('config: classes must be "own", false or a list of classes; using "own"')
        cfg.classes = 'own'
    end
    if type(cfg.classes) == 'table' then
        local known = {}
        for _, c in ipairs(W.CLASSES) do known[c.name] = true end
        for _, c in ipairs(cfg.classes) do
            if not known[c] then print('config: unknown class ' .. tostring(c)) end
        end
    end
    if not QUALITY_RANK[cfg.min_quality] then
        print('config: min_quality must be one of normal, magic, rare, set, unique; using normal')
        cfg.min_quality = 'normal'
    end
    cfg.categories = type(cfg.categories) == 'table' and cfg.categories or {}
    for cat, rule in pairs(cfg.categories) do
        if rule ~= false and not QUALITY_RANK[rule] then
            print(('config: categories.%s must be a quality or false; ignored'):format(tostring(cat)))
            cfg.categories[cat] = nil
        end
    end
    cfg.potions = type(cfg.potions) == 'table' and cfg.potions or {}
end
check_config()

-- The classes whose items are kept (names), nil: any.
local function wanted_classes(hero)
    if cfg.classes == false then return nil end
    if cfg.classes == 'own' then
        local own = hero and hero:class()
        return own and {own} or nil
    end
    return cfg.classes
end

-- keep(item) -> true | false, reason
local function keep(item, classes)
    if item.gold then return true end
    if item.quest and cfg.keep_quest then return true end
    if item.category == 'potion' then
        local rule = item.potion_color and cfg.potions[item.potion_color]
        if rule == nil then rule = cfg.potions[item.potion or 'other'] end
        if rule == false then return false, 'potion' end
        return true
    end
    if classes then
        local usable = false
        for _, c in ipairs(classes) do
            if W.usable_by(item, c) then usable = true; break end
        end
        if not usable then return false, 'class' end
    end
    if item.set and cfg.keep_set then return true end
    local rule = cfg.categories[item.category]
    if rule == nil then rule = cfg.min_quality end
    if rule == false then return false, 'category' end
    if item.quality_rank < QUALITY_RANK[rule] then return false, 'quality' end
    return true
end

local stats = {near = 0, blocked = 0, reasons = {}}
local last_items = {}

local function refresh()
    local hero = W.hero()
    if not cfg.enabled or not hero then
        W.pickup_rules(nil)
        stats.near, stats.blocked = 0, 0
        return
    end
    local x, y, layer = hero:position()
    local items = W.items_near(x, y, cfg.radius * W.TILE, {layer = layer, limit = 512})
    local classes = wanted_classes(hero)
    local blocked, reasons = {}, {}
    for _, it in ipairs(items) do
        local ok, why = keep(it, classes)
        it.keep, it.why = ok, why
        if not ok then
            blocked[#blocked + 1] = it.id
            reasons[why] = (reasons[why] or 0) + 1
        end
    end
    W.pickup_rules({block = blocked, dim = blocked, dim_alpha = cfg.dim_alpha})
    stats.near, stats.blocked, stats.reasons = #items, #blocked, reasons
    last_items = items
end

gear.every(0.25, refresh)
gear.on('session.start', refresh)

-- The overlay: the rules in force, read only.
local function line(text, x, y, color)
    ui.text(text, x, y, {color = color or 0xFFE0E0E0, size = 11})
end

local function overlay(state)
    if not cfg.overlay or state.mode ~= 'game' or state.exclusive then return end
    local x, y = cfg.overlay_x, cfg.overlay_y
    local rows = 7 + #CATEGORIES
    local h = 18 + rows * 13
    ui.rect(x, y, 196, h, ui.rgba(0, 0, 0, 140))
    ui.frame(x, y, 196, h, ui.rgba(200, 170, 90, 200))
    ui.text('Loot filter', x + 6, y + 3, {color = cfg.enabled and 0xFFFFD780 or 0xFF909090, size = 15, bold = true})
    ui.text(cfg.enabled and 'on' or 'off', x + 6, y + 3, {width = 184, align = 'right',
        color = cfg.enabled and 0xFF80FF80 or 0xFFFF8080})
    local ty = y + 24
    local classes = wanted_classes(W.hero())
    line('Classes: ' .. (classes and table.concat(classes, ', ') or 'any'), x + 6, ty); ty = ty + 13
    line('Minimum:', x + 6, ty)
    line(cfg.min_quality, x + 90, ty, QUALITY_COLOR[cfg.min_quality]); ty = ty + 13
    for _, cat in ipairs(CATEGORIES) do
        local rule = cfg.categories[cat]
        line(cat, x + 14, ty, 0xFFB0B0B0)
        if rule == false then
            line('never', x + 90, ty, 0xFFFF8080)
        else
            local q = rule or cfg.min_quality
            line(q .. (rule and '' or ' *'), x + 90, ty, QUALITY_COLOR[q])
        end
        ty = ty + 13
    end
    local p = {}
    for _, k in ipairs(POTIONS) do
        if cfg.potions[k] ~= false then p[#p + 1] = k end
    end
    for _, color in ipairs({'black', 'blue', 'green', 'red', 'yellow'}) do
        if cfg.potions[color] == false then p[#p + 1] = 'no ' .. color end
    end
    line('Potions: ' .. (#p > 0 and table.concat(p, ', ') or 'none'), x + 6, ty); ty = ty + 13
    line(('Set items: %s, quest: %s'):format(cfg.keep_set and 'always' or 'by rule', cfg.keep_quest and 'always' or 'by rule'), x + 6, ty); ty = ty + 13
    line(('Nearby: %d items, %d filtered'):format(stats.near, stats.blocked), x + 6, ty); ty = ty + 13
    line(('Refused pickups: %d'):format(W.pickup_refused()), x + 6, ty, 0xFFB0B0B0)
    if ui.button('open-settings', x + 6, y + h + 4, 184, 20, 'Settings (F8)', {tooltip = 'Change the rules in game'}) then
        ui.show_panel('settings', not ui.panel_open('settings'))
    end
end

---------------------------------------------------------------------------------------------------------------
-- The settings panel: the same rules, changed in game and saved to settings.json in this folder (it overrides
-- config.lua; delete it to go back to config.lua).
---------------------------------------------------------------------------------------------------------------
local SETTINGS = 'settings.json'
local SAVED_KEYS = {'enabled', 'classes', 'min_quality', 'categories', 'potions', 'keep_quest', 'keep_set', 'dim_alpha'}
local saved_note = 0

local function load_settings()
    local text = gear.read(SETTINGS)
    if not text then return end
    local s, err = gear.json.decode(text)
    if type(s) ~= 'table' then
        print('settings.json ignored: ' .. tostring(err))
        return
    end
    for _, k in ipairs(SAVED_KEYS) do
        if s[k] ~= nil then cfg[k] = s[k] end
    end
    check_config()
    print('settings.json loaded')
end

local function save_settings()
    local s = {}
    for _, k in ipairs(SAVED_KEYS) do s[k] = cfg[k] end
    local ok, err = gear.save(SETTINGS, gear.json.encode(s))
    saved_note = gear.time() + 2
    if not ok then print('cannot save the settings: ' .. tostring(err)) end
    return ok
end

load_settings()
ui.show_panel('settings', false)

local QUALITY_OPTIONS = {'normal', 'magic', 'rare', 'set', 'unique'}
local CATEGORY_OPTIONS = {'(minimum)', 'normal', 'magic', 'rare', 'set', 'unique', 'never'}
local function index_of(list, v)
    for i, x in ipairs(list) do if x == v then return i end end
    return 1
end

local function settings_panel()
    local p = ui.panel('settings', 'Loot filter settings', 240, 70, 540, 392)
    if not p.open then return end
    local x, y = p.content_x, p.content_y
    local changed, _ = false, nil
    local c
    c, cfg.enabled = ui.checkbox('enabled', x, y, 'Filter on', cfg.enabled, {tooltip = 'Off: the hero picks up everything'})
    changed = changed or c

    -- Classes
    y = y + 26
    ui.label('l-classes', 'Pick up items for', x, y, {color = ui.COLORS.accent})
    local mode = cfg.classes == 'own' and 1 or cfg.classes == false and 2 or 3
    c, mode = ui.radio_group('classmode', x, y + 18, {'my class', 'any class', 'these classes:'}, mode,
        {horizontal = true, tooltips = {'The hero\'s own class', 'Every item, whatever its class',
            'The classes ticked below (for your other heroes too)'}})
    if c then
        changed = true
        if mode == 1 then cfg.classes = 'own'
        elseif mode == 2 then cfg.classes = false
        else
            local own = W.hero() and W.hero():class()
            cfg.classes = {own or 'seraphim'}
        end
    end
    local picked = {}
    if type(cfg.classes) == 'table' then for _, n in ipairs(cfg.classes) do picked[n] = true end end
    for i, cls in ipairs(W.CLASSES) do
        local cx = x + ((i - 1) % 4) * 128
        local cy = y + 40 + math.floor((i - 1) / 4) * 20
        local cc, on = ui.checkbox('class-' .. cls.name, cx, cy, cls.name, picked[cls.name] == true,
            {enabled = mode == 3})
        if cc then
            changed = true
            picked[cls.name] = on
            local list = {}
            for _, k in ipairs(W.CLASSES) do if picked[k.name] then list[#list + 1] = k.name end end
            cfg.classes = list
        end
    end

    -- Qualities (dropdowns last in each row so their lists cover what follows)
    y = y + 90
    ui.label('l-quality', 'Lowest quality', x, y, {height = 18, valign = 'middle', color = ui.COLORS.accent,
        tooltip = 'Items below it are left on the ground'})
    local rows = {}
    for i, cat in ipairs(CATEGORIES) do
        rows[i] = {cat = cat, x = x + ((i - 1) % 2) * 260, y = y + 30 + math.floor((i - 1) / 2) * 24}
        ui.text(cat, rows[i].x, rows[i].y, {height = 18, valign = 'middle'})
    end

    -- Potions and the rest
    local py = y + 30 + 4 * 24 + 8
    ui.label('l-potions', 'Potions', x, py, {color = ui.COLORS.accent})
    for i, k in ipairs(POTIONS) do
        local pc, on = ui.checkbox('potion-' .. k, x + (i - 1) * 100, py + 18, k, cfg.potions[k] ~= false,
            {tooltip = k == 'colored' and 'The black, blue, green, red and yellow potions' or nil})
        if pc then cfg.potions[k] = on; changed = true end
    end
    py = py + 46
    c, cfg.keep_set = ui.checkbox('keep_set', x, py, 'Set items always', cfg.keep_set == true)
    changed = changed or c
    c, cfg.keep_quest = ui.checkbox('keep_quest', x + 180, py, 'Quest items always', cfg.keep_quest ~= false)
    changed = changed or c
    ui.text('Dim filtered labels', x, py + 26, {height = 16, valign = 'middle'})
    c, cfg.dim_alpha = ui.slider('dim', x + 140, py + 26, 200, 0, 255, cfg.dim_alpha, {step = 5,
        tooltip = 'Alpha of the labels of filtered items (255: not dimmed)'})
    changed = changed or c

    if ui.button('save', x, py + 56, 120, 22, 'Save', {tooltip = 'Writes settings.json in the mod\'s folder'}) then
        save_settings()
    end
    if ui.button('close', x + 130, py + 56, 120, 22, 'Close') then ui.show_panel('settings', false) end
    if gear.time() < saved_note then ui.text('saved', x + 262, py + 56, {height = 22, valign = 'middle', color = 0xFF80FF80}) end

    -- The quality dropdowns, drawn last so an open list lies over the rows below it.
    c, cfg.min_quality = (function()
        local ch, i = ui.dropdown('min_quality', x + 140, y, 120, QUALITY_OPTIONS, index_of(QUALITY_OPTIONS, cfg.min_quality))
        return ch, QUALITY_OPTIONS[i]
    end)()
    changed = changed or c
    for i = #rows, 1, -1 do
        local r = rows[i]
        local rule = cfg.categories[r.cat]
        local cur = rule == false and 'never' or rule or '(minimum)'
        local ch, idx = ui.dropdown('cat-' .. r.cat, r.x + 80, r.y, 120, CATEGORY_OPTIONS, index_of(CATEGORY_OPTIONS, cur))
        if ch then
            local v = CATEGORY_OPTIONS[idx]
            cfg.categories[r.cat] = v == 'never' and false or v ~= '(minimum)' and v or nil
            changed = true
        end
    end
    if changed then refresh() end
end

gear.on('draw', function(state)
    if not state then return end
    overlay(state)
    if state.mode == 'game' and not state.exclusive then
        if ui.key_pressed(cfg.settings_key or 'f8') then ui.show_panel('settings', not ui.panel_open('settings')) end
        settings_panel()
    end
end)

---------------------------------------------------------------------------------------------------------------
-- Self test (-gear-auto): items spawned at the hero's feet, the rules applied to them, the labels shown, a click on
-- the spot (which picks up what lies there, nearest first), and a check of what is left.
---------------------------------------------------------------------------------------------------------------
if not gear.auto then return end
local A = gear.auto
local results = {passed = 0, failed = 0, items = {}}

local function check(name, ok, detail)
    if ok then results.passed = results.passed + 1 else results.failed = results.failed + 1 end
    print(('%s %s%s'):format(ok and 'PASS' or 'FAIL', name, detail and (': ' .. tostring(detail)) or ''))
end

local function describe(it)
    return ('%d %s kind 0x%x %s rarity %d classes [%s]%s%s keep %s %s'):format(it.id, tostring(it.type_name), it.kind,
        it.category, it.rarity, table.concat(it.classes, ','), it.potion and (' potion ' .. it.potion) or '',
        it.set and ' set' or '', tostring(it.keep), it.why or '')
end

local function first_type(text)
    for _, r in ipairs(gear.data.object_types:search(text, 50)) do
        if r.kind ~= 0 then return r.id, r.name end
    end
end

gear.task(function()
    check('main menu', A.wait_for_menu(120))
    if not A.wait_for_game(180) then
        check('game with a hero', false)
        A.write('lootfilter.json', gear.json.encode(results))
        return A.quit(1)
    end
    gear.wait(3)
    local hero = W.hero()
    local cls = hero:class()
    check('hero class', cls ~= nil, cls)
    results.class = cls

    -- What lies around already.
    refresh()
    for _, it in ipairs(last_items) do results.items[#results.items + 1] = describe(it) end
    print(('%d items around the hero, %d filtered'):format(stats.near, stats.blocked))
    for i = 1, math.min(#last_items, 25) do print('  ' .. describe(last_items[i])) end

    -- The potion types (kind 9), and a few weapons around the hero with their labels before any rule: their
    -- colours against the rarity read.
    local types = gear.data.object_types
    local potions = {}
    for id = 1, types:count() - 1 do
        if types:kind(id) == 9 then potions[#potions + 1] = ('%d %s'):format(id, tostring(types:name(id))) end
    end
    results.potion_types = potions
    print('potion types: ' .. table.concat(potions, '; '))
    local x, y, layer = hero:position()
    cfg.enabled = false
    refresh()
    local swords = {}
    for i, r in ipairs(types:search('SWORD', 6)) do
        local s = W.spawn(r.id, x + 60 * math.cos(i), y + 60 * math.sin(i), {layer = layer})
        local it = s and W.item(s.id)
        if it then swords[#swords + 1] = it; print('sample: ' .. describe(it)) end
    end
    A.key_down('alt')
    gear.wait(0.5)
    A.screenshot('lootfilter-samples')
    A.key_up('alt')
    cfg.enabled = true

    -- Two items at the hero's feet: a health potion (kept) and a weapon, refused by a test rule.
    local potion_type, potion_name = 0x13C0, types:name(0x13C0)   -- the small health potion
    if not potion_type and potions[1] then
        potion_type = tonumber(potions[1]:match('^%d+'))
        potion_name = types:name(potion_type)
    end
    local weapon_type, weapon_name = first_type('SWORD')
    results.types = {potion = potion_name, weapon = weapon_name}
    print(('spawning %s and %s'):format(tostring(potion_name), tostring(weapon_name)))
    local potion = potion_type and W.spawn(potion_type, x + 20, y + 20, {layer = layer})
    local weapon = weapon_type and W.spawn(weapon_type, x - 20, y - 20, {layer = layer})
    check('spawned a potion and a weapon', potion ~= nil and weapon ~= nil)
    cfg.categories.weapon = false
    refresh()
    local pi, wi = potion and W.item(potion.id), weapon and W.item(weapon.id)
    if pi then print('potion: ' .. describe(pi)) end
    if wi then print('weapon: ' .. describe(wi)) end
    check('the potion is an item', pi and pi.category == 'potion', pi and pi.category)
    check('the weapon is an item', wi and wi.category == 'weapon', wi and wi.category)
    local blocked_weapon = false
    for _, it in ipairs(last_items) do
        if wi and it.id == wi.id then blocked_weapon = it.keep == false end
    end
    check('the weapon is filtered', blocked_weapon)

    -- Labels (Alt), then a click where the hero stands: the game walks to the nearest item there.
    A.key_down('alt')
    gear.wait(0.5)
    A.screenshot('lootfilter-labels')
    A.key_up('alt')
    local s = A.state()
    local before = W.pickup_refused()
    for _ = 1, 4 do
        A.click(math.floor(s.width / 2), math.floor(s.height / 2))
        gear.wait(1.5)
    end
    A.screenshot('lootfilter-after')
    local p2, w2 = pi and W.item(pi.id), wi and W.item(wi.id)
    results.after = {potion = p2 and p2.on_ground, weapon = w2 and w2.on_ground, refused = W.pickup_refused() - before}
    print(('after the clicks: potion on the ground %s, weapon on the ground %s, refused %d'):format(
        tostring(p2 and p2.on_ground), tostring(w2 and w2.on_ground), W.pickup_refused() - before))
    check('the weapon stays on the ground', w2 and w2.on_ground)

    -- The settings panel: opened from the overlay's button, a quality picked, a potion unticked, saved.
    local before_settings = gear.read(SETTINGS)
    local oh = 18 + (7 + #CATEGORIES) * 13
    A.ui_click(cfg.overlay_x + 90, cfg.overlay_y + oh + 14)
    gear.wait(0.5)
    check('settings panel opens', ui.panel_open('settings'))
    A.screenshot('lootfilter-settings')
    local px, py = 248, 98 + 26 + 90           -- content (248, 98); the quality row
    A.ui_click(px + 190, py + 9)                -- the lowest quality dropdown
    gear.wait(0.5)
    A.screenshot('lootfilter-settings-dropdown')
    A.ui_click(px + 190, py + 18 + 1 + 18 + 9)  -- its second entry: magic
    gear.wait(0.5)
    check('lowest quality from the dropdown', cfg.min_quality == 'magic', cfg.min_quality)
    local potions_y = py + 30 + 4 * 24 + 8 + 18
    A.ui_click(px + 200 + 7, potions_y + 8)     -- the stamina checkbox
    gear.wait(0.5)
    check('potion checkbox', cfg.potions.stamina == false, tostring(cfg.potions.stamina))
    A.ui_click(px + 50, potions_y - 18 + 46 + 56 + 11)   -- Save
    gear.wait(0.5)
    local saved = gear.read(SETTINGS)
    check('settings saved', saved and saved:find('"magic"', 1, true) ~= nil, saved)
    A.screenshot('lootfilter-settings-saved')
    -- Leave the folder as it was.
    gear.save(SETTINGS, before_settings or '{}')
    A.write('lootfilter.json', gear.json.encode(results))
    print(('%d passed, %d failed'):format(results.passed, results.failed))
    if cfg.test_quit then A.quit(results.failed == 0 and 0 or 1) end
end)
