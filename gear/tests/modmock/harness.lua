-- A mock of the gear API (only names the docs list); a call of anything else fails loudly.
T = { now = 100, files = {}, handlers = {}, keys = {}, timers = {}, sent = {}, channels = {}, clicks = {}, picks = {}, texts = {}, settings = {}, supported = true,
  panel_open = {}, calls = {}, hero_items = {}, ground = {} }
local function strict(name, t)
  return setmetatable(t, { __index = function(_, k) error('gear API: no ' .. name .. '.' .. tostring(k), 2) end })
end
local ui = strict('ui', {})
ui.COLORS = { accent = 1, text = 2 }
ui.rgba = function(r, g, b, a) return r * 65536 + g * 256 + b end
ui.rect = function() return true end
ui.frame = function() return true end
ui.text = function(text, x, y, opts) T.texts[#T.texts + 1] = tostring(text); return true end
ui.label = function(id, text, x, y, opts) T.texts[#T.texts + 1] = tostring(text); return true end
ui.button = function(id, x, y, w, h, text, opts)
  T.texts[#T.texts + 1] = '[' .. tostring(text) .. ']'
  if T.clicks[id] then T.clicks[id] = nil; return true end
  return false
end
ui.checkbox = function(id, x, y, text, checked, opts) return false, checked end
ui.radio_group = function(id, x, y, options, index, opts)
  local pick = T.picks[id]
  if pick then T.picks[id] = nil; return true, pick end
  return false, index
end
ui.dropdown = function(id, x, y, w, options, index, opts)
  local pick = T.picks[id]
  if pick then T.picks[id] = nil; return true, pick end
  return false, index
end
ui.scroll_begin = function(id, x, y, w, h, opts) return { x = x, y = y, w = w, h = h, offset = 0, max = 0, content_x = x, content_y = y } end
ui.scroll_end = function() return 0, 0 end
ui.text_wrap = function(text, width, opts) return { tostring(text) }, 16 end
ui.panel = function(id, title, x, y, w, h, opts)
  if not T.panel_open[id] then return { open = false } end
  return { open = true, x = x, y = y, w = w, h = h, content_x = x + 8, content_y = y + 24, content_w = w - 16, content_h = h - 32 }
end
ui.show_panel = function(id, open) T.panel_open[id] = open end
ui.panel_open = function(id) return T.panel_open[id] == true end
gear = strict('gear', {
  config = { page_size = 12 }, ui = ui,
  settings = strict('settings', { get = function(id) local v = T.settings[id]; return v end }),
  keys = strict('keys', { on = function(id, fn) T.keys[id] = fn end }),
  time = function() return T.now end,
  after = function(s, fn) T.timers[#T.timers + 1] = { at = T.now + s, fn = fn } end,
  every = function(s, fn) T.timers[#T.timers + 1] = { at = T.now + s, every = s, fn = fn } end,
  read = function(name) return T.files[name] end,
  save = function(name, text) T.files[name] = text; return true end,
  json = strict('json', { encode = function(v) T.encoded = v; return 'JSON:' .. tostring(v) end, decode = function(t) return T.encoded end }),
  on = function(event, fn) T.handlers[event] = T.handlers[event] or {}; table.insert(T.handlers[event], fn) end,
})
local net = strict('net', {})
net.supported = function() return T.supported end
net.channel = function(name)
  local ch = { name = name, handlers = {} }
  ch.on = function(self, fn) table.insert(self.handlers, fn) return fn end
  ch.send = function(self, payload, to)
    if not T.supported then return nil, 'custom net messages are not available here' end
    T.sent[#T.sent + 1] = { channel = name, payload = payload, to = to }
    return true
  end
  T.channels[name] = ch
  return ch
end
gear.net = net
local hero = {}
function hero.level() return 41 end
function hero.class() return 'battlemage', 3 end
function hero.name() return 'Alice' end
function hero.position() return 1000, 1000, 0 end
gear.world = strict('world', { TILE = 53.7, hero = function() return hero end,
  items_near = function(x, y, r, o) return T.ground end })
gear.inv = strict('inv', { container = function(w) return { items = function() return T.hero_items end } end })

function fire(event, ...) for _, fn in ipairs(T.handlers[event] or {}) do fn(...) end end
function frame() T.texts = {}; fire('draw', { mode = 'game' }) end
function tick(seconds)
  T.now = T.now + seconds
  for _, t in ipairs(T.timers) do if t.at <= T.now then t.fn(); if t.every then t.at = T.now + t.every else t.at = 1e18 end end end
end
function server_says(channel, payload, slot) for _, fn in ipairs(T.channels[channel].handlers) do fn({ slot = slot or 0 }, payload) end end
function has_text(needle)
  for _, t in ipairs(T.texts) do if t:find(needle, 1, true) then return true end end
  return false
end
function expect(cond, msg) if not cond then error('EXPECT FAILED: ' .. msg, 2) end print('pass: ' .. msg) end
