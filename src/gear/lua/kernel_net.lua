-- Multiplayer (gear.mp). A player is a hero object: slots 1..16 are the heroes' object ids. See docs/GearMultiplayer.md.
local mp = {}
do
local player_mt = {}
player_mt.__index = player_mt
-- native.mp_info() -> {network, is_host, host_slot} (host_slot nil while it is not known).
local function mp_info()
    return native.mp_info and native.mp_info() or {network = false}
end
local function host_slot()
    local i = mp_info()
    if not i.network or i.is_host then return native.hero() or (not i.network and 1 or nil) end
    return i.host_slot
end
local function make_player(id)
    local o = native.object(id)
    if not (o and o.creature) then return nil end
    local cname = creature_mt.class({id = id})
    local name = native.hero_name and native.hero_name(id) or nil
    local p = {slot = id, class = cname, is_local = native.hero() == id, name = name or ('hero ' .. id)}
    p.key = p.name:lower() .. ':' .. (cname or '?')
    p.is_host = host_slot() == id
    return setmetatable(p, player_mt)
end
function player_mt:hero() return wrap(self.slot) end
-- A hero's name as the game shows it (nil for other creatures).
function creature_mt:name()
    if self.id < 1 or self.id > 16 then return nil end
    return native.hero_name and native.hero_name(self.id) or nil
