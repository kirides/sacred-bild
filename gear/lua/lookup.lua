-- gear.lookup: find a "thing" of the game by typing a few letters. Shipped inside Sacred Gear and run once per mod, in the mod's own
-- environment (so `gear` below is the mod's). Nothing here is special: it only uses the public API (gear.data, gear.ui, gear.text).
--
--   * kinds: a kind is a list of things (items, creatures, abilities, or what a mod registers) with a name to show and the
--     words to search; `search` ranks them for a text, forgiving (see `words` and `score`).
--   * a picker: `open` shows a small search window; the player types, picks a thing and the callback `select` gets it and
--     answers what the lookup should return; `pick` does the same inside a task and returns it.

local ui = gear.ui
local L = {}

-- ---- forgiving matching -------------------------------------------------------------------------------------------------

local FOLD = {
    ['ä'] = 'a', ['Ä'] = 'a', ['ö'] = 'o', ['Ö'] = 'o', ['ü'] = 'u', ['Ü'] = 'u', ['ß'] = 'ss',
    ['à'] = 'a', ['á'] = 'a', ['â'] = 'a', ['ã'] = 'a', ['å'] = 'a', ['è'] = 'e', ['é'] = 'e', ['ê'] = 'e', ['ë'] = 'e',
    ['ì'] = 'i', ['í'] = 'i', ['î'] = 'i', ['ï'] = 'i', ['ò'] = 'o', ['ó'] = 'o', ['ô'] = 'o', ['õ'] = 'o',
    ['ù'] = 'u', ['ú'] = 'u', ['û'] = 'u', ['ñ'] = 'n', ['ç'] = 'c', ['ý'] = 'y',
    ['À'] = 'a', ['Á'] = 'a', ['Â'] = 'a', ['È'] = 'e', ['É'] = 'e', ['Ê'] = 'e', ['Ì'] = 'i', ['Í'] = 'i',
    ['Ò'] = 'o', ['Ó'] = 'o', ['Ù'] = 'u', ['Ú'] = 'u', ['Ñ'] = 'n', ['Ç'] = 'c',
}

function L.words(text)
    local s = tostring(text or '')
    s = s:gsub('[\195][\128-\191]', function(c) return FOLD[c] or c end):lower()
    local list = {}
    for w in s:gmatch('[%w]+') do list[#list + 1] = w end
    return list
end

local function distance(a, b, limit)
    local la, lb = #a, #b
    if math.abs(la - lb) > limit then return limit + 1 end
    local prev = {}
    for j = 0, lb do prev[j] = j end
    for i = 1, la do
        local cur = {[0] = i}
        local best = cur[0]
        for j = 1, lb do
            local cost = a:byte(i) == b:byte(j) and 0 or 1
            local v = math.min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost)
            cur[j] = v
            if v < best then best = v end
        end
        if best > limit then return limit + 1 end
        prev = cur
    end
    return prev[lb]
end

local function subsequence(q, w)
    local at = 1
    for i = 1, #q do
        local found = w:find(q:sub(i, i), at, true)
        if not found then return false end
        at = found + 1
    end
    return true
end

local function strict_score(q, w)
    if w == q then return 100 end
    if w:sub(1, #q) == q then return 85 - math.min(20, #w - #q) end
    if w:find(q, 1, true) then return 60 end
    return 0
end

local function loose_score(q, w)
    if #q >= 4 then
        local d = math.min(distance(q, w, 1), distance(q, w:sub(1, #q), 1))
        if d <= 1 then return 45 end
    end
    if #q >= 4 and #w <= #q * 2 and subsequence(q, w) then return 25 end
    return 0
end

function L.score(query, words)
    local qs = type(query) == 'table' and query or L.words(query)
    if #qs == 0 then return 0 end
    local total = 0
    for _, q in ipairs(qs) do
        local best = 0
        for _, w in ipairs(words) do
            local s = strict_score(q, w)
            if s > best then best = s end
        end
        if best == 0 then
            for _, w in ipairs(words) do
                local s = loose_score(q, w)
                if s > best then best = s end
            end
        end
        if best == 0 then return nil end
        total = total + best
    end
    return total / #qs
end

-- ---- kinds ------------------------------------------------------------------------------------------------------------------

local kinds = {}

function L.register(kind, def)
    assert(type(kind) == 'string' and kind ~= '', 'kind must be a name')
    assert(type(def) == 'table' and type(def.things) == 'function', 'def.things must be a function that returns the things')
    kinds[kind] = {label = def.label or kind, things = def.things, name = def.name, text = def.text, detail = def.detail, cache = nil}
end

function L.kinds()
    local list = {}
    for name in pairs(kinds) do list[#list + 1] = name end
    table.sort(list)
    return list
end

-- The things of a kind, each with `name` (what the picker shows), once (the list is kept; `refresh` drops it).
local function loaded(kind)
    local k = kinds[kind]
    assert(k, 'unknown lookup kind ' .. tostring(kind))
    if not k.cache then
        local list = k.things() or {}
        local cache = {}
        for i, thing in ipairs(list) do
            local shown = k.name and k.name(thing) or thing.name or tostring(thing.id or i)
            cache[i] = {thing = thing, shown = tostring(shown), words = L.words(k.text and k.text(thing) or shown)}
        end
        k.cache = cache
    end
    return k.cache, k
end

function L.refresh(kind)
    if kinds[kind] then kinds[kind].cache = nil end
end

-- The things of `kind` that match `text`, best first (all of them in their own order for an empty text). `filter(thing)` may
-- leave things out. Returns the things.
function L.search(kind, text, limit, filter)
    local cache = loaded(kind)
    limit = limit or 50
    local qs = L.words(text)
    local out = {}
    if #qs == 0 then
        for _, e in ipairs(cache) do
            if not filter or filter(e.thing) then
                out[#out + 1] = e.thing
                if #out >= limit then break end
            end
        end
        return out
    end
    local scored = {}
    for i, e in ipairs(cache) do
        local s = L.score(qs, e.words)
        if s and (not filter or filter(e.thing)) then scored[#scored + 1] = {e = e, score = s, order = i} end
    end
    table.sort(scored, function(a, b)
        if a.score ~= b.score then return a.score > b.score end
        local la, lb = #a.e.words, #b.e.words
        if la ~= lb then return la < lb end
        return a.order < b.order
    end)
    for i = 1, math.min(#scored, limit) do out[i] = scored[i].e.thing end
    return out
end

function L.name(kind, thing)
    local k = kinds[kind]
    assert(k, 'unknown lookup kind ' .. tostring(kind))
    return tostring(k.name and k.name(thing) or thing.name or thing.id)
end

-- ---- built-in kinds ---------------------------------------------------------------------------------------------------------

local ITEM_KINDS = {[5] = true, [6] = true, [8] = true, [13] = true, [17] = true, [18] = true, [19] = true, [20] = true, [21] = true,
    [22] = true, [23] = true, [24] = true, [25] = true, [29] = true}
local KIND_MONSTER = 3
local SKIP = {grn = true, fem = true, mal = true, male = true, female = true, new = true, old = true, big = true, small = true,
    med = true, tiny = true, var = true, npc = true, boss = true, spawn = true, anim = true, ani = true}

-- Equipment item types, named as the game's tooltip shows them (the player's language). {id, name, internal, kind}
L.register('item', {
    label = 'Items',
    things = function()
        local list, types = {}, gear.data.object_types
        for id = 1, 0x7E5F do
            local kind = types:kind(id)
            if ITEM_KINDS[kind] then
                local internal, shown = types:name(id), types:display_name(id)
                if internal and shown then list[#list + 1] = {id = id, name = shown, internal = internal, kind = kind} end
            end
        end
        return list
    end,
    text = function(t) return t.name .. ' ' .. t.internal end,
    detail = function(t) return t.internal end,
})

-- Creatures as words of the monster types' names (ORC_WARRIOR.GRN: orc, warrior): {word, name, count, types = {ids}}.
-- `word` is what gear.drops, bounties and the like can match on.
L.register('creature', {
    label = 'Creatures',
    things = function()
        local by, list, types = {}, {}, gear.data.object_types
        for id = 1, 0x7E5F do
            if types:kind(id) == KIND_MONSTER then
                local name = types:name(id)
                if name then
                    local seen = {}
                    for w in name:lower():gmatch('%a+') do
                        if #w >= 3 and not SKIP[w] and not seen[w] then
                            seen[w] = true
                            local e = by[w]
                            if not e then
                                e = {word = w, name = w:gsub('^%l', string.upper), count = 0, types = {}, names = {}}
                                by[w] = e
                                list[#list + 1] = e
                            end
                            e.count = e.count + 1
                            e.types[#e.types + 1] = id
                            local shown = types:display_name(id)
                            if shown then
                                e.shown_set = e.shown_set or {}
                                if not e.shown_set[shown] then e.shown_set[shown] = true; e.names[#e.names + 1] = shown end
                            end
                        end
                    end
                end
            end
        end
        table.sort(list, function(a, b) return a.word < b.word end)
        return list
    end,
    text = function(t) return t.word .. ' ' .. table.concat(t.names, ' ') end,
    detail = function(t) return t.count .. (t.count == 1 and ' type' or ' types') end,
})

-- Single creature types by name (the player's language and the internal one): {id, name (localized, else internal), internal}.
-- What a bounty or event picks when it means one kind of creature: save `id`, never the name.
L.register('creature_type', {
    label = 'Creature types',
    things = function()
        local list, types = {}, gear.data.object_types
        for id = 1, 0x7E5F do
            if types:kind(id) == KIND_MONSTER then
                local internal = types:name(id)
                if internal then list[#list + 1] = {id = id, name = types:label(id), internal = internal} end
            end
        end
        return list
    end,
    text = function(t) return t.name .. ' ' .. t.internal end,
    detail = function(t) return t.internal end,
})

-- Abilities (combat arts and spells), named in the game's texts: {id, name, kind = 'combat_art' | 'spell'}.
L.register('ability', {
    label = 'Abilities',
    things = function()
        local list, abilities = {}, gear.data.abilities
        for _, id in ipairs(abilities:ids()) do
            local a = abilities[id]
            local text = a and gear.text.get(a.name_text)
            if text and text ~= '' then list[#list + 1] = {id = id, name = text, kind = a.kind} end
        end
        return list
    end,
    detail = function(t) return t.kind end,
})

-- ---- a text field -----------------------------------------------------------------------------------------------------------

local focus = nil          -- id of the text field that takes the keys

-- A one-line text field (the keyboard needs the game's focus; a controller uses the lists around it). Returns the new text,
-- and true when Enter was pressed. The field with the focus keeps the game's keys away while it is on screen.
function L.text_field(id, x, y, w, text, opts)
    opts = opts or {}
    local r = ui.region('lookup-f:' .. id, x, y, w, 20, {tooltip = opts.tooltip})
    if r.clicked then focus = id end
    if opts.focus and focus == nil then focus = id end
    local active = focus == id
    ui.rect(x, y, w, 20, ui.rgba(0, 0, 0, active and 220 or 140))
    ui.frame(x, y, w, 20, active and ui.rgba(240, 200, 110, 230) or ui.rgba(200, 170, 90, 140), 1)
    ui.text(text .. ((active and math.floor(gear.time() * 2) % 2 == 0) and '|' or ''), x + 4, y + 3, {size = 12, width = w - 8})
    local entered = false
    if active then
        local typed = ui.typed()
        if typed ~= '' then text = (text .. typed):sub(1, opts.max or 60) end
        if ui.key_pressed(8) and #text > 0 then
            local cut = #text
            while cut > 1 and text:byte(cut) >= 0x80 and text:byte(cut) < 0xC0 do cut = cut - 1 end
            text = text:sub(1, cut - 1)
        end
        if ui.key_pressed('enter') then entered = true end
        if ui.key_pressed('escape') then focus = nil end
    end
    return text, entered
end

function L.focused() return focus end
function L.unfocus() focus = nil end

-- ---- the picker -------------------------------------------------------------------------------------------------------------

local picker = nil         -- the open picker: {kind, opts, text, results, shown_for, index}
local drawing = false

local ROWS = 12
local PANEL = 'lookup'

local function close(p, cancelled)
    if picker ~= p then return end
    picker = nil
    focus = nil
    ui.capture_keys(false)
    ui.show_panel(PANEL, false)
    if p.opts.closed then p.opts.closed(cancelled) end
end

local function choose(p, thing)
    local opts = p.opts
    local result = thing
    if opts.select then
        result = opts.select(thing)
        if result == false then return end              -- the callback refuses: the picker stays open
        if result == nil then result = thing end
    end
    if opts.done then opts.done(result, thing) end
    if not opts.multi then close(p, false) end
end

local function draw_picker()
    local p = picker
    if not p then return end
    local opts = p.opts
    local panel = ui.panel(PANEL, opts.title or ('Find: ' .. (kinds[p.kind] and kinds[p.kind].label or p.kind)), 330, 120, 380, 330, {})
    if not panel.open then close(p, true) return end
    ui.capture_keys(true)
    local x, y, w = panel.content_x, panel.content_y, panel.content_w
    ui.text('Type a few letters of the name', x, y, {size = 11, color = 0xFFB0B0B0})
    local text, entered = L.text_field('picker', x, y + 16, w, p.text, {focus = true, tooltip = 'Several words in any order, typos are fine'})
    if text ~= p.text or not p.results then
        p.text, p.results, p.index = text, L.search(p.kind, text, 60, opts.filter), 1
    end
    local k = kinds[p.kind]
    local ry = y + 44
    local results = p.results
    if ui.key_pressed('down') then p.index = math.min(#results, p.index + 1) end
    if ui.key_pressed('up') then p.index = math.max(1, p.index - 1) end
    if p.index > p.top + ROWS - 1 then p.top = p.index - ROWS + 1 end
    if p.index < p.top then p.top = p.index end
    local picked = nil
    if #results == 0 then
        ui.text('Nothing matches', x, ry, {size = 12, color = 0xFFFF8080})
    end
    for row = 0, ROWS - 1 do
        local thing = results[p.top + row]
        if not thing then break end
        local label = L.name(p.kind, thing)
        local detail = k.detail and k.detail(thing)
        local sel = (p.top + row) == p.index
        if sel then ui.rect(x, ry + row * 21, w, 20, ui.rgba(240, 200, 110, 40)) end
        if ui.button('lookup-r:' .. row, x, ry + row * 21, w, 20, label .. (detail and ('   (' .. detail .. ')') or '')) then picked = thing end
    end
    if entered and results[p.index] then picked = results[p.index] end
    local foot = ry + ROWS * 21 + 4
    if #results > ROWS then ui.text(('%d matches: Up and Down scroll, or type more'):format(#results), x, foot, {size = 10, color = 0xFFB0B0B0}) end
    if ui.button('lookup-close', x + w - 70, foot - 2, 70, 20, opts.multi and 'Done' or 'Cancel') then close(p, not opts.multi) return end
    if picked then choose(p, picked) end
end

-- Opens the picker for `kind` ('item', 'creature', 'ability' or a registered kind). opts:
--   title        the window's title
--   text         the text the search starts with
--   filter(thing)  false leaves a thing out
--   select(thing)  called with the thing that was chosen; what it returns is the lookup's result (nil: the thing itself,
--                  false: refused, the picker stays open)
--   done(result, thing)  called with that result
--   multi        keep the picker open after a choice (for adding several things); the Done button closes it
--   closed(cancelled)    called when the picker is gone
function L.open(kind, opts)
    assert(kinds[kind], 'unknown lookup kind ' .. tostring(kind))
    opts = opts or {}
    if picker then close(picker, true) end
    picker = {kind = kind, opts = opts, text = opts.text or '', top = 1, index = 1}
    focus = 'picker'
    ui.show_panel(PANEL, true)
    if not drawing then
        drawing = true
        gear.on('draw', function() draw_picker() end)
    end
end

function L.is_open() return picker ~= nil end

function L.close() if picker then close(picker, true) end end

-- Inside a task (`gear.task`): opens the picker and waits for the choice. Returns the result of `select` (or the thing), nil when
-- the player cancelled.
function L.pick(kind, opts)
    opts = opts or {}
    local finished, value = false, nil
    local done, closed = opts.done, opts.closed
    local o = {}
    for k, v in pairs(opts) do o[k] = v end
    o.multi = false
    o.done = function(result, thing) value, finished = result, true if done then done(result, thing) end end
    o.closed = function(cancelled) finished = true if closed then closed(cancelled) end end
    L.open(kind, o)
    gear.wait_until(function() return finished end, 3600)
    return value
end

return L
