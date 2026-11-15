-- Self test of gear.ui.item_icon: a grid of weapon and armour models drawn by type id (row 1) and from live items
-- spawned beside the hero (row 2). Screenshots, then quits. Run isolated:
--   Sacred.exe -gear-auto -gear-load=1 -gear-instance=icons-1 -gear-timeout=100 -gear-mods=itemicontest
local A = gear.auto
local ui = gear.ui
local W = gear.world
local cfg = gear.config

local types = {}        -- {id, name}
local items = {}        -- item tables
local show = false
local results = {passed = 0, failed = 0}
local function check(name, cond, detail)
    results[cond and 'passed' or 'failed'] = results[cond and 'passed' or 'failed'] + 1
    print(('%s: %s%s'):format(cond and 'PASS' or 'FAIL', name, detail and (' (' .. tostring(detail) .. ')') or ''))
end

local function pick()
    local out, seen = {}, {}
    for _, word in ipairs({'SWORD', 'BOW', 'AXE', 'HELM', 'SHIELD', 'ARMOR', 'BOOT', 'STAFF', 'MACE', 'GLOVE'}) do
        for _, r in ipairs(gear.data.object_types:search(word, 40)) do
            local kind = r.kind
            if kind and (kind == 5 or kind == 0x21 or kind == 6 or kind == 0xD or kind == 0x11 or kind == 0x12 or kind == 0x13)
                and not seen[r.id] then
                seen[r.id] = true
                out[#out + 1] = {id = r.id, name = r.name}
                break
            end
        end
        if #out >= cfg.icons then break end
    end
    return out
end

local function draw(state)
    if not show or state.mode ~= 'game' then return end
    local x0, y0 = 120, 60
    ui.rect(x0 - 10, y0 - 10, 8 + #types * 110, 270, 0xD0101018, {layer = 'top'})
    ui.text('item_icon by type id', x0, y0 - 8, {layer = 'top', color = 0xFFFFD780})
    for i, t in ipairs(types) do
        if cfg.mode == 'live' then break end
        local x = x0 + (i - 1) * 110
        ui.rect(x, y0 + 12, 100, 100, 0x60304050, {layer = 'top'})
        ui.item_icon(t.id, x, y0 + 12, 100, 100, {layer = 'top'})
        ui.text(t.name, x, y0 + 114, {layer = 'top', size = 11, width = 100, align = 'center'})
    end
    ui.text('item_icon by item (live objects)', x0, y0 + 134, {layer = 'top', color = 0xFFFFD780})
    for i, it in ipairs(items) do
        if cfg.mode == 'type' then break end
        local x = x0 + (i - 1) * 110
        ui.rect(x, y0 + 152, 100, 100, 0x60304050, {layer = 'top'})
        ui.item_icon(it, x, y0 + 152, 100, 100, {layer = 'top'})
    end
end
gear.on('draw', draw)

gear.task(function()
    A.wait_for_menu(120)
    if not A.wait_for_game(180) then return A.quit(1) end
    gear.wait(3)
    types = pick()
    check('types found', #types >= 3, #types)
    local hero = W.hero()
    local x, y, layer = hero:position()
    for i, t in ipairs(types) do
        local s = W.spawn(t.id, x + 70 + 15 * i, y + 40, {layer = layer})
        local it = s and W.item(s.id)
        if it then items[#items + 1] = it end
    end
    check('items spawned', #items >= 3, #items)
    show = true
    gear.wait(1.5)
    A.screenshot('icons-1')
    gear.wait(1)
    A.screenshot('icons-2')
    gear.wait(4)      -- thousands of frames with temporary items made and destroyed
    local scratch = gear.inv.container(31)
    check('no item is left in the scratch container', scratch and #scratch:items() == 0, scratch and #scratch:items())
    local near = W.items_near(x, y, 600, {layer = layer, limit = 200})
    check('no item leaked into the world', #near == #items, ('%d near, %d spawned'):format(#near, #items))
    check('item_icon by name queues', ui.item_icon(types[1].name, 0, 0, 1, 1) == true)
    A.screenshot('icons-3')
    A.write('itemicontest.json', gear.json.encode(results))
    print(('%d passed, %d failed'):format(results.passed, results.failed))
    if cfg.quit then A.quit(results.failed == 0 and 0 or 1) end
end)
