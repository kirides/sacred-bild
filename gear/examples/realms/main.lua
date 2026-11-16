-- The realm picker: a page of this mod in the mod manager (F11, or LT+LB on a pad: Mods > Realm > More). It lists the
-- realms and makes one the online play server: the next time Closed or Open Internet is opened in the multiplayer
-- menu, register and login go to that realm (LAN is not touched). It binds no key and uses only Gear widgets, so the
-- pad and the mouse work the same.
--
-- Add realms from a mod with gear.realm.add{id = ..., name = ..., host = ..., port = ...}, or edit mods\gear_realms.json:
--     {"version": 1, "realms": [{"id": "private", "name": "Private realm", "host": "realm.example.com", "port": 7066}]}

local ui = gear.ui
local C = ui.COLORS

local message
local function note(text) message = text end

gear.mods.page(function(x, y, w, h)
    local realms = gear.realm.list()
    local current = gear.realm.current()
    ui.text('Online play server (Closed / Open Internet)', x, y, {color = C.text})
    local names, index = {}, 1
    for i, r in ipairs(realms) do
        names[i] = r.name
        if r.id == current.id then index = i end
    end
    local line = y + 24
    ui.text('Realm', x, line, {color = C.text_dim, height = 18, valign = 'middle'})
    local changed, picked = ui.dropdown('realm', x + 60, line, math.min(w - 70, 320), names, index,
        {tooltip = 'Register and login of the next Closed / Open Internet go to this server'})
    if changed then
        local ok, why = gear.realm.use(realms[picked].id)
        note(ok and ('Realm: ' .. realms[picked].name) or ('Not changed: ' .. tostring(why)))
    end
    local r = realms[index]
    ui.text('Host: ' .. (r.host ~= '' and r.host or '(from Settings.cfg)') .. (r.port and (':' .. r.port) or ''),
        x, line + 30, {color = C.text_dim})
    if message then ui.text(message, x, line + 52, {color = C.text_dim}) end
    return 100
end)