end
function mp.players()
    local out = {}
    for id = 1, 16 do
        local p = make_player(id)
        if p then out[#out + 1] = p end
    end
    return out
end
function mp.player(slot)
    assert(type(slot) == 'number', 'slot must be a number')
    if slot < 1 or slot > 16 then return nil end
    return make_player(math.floor(slot))
end
function mp.local_player()
    local id = native.hero()
    return id and make_player(id) or nil
end
-- Whether other players' heroes are in the game (a network game with more than the local player).
function mp.active() return #mp.players() > 1 end
-- mp.mode() -> 'single' | 'lan' | 'open' | 'closed': the kind of game, from the engine's own flags (see World::networkMode).
function mp.mode() return native.mp_mode() end
function mp.local_slot() return native.hero() or (not mp_info().network and 1 or nil) end
function mp.host_slot() return host_slot() end
function mp.is_host()
    local i = mp_info()
    return not i.network or i.is_host == true
end

-- The mod channel. A message (kind 's' string or 'j' JSON, the channel's length and name, the body) is cut into frames
-- the native side sends as private chat (0x003) to the target slot: native.mp_send_frame(slot, utf16le) -> bool;
-- received frames come from native.mp_take_frame() -> slot, utf16le. A frame is UTF-16 units: the marker, the message
-- number, the fragment index, in the first fragment the total length (two units), then the bytes packed 15 bits per
-- unit; every unit but the marker is 0x4000 + a 15-bit value (no NUL, no surrogates). At most 127 units (the 254 bytes
-- the server relays): 228 bytes in the first frame, 232 in each further one.
local C = {
    MARKER = 0xE1A0, FIRST = 228, NEXT = 232, MAX_BODY = 65536, RATE = 8192, BURST = 2048, PER_TICK = 4,
    MAX_QUEUED = 512 * 1024, EXPIRE = 30, MAX_PARTIAL = 16,
    queue = {}, qhead = 1, qtail = 0, queued = 0, credit = 2048, number = 0, now = 0,
    loop = {}, partial = {}, force = false,
}
local N   -- the custom net messages (set below)
mp._channel = C   -- for the kernel tests (names starting with _ are not part of the API)

local band, bor, lshift, rshift = bit.band, bit.bor, bit.lshift, bit.rshift
local schar, sbyte = string.char, string.byte

local function units_to_le(units)
    local parts = {}
    for i = 1, #units do
        local u = units[i]
        parts[i] = schar(band(u, 0xFF), rshift(u, 8))
    end
    return table.concat(parts)
end
-- The bytes s[i .. j] packed 15 bits per unit, appended to `units`.
local function pack(s, i, j, units)
    local acc, n = 0, 0
    for k = i, j do
        acc = bor(lshift(acc, 8), sbyte(s, k))
        n = n + 8
        if n >= 15 then
            n = n - 15
            units[#units + 1] = 0x4000 + band(rshift(acc, n), 0x7FFF)
            acc = band(acc, lshift(1, n) - 1)
        end
    end
    if n > 0 then units[#units + 1] = 0x4000 + band(lshift(acc, 15 - n), 0x7FFF) end
end
-- `count` bytes from units[from ..], appended to `out`; false if the units run out first.
local function unpack_units(units, from, count, out)
    local acc, n, got = 0, 0, 0
    for k = from, #units do
        if got >= count then break end
        acc = bor(lshift(acc, 15), units[k] - 0x4000)
        n = n + 15
        while n >= 8 and got < count do
            n = n - 8
            got = got + 1
            out[#out + 1] = schar(band(rshift(acc, n), 0xFF))
        end
        acc = band(acc, lshift(1, n) - 1)
    end
    return got == count
end
function C.frame_count(len) return len <= C.FIRST and 1 or 1 + math.ceil((len - C.FIRST) / C.NEXT) end
-- The frames (UTF-16LE strings) of one encoded message.
function C.frames(data, number)
    local out, len, pos = {}, #data, 1
    for index = 0, C.frame_count(len) - 1 do
        local units = {C.MARKER, 0x4000 + number, 0x4000 + index}
        local size = index == 0 and C.FIRST or C.NEXT
        if index == 0 then
            units[4] = 0x4000 + rshift(len, 15)
            units[5] = 0x4000 + band(len, 0x7FFF)
        end
        pack(data, pos, math.min(pos + size - 1, len), units)
        pos = pos + size
        out[#out + 1] = units_to_le(units)
    end
    return out
end
function C.encode(channel, payload)
    if type(channel) ~= 'string' or not channel:match('^[%w_%.%-]+$') or #channel > 32 then
        return nil, 'channel must be 1 to 32 letters, digits, _ . -'
    end
    local kind, body = 's', payload
    if type(payload) == 'table' then
        local ok, text = pcall(json.encode, payload)
        if not ok then return nil, tostring(text) end
        kind, body = 'j', text
    elseif type(payload) ~= 'string' then
        return nil, 'payload must be a table or a string'
    end
    if #body > C.MAX_BODY then return nil, 'the message is larger than 64 KB' end
    return kind .. schar(#channel) .. channel .. body
end
function C.decode(data)
    local kind, n = data:sub(1, 1), sbyte(data, 2)
    if not n or n == 0 or #data < 2 + n then return nil end
    local channel, body = data:sub(3, 2 + n), data:sub(3 + n)
    -- s/j: gear.mp messages; n/k: gear.net channel messages on the chat carrier (string / JSON); q/a: the net handshake
    if kind == 's' or kind == 'n' or kind == 'q' or kind == 'a' then return channel, body, kind end
    if kind == 'j' or kind == 'k' then
        local v = json.decode(body)
        if v ~= nil then return channel, v, kind end
    end
end

function C.deliver(slot, data)
    local channel, payload, kind = C.decode(data)
    if not channel then
        log('gear.mp: a message from slot %s could not be read', tostring(slot))
        return
    end
    if kind == 'n' or kind == 'k' then return N.chat_message(slot, channel, payload) end
    if kind == 'q' or kind == 'a' then return N.chat_control(slot, kind, payload) end
    kernel.dispatch('mp_message', C.player_of(slot), channel, payload)
end

-- gear.mp.send(channel, payload, to) -> true | nil, message. `to`: a slot, a player, 'host' (default) or 'all'.
function mp.send(channel, payload, to)
    local data, err = C.encode(channel, payload)
    if not data then return nil, err end
    local me = native.hero()
    if not me then return nil, 'no game is running' end
    local network = mp_info().network
    if to == nil then to = 'host' end
    if type(to) == 'table' then to = to.slot end
    local targets = {}
    if to == 'host' then
        local h = host_slot()
        if not h then return nil, 'the host is not known yet' end
        targets[1] = h
    elseif to == 'all' then
        if network then
            for _, p in ipairs(mp.players()) do targets[#targets + 1] = p.slot end
        else
            targets[1] = me
        end
    elseif type(to) == 'number' and to >= 1 and to <= 16 and to == math.floor(to) then
        if to ~= me and not (network and mp.player(to)) then return nil, 'no such player' end
        targets[1] = to
    else
        return nil, "to must be a slot (1 to 16), a player, 'host' or 'all'"
    end
    local frames
    local waiting = 0
    for _, slot in ipairs(targets) do
        if slot ~= me or (C.force and network) then waiting = waiting + C.frame_count(#data) * 254 end
    end
    if C.queued + waiting > C.MAX_QUEUED then return nil, 'too many messages are waiting to be sent' end
    C.number = (C.number + 1) % 0x8000
    for _, slot in ipairs(targets) do
        if slot == me and not (C.force and network) then
            C.loop[#C.loop + 1] = data
        elseif slot ~= me and N.state == 'ready' and N.api.peer_supported(slot) and N.enqueue(N.mp_channel, data, 0x10 + slot) then
            C.via_net = (C.via_net or 0) + 1   -- the custom opcode carries it
        else
            frames = frames or C.frames(data, C.number)
            for _, f in ipairs(frames) do
                C.qtail = C.qtail + 1
                C.queue[C.qtail] = {slot, f}
                C.queued = C.queued + #f
            end
        end
    end
    return true
end
-- Debug: in a network game, messages to the local slot go through the real chat carrier instead of the loopback.
function mp.force_network(on) C.force = on and true or false end

function C.receive(slot, s)
    local units = {}
    for i = 1, #s - 1, 2 do units[#units + 1] = sbyte(s, i) + sbyte(s, i + 1) * 256 end
    if units[1] ~= C.MARKER or #units < 4 or #units > 127 then return end
    for i = 2, #units do
        if units[i] < 0x4000 or units[i] > 0xBFFF then return end
    end
    C.received = (C.received or 0) + 1
    local number, index = units[2] - 0x4000, units[3] - 0x4000
    local key = slot * 0x8000 + number
    local m = C.partial[key]
    if not m then
        local open, oldest = 0, nil
        for k, v in pairs(C.partial) do
            if v.slot == slot then
                open = open + 1
                if not oldest or v.at < C.partial[oldest].at then oldest = k end
            end
        end
        if open >= C.MAX_PARTIAL then C.partial[oldest] = nil end
        m = {slot = slot, at = C.now, parts = {}, got = 0}
        C.partial[key] = m
    end
    if index == 0 then
        if #units < 6 then C.partial[key] = nil return end
        m.len = (units[4] - 0x4000) * 0x8000 + (units[5] - 0x4000)
        if m.len > C.MAX_BODY + 64 then C.partial[key] = nil return end
        m.count = C.frame_count(m.len)
    end
    if not m.parts[index] then
        m.parts[index] = units
        m.got = m.got + 1
    end
    if m.count and index >= m.count then C.partial[key] = nil return end
    if not (m.count and m.got >= m.count) then return end
    C.partial[key] = nil
    local out = {}
    for i = 0, m.count - 1 do
        local first = i == 0
        local size = first and math.min(m.len, C.FIRST) or math.min(m.len - C.FIRST - (i - 1) * C.NEXT, C.NEXT)
        if not unpack_units(m.parts[i], first and 6 or 4, size, out) then return end
    end
    C.deliver(slot, table.concat(out))
end

-- Net channels (gear.net, docs/GearMultiplayer.md): channels with a name. Their messages ride on the chat carrier (frames
-- of kind n/k, to a player slot or to slot 0, the game server) and, when the server agrees, on two message types above
-- the stock range. The handshake is on the chat carrier too: the client announces itself ('h') on entering the game, the server
-- answers with a query 'q' with the numbers it offers, the client answers 'a' (accepts or declines), and on an accept sends HELLO on the agreed control type;
-- the server's HELLO_ACK (limits) and PEERS (who else can) complete it. A server that never queries (the original
-- GameServer.exe, other servers) never sees a custom type from us: channels stay on chat (to players; not to the server).
-- Native side: native.net_ops() -> control, data, ready; net_configure(control, data); net_send_op(type, bytes) -> bool;
-- net_take_op() -> type, sender slot, bytes | nil. Data frame: [u32 channel id (FNV-1a of the name)][u8 flags: 1 first,
-- 2 last, 4 JSON][u8 target: 0 server, 1 host, 0xFF all, 0x10 + slot][u16 message id][bytes], all little endian.
N = {
    control = 0x1E0, data = 0x1E1, state = 'idle', hello_at = 0, mask = 0, ack = nil, now = 0,
    FRAGMENT = 4000, MAX_MESSAGE = 262144, RATE = 65536, INFLIGHT = 8, HELLO_TIMEOUT = 3, EXPIRE = 10,
    MAX_PARTIAL_BYTES = 1048576, MAX_QUEUED = 1048576, PER_TICK = 16,
    channels = {}, partial = {}, partial_bytes = 0, queue = {}, qhead = 1, qtail = 0, queued = 0, credit = 0, number = 0,
    unknown = 0, dropped = 0, received = 0, sent = 0, supported = false, snapshot = '', server_chat = false, announces = 0, announce_at = 0,
}
mp._net = N   -- for the kernel tests
local net_api = {}
N.api = net_api
local channel_mt = {}
channel_mt.__index = channel_mt

local function fnv1a(s)
    local M = 4294967296
    local h = 0x811C9DC5
    for i = 1, #s do
        h = bit.bxor(h, sbyte(s, i)) % M
        h = (bit.lshift(h, 24) % M + h * 0x193) % M      -- h * 0x01000193 mod 2^32: the prime is 2^24 + 0x193
    end
    return h
end
N.fnv1a = fnv1a
local function u16(s, i) local a, b = sbyte(s, i, i + 1); return a + b * 256 end
local function u32(s, i) local a, b, c, d = sbyte(s, i, i + 3); return a + b * 256 + c * 65536 + d * 16777216 end
local function p16(v) return schar(band(v, 255), band(rshift(v, 8), 255)) end
local function p32(v) return schar(band(v, 255), band(rshift(v, 8), 255), band(rshift(v, 16), 255), band(rshift(v, 24), 255)) end

local function player_of(slot)
    return slot == 0 and {slot = 0, name = 'server', key = 'server', is_local = false, is_host = false}
        or mp.player(slot)
        or setmetatable({slot = slot, name = 'hero ' .. slot, key = '?', is_local = false, is_host = host_slot() == slot}, player_mt)
end
C.player_of = player_of

local function caps_changed()
    local peers = {}
    for slot = 1, 16 do if band(rshift(N.mask, slot - 1), 1) == 1 then peers[#peers + 1] = slot end end
    local supported = N.state == 'ready'
    local snapshot = (supported and table.concat(peers, ',') or '') .. (N.server_chat and '+chat' or '')
    if supported ~= N.supported or snapshot ~= N.snapshot then
        N.supported, N.snapshot = supported, snapshot
        kernel.dispatch('net_caps', {supported = supported, peers = peers, chat = N.server_chat})
    end
end

local function net_reset()
    N.state, N.mask, N.ack, N.server_chat = 'idle', 0, nil, false
    N.announces, N.announce_at = 0, 0
    N.partial, N.partial_bytes = {}, 0
    N.queue, N.qhead, N.qtail, N.queued = {}, 1, 0, 0
    caps_changed()
end

-- gear.net.configure({control = 0x1E0, data = 0x1E1}): the custom message types (for servers with opcodes of their own);
-- before the game connects. The data type is told by the server in the handshake as well.
function net_api.configure(opts)
    if type(opts) ~= 'table' then return nil, 'opts must be a table' end
    if N.state ~= 'idle' then return nil, 'the handshake has already started' end
    local c, d = opts.control or N.control, opts.data or N.data
    if not (native.net_configure and native.net_configure(c, d)) then return nil, 'invalid message types (above 0x1D5, below 0x400, different)' end
    N.control, N.data = c, d
    N.preferred = {c, d}   -- the handshake accepts only these numbers from a server
    return true
end

function net_api.supported() return N.state == 'ready' end
function net_api.peer_supported(slot)
    if N.state ~= 'ready' or type(slot) ~= 'number' or slot < 1 or slot > 16 or slot ~= math.floor(slot) then return false end
    return band(rshift(N.mask, slot - 1), 1) == 1
end
function net_api.limits()
    if N.state ~= 'ready' then return nil end
    return {max_fragment = N.FRAGMENT, max_message = N.MAX_MESSAGE, bytes_per_second = N.RATE, in_flight = N.INFLIGHT}
end

function net_api.channel(name, opts)
    if type(name) ~= 'string' or #name < 1 or #name > 64 or name:find('[^\32-\126]') then
        return nil, 'a channel name is 1 to 64 printable ASCII characters'
    end
    local id = fnv1a(name)
    local c = N.channels[id]
    if c then
        if c.name ~= name then return nil, ('channel %q has the same id as %q: rename one'):format(name, c.name) end
        return c
    end
    c = setmetatable({name = name, id = id, handlers = {}}, channel_mt)
    N.channels[id] = c
    return c
end

-- ch:on(fn): fn(from, payload) for every message of the channel; fn is returned (give it to ch:off).
function channel_mt:on(fn)
    assert(type(fn) == 'function', 'fn must be a function')
    self.handlers[#self.handlers + 1] = fn
    return fn
end
function channel_mt:off(fn)
    for i, h in ipairs(self.handlers) do
        if h == fn then table.remove(self.handlers, i) return true end
    end
    return false
end

-- Queues one message (string, or a table as JSON) as fragments; `target` is the wire byte. true | nil, message.
function N.enqueue(c, payload, target)
    if N.state ~= 'ready' then return nil, 'custom net messages are not available here (the server does not support them)' end
    local flags, body = 0, payload
    if type(payload) == 'table' then
        local ok, text = pcall(json.encode, payload)
        if not ok then return nil, tostring(text) end
        flags, body = 4, text
    elseif type(payload) ~= 'string' then
        return nil, 'payload must be a table or a string'
    end
    if #body > N.MAX_MESSAGE then return nil, ('the message is larger than %d bytes'):format(N.MAX_MESSAGE) end
    if N.queued + #body > N.MAX_QUEUED then return nil, 'too many messages are waiting to be sent' end
    N.number = (N.number + 1) % 65536
    N.qtail = N.qtail + 1
    N.queue[N.qtail] = {c = c, flags = flags, target = target, id = N.number, body = body, pos = 1}
    N.queued = N.queued + #body
    return true
end

-- One frame group of the chat carrier: kind, name and body as one message to `slot` (0: the game server).
local function chat_frame(slot, kind, name, body)
    if #body > C.MAX_BODY then return nil, 'the message is larger than 64 KB on the chat carrier' end
    local data = kind .. schar(#name) .. name .. body
    if C.queued + C.frame_count(#data) * 254 > C.MAX_QUEUED then return nil, 'too many messages are waiting to be sent' end
    C.number = (C.number + 1) % 0x8000
    for _, f in ipairs(C.frames(data, C.number)) do
        C.qtail = C.qtail + 1
        C.queue[C.qtail] = {slot, f}
        C.queued = C.queued + #f
        C.last_entry = C.queue[C.qtail]
    end
    return true
end
-- A message on a net channel through the chat carrier (n: bytes, k: JSON).
function N.chat_send(slot, name, payload)
    if type(payload) == 'table' then
        local ok, text = pcall(json.encode, payload)
        if not ok then return nil, tostring(text) end
        return chat_frame(slot, 'k', name, text)
    elseif type(payload) ~= 'string' then
        return nil, 'payload must be a table or a string'
    end
    return chat_frame(slot, 'n', name, payload)
end

-- How a message to `to` would travel: a list of steps {via = 'custom' | 'chat', slot, target}, or nil and why.
local function plan(to)
    if to == nil then to = 'host' end
    if type(to) == 'table' then to = to.slot end
    local me = native.hero()
    if to == 'server' then
        if N.state == 'ready' then return {{via = 'custom', slot = 0, target = 0}} end
        if N.server_chat then return {{via = 'chat', slot = 0}} end
        return nil, 'the server does not support net channels'
    end
    local slots
    if to == 'host' then
        local h = host_slot()
        if not h then return nil, 'the host is not known yet' end
        if h == me then return nil, 'a net message cannot go to this computer' end
        slots = {h}
    elseif to == 'all' then
        slots = {}
        for _, p in ipairs(mp.players()) do if p.slot ~= me then slots[#slots + 1] = p.slot end end
    elseif type(to) == 'number' and to >= 1 and to <= 16 and to == math.floor(to) then
        if to == me then return nil, 'a net message cannot go to this computer' end
        slots = {to}
    else
        return nil, "to must be 'server', 'host', 'all', a slot (1 to 16) or a player"
    end
    local custom, every = N.state == 'ready', N.state == 'ready'
    for _, s in ipairs(slots) do
        if not (custom and net_api.peer_supported(s)) then every = false end
    end
    if to == 'all' and every and #slots > 0 then return {{via = 'custom', target = 0xFF}} end
    local steps = {}
    for _, s in ipairs(slots) do
        if custom and net_api.peer_supported(s) then steps[#steps + 1] = {via = 'custom', slot = s, target = to == 'host' and 1 or 0x10 + s}
        else steps[#steps + 1] = {via = 'chat', slot = s} end
    end
    return steps
end

-- ch:send(payload, to) -> true | nil, message. `to`: 'server', 'host' (default), 'all', a slot or a player. Over the custom
-- message types where both sides support them, over the chat carrier otherwise.
function channel_mt:send(payload, to)
    local steps, err = plan(to)
    if not steps then return nil, err end
    for i, st in ipairs(steps) do
        local ok, e
        if st.via == 'custom' then ok, e = N.enqueue(self, payload, st.target) else ok, e = N.chat_send(st.slot, self.name, payload) end
        if not ok and i == 1 then return nil, e end
    end
    return true
end

-- 'custom', 'chat', 'mixed' (some players one way, some the other) or nil (not possible): the transport a message to `to`
-- would use now.
function net_api.transport(to)
    local steps = plan(to)
    if not steps or #steps == 0 then return nil end
    local custom = 0
    for _, st in ipairs(steps) do if st.via == 'custom' then custom = custom + 1 end end
    return custom == #steps and 'custom' or custom == 0 and 'chat' or 'mixed'
end
function channel_mt:transport(to) return net_api.transport(to) end

local function net_deliver(ch, slot, json_flag, body)
    local payload = body
    if json_flag then
        payload = json.decode(body)
        if payload == nil then N.dropped = N.dropped + 1 return end
    end
    N.dispatch_message(ch, slot, payload)
end
function N.dispatch_message(ch, slot, payload)
    local from = player_of(slot)
    for _, fn in ipairs({unpack(ch.handlers)}) do
        local ok, err = pcall(fn, from, payload)
        if not ok then log('gear.net: handler of %s failed: %s', ch.name, tostring(err)) end
    end
end

-- A channel message that came on the chat carrier (payload already a string or a table).
function N.chat_message(slot, name, payload)
    local ch = N.channels[fnv1a(name)]
    if not ch or ch.name ~= name then N.unknown = N.unknown + 1 return end
    N.received = N.received + 1
    N.dispatch_message(ch, slot, payload)
end

-- The handshake on the chat carrier: only the server's query 'q' [u8 proto][u16 control][u16 data] is acted on; the answer 'a'
-- [u8 proto][u16 control][u16 data][u8 flags: 1 accepts the custom types] goes back over chat, then HELLO follows on the
-- agreed control type once the answer has left (poll).
function N.chat_control(slot, kind, body)
    if slot ~= 0 or kind ~= 'q' or #body < 5 or N.state ~= 'idle' then return end
    local proto, control, data = sbyte(body, 1), u16(body, 2), u16(body, 4)
    N.server_chat = true
    local accept = proto >= 1 and native.net_configure ~= nil and (not N.preferred or (N.preferred[1] == control and N.preferred[2] == data))
        and native.net_configure(control, data) == true
    if accept then N.control, N.data = control, data end
    chat_frame(0, 'a', 'gear.net', schar(1) .. p16(N.control) .. p16(N.data) .. schar(accept and 1 or 0))
    if accept then N.state, N.hello_wait = 'answered', C.last_entry else N.state = 'chat' end
    log('gear.net: the server offered custom message types %X/%X: %s', control, data, accept and 'accepted' or 'declined')
    caps_changed()
end

local function drop_partial(key)
    local m = N.partial[key]
    if m then N.partial_bytes = N.partial_bytes - m.size; N.partial[key] = nil end
end

local function net_receive(slot, s)
    if #s < 8 then return end
    local ch = N.channels[u32(s, 1)]
    if not ch then N.unknown = N.unknown + 1 return end
    local flags, id = sbyte(s, 5), u16(s, 7)
    local first, last, js = band(flags, 1) ~= 0, band(flags, 2) ~= 0, band(flags, 4) ~= 0
    local body = s:sub(9)
    N.received = N.received + 1
    if first and last then
        if #body <= N.MAX_MESSAGE then net_deliver(ch, slot, js, body) end
        return
    end
    local key = slot * 65536 + id
    local m = N.partial[key]
    if first then
        drop_partial(key)
        local open, oldest = 0, nil
        for k, v in pairs(N.partial) do
            if v.slot == slot then
                open = open + 1
                if not oldest or v.at < N.partial[oldest].at then oldest = k end
            end
        end
        if open >= N.INFLIGHT then drop_partial(oldest) end
        m = {slot = slot, ch = ch, json = js, parts = {}, size = 0, at = N.now}
        N.partial[key] = m
    elseif not m or m.ch ~= ch then
        return
    end
    m.size = m.size + #body
    N.partial_bytes = N.partial_bytes + #body
    m.parts[#m.parts + 1] = body
    m.at = N.now
    if m.size > N.MAX_MESSAGE or N.partial_bytes > N.MAX_PARTIAL_BYTES then drop_partial(key) N.dropped = N.dropped + 1 return end
    if last then
        N.partial[key] = nil
        N.partial_bytes = N.partial_bytes - m.size
        net_deliver(ch, slot, m.json, table.concat(m.parts))
    end
end
N.receive = net_receive

local function net_control(s)
    local kind = sbyte(s, 1)
    if kind == 2 and #s >= 19 and (N.state == 'hello' or N.state == 'ready') then
        N.ack = {protocol = sbyte(s, 2), features = u32(s, 3), data = u16(s, 7), fragment = u16(s, 9), message = u32(s, 11),
            rate = u32(s, 15), in_flight = sbyte(s, 19)}
        local a = N.ack
        N.FRAGMENT = math.max(64, math.min(a.fragment, 4000))
        N.MAX_MESSAGE = a.message
        N.RATE = math.max(1024, a.rate)
        N.INFLIGHT = math.max(1, a.in_flight)
        if a.data ~= N.data and native.net_configure and native.net_configure(N.control, a.data) then N.data = a.data end
        N.state = 'ready'
        N.credit = N.RATE
        log('gear.net: the server supports custom net messages (fragment %d, message %d, %d B/s)', N.FRAGMENT, N.MAX_MESSAGE, N.RATE)
        caps_changed()
    elseif kind == 3 and #s >= 3 and N.state == 'ready' then
        N.mask = u16(s, 2)
        caps_changed()
    end
end

-- Every tick: the handshake, frames in, frames out (paced to the server's announced rate), stale partial messages.
function N.poll(dt)
    N.now = N.now + (dt or 0)
    if not native.net_ops then return end
    local _, _, ready = native.net_ops()
    local info = mp_info()
    if not ready then
        if N.state ~= 'idle' or N.server_chat then net_reset() end
        return
    end
    -- A Gear client announces itself to the server over chat on entering the game (private chat to slot 0, harmless to servers that
    -- do not know it); only a server that heard it writes channel frames to this client, which plain clients would show as chat.
    if N.state == 'idle' and not N.server_chat and info.network and native.hero() and N.announces < 6 and N.now >= N.announce_at then
        N.announces, N.announce_at = N.announces + 1, N.now + 5
        chat_frame(0, 'h', 'gear.net', schar(1))
    end
    if N.state == 'answered' and (not N.hello_wait or N.hello_wait.sent) then
        local hash = kernel.mod_set_hash and tonumber(kernel.mod_set_hash(), 16) or 0
        if native.net_send_op(N.control, schar(1, 1) .. p32(3) .. p32(hash)) then N.state, N.hello_at = 'hello', N.now end
    elseif N.state == 'hello' and N.now - N.hello_at > N.HELLO_TIMEOUT then
        N.state = 'unsupported'
        log('gear.net: no answer to HELLO: the custom message types stay off, the chat carrier is used')
    end
    for _ = 1, 64 do
        local t, slot, s = native.net_take_op()
        if not t then break end
        if t == N.control then net_control(s) elseif t == N.data and N.state == 'ready' then net_receive(slot, s) end
    end
    for k, m in pairs(N.partial) do
        if N.now - m.at > N.EXPIRE then drop_partial(k) end
    end
    if N.state ~= 'ready' then return end
    N.credit = math.min(N.credit + N.RATE * (dt or 0), N.RATE * 2)
    local sent = 0
    while N.qhead <= N.qtail and sent < N.PER_TICK and N.credit >= 0 do
        local m = N.queue[N.qhead]
        local len = #m.body
        local take = math.min(N.FRAGMENT, len - m.pos + 1)
        local flags = m.flags + (m.pos == 1 and 1 or 0) + (m.pos + take > len and 2 or 0)
        local frame = p32(m.c.id) .. schar(flags, m.target) .. p16(m.id) .. m.body:sub(m.pos, m.pos + take - 1)
        if not native.net_send_op(N.data, frame) then N.dropped = N.dropped + 1 else N.sent = N.sent + 1 end
        N.credit = N.credit - #frame - 20
        sent = sent + 1
        m.pos = m.pos + take
        if m.pos > len then
            N.queue[N.qhead] = nil
            N.qhead = N.qhead + 1
            N.queued = N.queued - len
        end
    end
    if N.qhead > N.qtail then N.queue, N.qhead, N.qtail = {}, 1, 0 end
end

-- gear.mp.send travels on this channel when the target can use it: the encoded message as raw bytes
N.mp_channel = net_api.channel('gear.mp')
N.mp_channel:on(function(from, data) C.deliver(from.slot, data) end)

-- Every tick: the loopback messages of the last tick, the frames that came in, the frames to send (paced: about
-- 8 KB/s and at most 4 frames a tick, so the game's send queue never floods).
function kernel.poll_mp(dt)
    N.poll(dt)
    C.now = C.now + (dt or 0)
    if #C.loop > 0 then
        local list = C.loop
        C.loop = {}
        local me = native.hero()
        for _, data in ipairs(list) do C.deliver(me, data) end
    end
    if native.mp_take_frame then
        for _ = 1, 64 do
            local slot, s = native.mp_take_frame()
            if not slot then break end
            C.receive(slot, s)
        end
    end
    for k, m in pairs(C.partial) do
        if C.now - m.at > C.EXPIRE then C.partial[k] = nil end
    end
    C.credit = math.min(C.credit + C.RATE * (dt or 0), C.BURST)
    local sent = 0
    while C.qhead <= C.qtail and sent < C.PER_TICK and C.credit >= 0 do
        local f = C.queue[C.qhead]
        f.sent = true
        -- false: the game cannot send it (the player left, the connection is gone): the frame is dropped
        if not (native.mp_send_frame and native.mp_send_frame(f[1], f[2])) then C.dropped = (C.dropped or 0) + 1 end
        C.queue[C.qhead] = nil
        C.qhead = C.qhead + 1
        C.queued = C.queued - #f[2]
        C.credit = C.credit - #f[2] - 20
        sent = sent + 1
    end
    if C.qhead > C.qtail then C.queue, C.qhead, C.qtail = {}, 1, 0 end
end
end

