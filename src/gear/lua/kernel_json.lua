---------------------------------------------------------------------------------------------------------------
-- JSON (gear.json): encode(value) -> text; decode(text) -> value | nil, message. Objects are tables with string
-- keys, arrays tables with 1..n; json.null stands for null (decode gives it for null values in arrays and objects).
---------------------------------------------------------------------------------------------------------------
local json = {null = setmetatable({}, {__tostring = function() return 'null' end, __metatable = false})}

do
    local escapes = {['"'] = '\\"', ['\\'] = '\\\\', ['\b'] = '\\b', ['\f'] = '\\f', ['\n'] = '\\n',
                     ['\r'] = '\\r', ['\t'] = '\\t'}
    local function escape(s)
        return '"' .. s:gsub('[%c"\\]', function(c)
            return escapes[c] or string.format('\\u%04x', c:byte())
        end) .. '"'
    end

    local encode
    local function is_array(t)
        local n = 0
        for k in pairs(t) do
            if type(k) ~= 'number' or k < 1 or k % 1 ~= 0 then return false end
            if k > n then n = k end
        end
        return n == #t, n
    end

    function encode(v, depth)
        depth = depth or 0
        if depth > 64 then error('json: nested too deeply', 0) end
        local t = type(v)
        if v == nil or v == json.null then return 'null'
        elseif t == 'boolean' then return tostring(v)
        elseif t == 'number' then
            if v ~= v or v == math.huge or v == -math.huge then error('json: cannot encode ' .. tostring(v), 0) end
            if v % 1 == 0 and v > -1e15 and v < 1e15 then return string.format('%d', v) end
            return string.format('%.17g', v)
        elseif t == 'string' then return escape(v)
        elseif t == 'table' then
            local array, n = is_array(v)
            local parts = {}
            if array and (n > 0 or next(v) == nil) then
                for i = 1, n do parts[i] = encode(v[i], depth + 1) end
                return '[' .. table.concat(parts, ',') .. ']'
            end
            local keys = {}
            for k in pairs(v) do
                if type(k) ~= 'string' then error('json: object keys must be strings', 0) end
                keys[#keys + 1] = k
            end
            table.sort(keys)
            for i, k in ipairs(keys) do parts[i] = escape(k) .. ':' .. encode(v[k], depth + 1) end
            return '{' .. table.concat(parts, ',') .. '}'
        end
        error('json: cannot encode a ' .. t, 0)
    end

    function json.encode(v)
        local ok, result = pcall(encode, v)
        if not ok then error(result, 2) end
        return result
    end

    local function utf8(cp)
        if cp < 0x80 then return string.char(cp)
        elseif cp < 0x800 then return string.char(0xC0 + math.floor(cp / 0x40), 0x80 + cp % 0x40)
        elseif cp < 0x10000 then
            return string.char(0xE0 + math.floor(cp / 0x1000), 0x80 + math.floor(cp / 0x40) % 0x40, 0x80 + cp % 0x40)
        end
        return string.char(0xF0 + math.floor(cp / 0x40000), 0x80 + math.floor(cp / 0x1000) % 0x40,
                           0x80 + math.floor(cp / 0x40) % 0x40, 0x80 + cp % 0x40)
    end

    function json.decode(text)
        if type(text) ~= 'string' then return nil, 'json: text must be a string' end
        local pos = 1
        local function fail(msg) error({msg = 'json: ' .. msg .. ' at ' .. pos}, 0) end
        local function ws() pos = text:find('[^ \t\r\n]', pos) or #text + 1 end
        local value
        local function str()
            pos = pos + 1
            local parts = {}
            while true do
                local a, b = text:find('[^"\\]*', pos)
                parts[#parts + 1] = text:sub(a, b)
                pos = b + 1
                local c = text:sub(pos, pos)
                if c == '"' then pos = pos + 1; return table.concat(parts) end
                if c == '' then fail('unterminated string') end
                local e = text:sub(pos + 1, pos + 1)
                if e == 'u' then
                    local hex = text:sub(pos + 2, pos + 5)
                    if not hex:match('^%x%x%x%x$') then fail('bad \\u escape') end
                    local cp = tonumber(hex, 16)
                    pos = pos + 6
                    if cp >= 0xD800 and cp < 0xDC00 and text:sub(pos, pos + 1) == '\\u' then
                        local low = tonumber(text:sub(pos + 2, pos + 5), 16)
                        if low and low >= 0xDC00 and low < 0xE000 then
                            cp = 0x10000 + (cp - 0xD800) * 0x400 + (low - 0xDC00)
                            pos = pos + 6
                        end
                    end
                    parts[#parts + 1] = utf8(cp)
                else
                    local map = {['"'] = '"', ['\\'] = '\\', ['/'] = '/', b = '\b', f = '\f', n = '\n', r = '\r', t = '\t'}
                    if not map[e] then fail('bad escape') end
                    parts[#parts + 1] = map[e]
                    pos = pos + 2
                end
            end
        end
        local depth = 0
        function value()
            ws()
            local c = text:sub(pos, pos)
            if c == '{' then
                depth = depth + 1
                if depth > 64 then fail('nested too deeply') end
                pos = pos + 1
                local obj = {}
                ws()
                if text:sub(pos, pos) == '}' then pos = pos + 1; depth = depth - 1; return obj end
                while true do
                    ws()
                    if text:sub(pos, pos) ~= '"' then fail('expected a key') end
                    local k = str()
                    ws()
                    if text:sub(pos, pos) ~= ':' then fail("expected ':'") end
                    pos = pos + 1
                    obj[k] = value()
                    ws()
                    local d = text:sub(pos, pos)
                    pos = pos + 1
                    if d == '}' then depth = depth - 1; return obj end
                    if d ~= ',' then fail("expected ',' or '}'") end
                end
            elseif c == '[' then
                depth = depth + 1
                if depth > 64 then fail('nested too deeply') end
                pos = pos + 1
                local arr = {}
                ws()
                if text:sub(pos, pos) == ']' then pos = pos + 1; depth = depth - 1; return arr end
                while true do
                    arr[#arr + 1] = value()
                    ws()
                    local d = text:sub(pos, pos)
                    pos = pos + 1
                    if d == ']' then depth = depth - 1; return arr end
                    if d ~= ',' then fail("expected ',' or ']'") end
                end
            elseif c == '"' then return str()
            elseif text:sub(pos, pos + 3) == 'true' then pos = pos + 4; return true
            elseif text:sub(pos, pos + 4) == 'false' then pos = pos + 5; return false
            elseif text:sub(pos, pos + 3) == 'null' then pos = pos + 4; return json.null
            end
            local num = text:match('^-?%d+%.?%d*[eE]?[-+]?%d*', pos)
            if not num or num == '' or num == '-' then fail('unexpected character') end
            pos = pos + #num
            local n = tonumber(num)
            if not n then fail('bad number') end
            return n
        end
        local ok, result = pcall(function()
            local v = value()
            ws()
            if pos <= #text then fail('trailing text') end
            return v
        end)
        if ok then return result end
        if type(result) == 'table' then return nil, result.msg end
        return nil, 'json: ' .. tostring(result)
    end
end

