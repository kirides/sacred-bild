-- gear.fmt: numbers, times and texts made ready to show. Shipped inside Sacred Gear and run once per mod, in the mod's own
-- environment (so `gear` below is the mod's). It only uses the public API (gear.ui.text_size for fit_width).

local F = {}

-- 1234567 -> '1,234,567' (rounded to a whole number; `sep` is the separator, default ',').
function F.number(n, sep)
    n = math.floor((tonumber(n) or 0) + 0.5)
    sep = sep or ','
    local groups = {}
    local digits = tostring(math.abs(n))
    local head = #digits % 3
    if head > 0 then groups[1] = digits:sub(1, head) end
    for i = head + 1, #digits, 3 do groups[#groups + 1] = digits:sub(i, i + 2) end
    return (n < 0 and '-' or '') .. table.concat(groups, sep)
end

local function whole(seconds) return math.max(0, math.floor(tonumber(seconds) or 0)) end

-- Seconds as a clock: 3725 -> '1:02:05' (hours always shown).
function F.duration(seconds)
    local s = whole(seconds)
    return ('%d:%02d:%02d'):format(math.floor(s / 3600), math.floor(s / 60) % 60, s % 60)
end

-- Seconds as minutes and seconds: 125 -> '2:05' (minutes grow past 59).
function F.clock(seconds)
    local s = whole(seconds)
    return ('%d:%02d'):format(math.floor(s / 60), s % 60)
end

-- Seconds in words of the two biggest units: '2d 03h', '1h 02m', '3m 05s', '12s'.
function F.span(seconds)
    local s = whole(seconds)
    if s >= 86400 then return ('%dd %02dh'):format(math.floor(s / 86400), math.floor(s % 86400 / 3600)) end
    if s >= 3600 then return ('%dh %02dm'):format(math.floor(s / 3600), math.floor(s % 3600 / 60)) end
    if s >= 60 then return ('%dm %02ds'):format(math.floor(s / 60), s % 60) end
    return s .. 's'
end

-- The length in bytes of `text` cut to at most `n` bytes without splitting a UTF-8 character.
local function utf8_cut(text, n)
    if n >= #text then return #text end
    while n > 0 do
        local b = text:byte(n + 1)
        if not b or b < 0x80 or b >= 0xC0 then break end
        n = n - 1
    end
    return n
end

-- A text from outside made safe to show in one line: nil becomes '', control characters become spaces, and a text longer
-- than `max` characters (bytes) is cut with '...'.
function F.fit(text, max)
    text = tostring(text == nil and '' or text):gsub('%c', ' ')
    if max and #text > max then text = text:sub(1, utf8_cut(text, math.max(0, max - 3))) .. '...' end
    return text
end

-- `text` cut with '...' so that it is at most `width` wide in the font of `size` (gear.ui.text_size). The results are
-- cached (measuring is not free); whole UTF-8 characters only.
local fit_cache, fit_count = {}, 0
function F.fit_width(text, width, size)
    text = tostring(text or '')
    size = size or 12
    local key = text .. '\0' .. math.floor(width) .. '\0' .. size
    local hit = fit_cache[key]
    if hit then return hit end
    local ui = gear.ui
    local w = ui.text_size(text, {size = size})
    local out = text
    if w and w > width then
        local n = #text
        while n > 0 do
            n = utf8_cut(text, n - 1)
            out = text:sub(1, n) .. '...'
            local tw = ui.text_size(out, {size = size})
            if not tw or tw <= width then break end
        end
        if n == 0 then out = '...' end
    end
    fit_count = fit_count + 1
    if fit_count > 2000 then fit_cache, fit_count = {}, 0 end
    fit_cache[key] = out
    return out
end

-- The game's text markup taken out of a line (a tooltip line, a chat line): `\cAARRGGBB` is a colour, `\gAARRGGBB` plus 8
-- more characters (hex, space padded) an inline image, `\r` a reset, `\w`. Leading and trailing spaces go too. Returns nil
-- for an empty result, and also when other backslash codes remain (unknown markup), unless `keep_unknown` is true.
function F.plain(line, keep_unknown)
    line = tostring(line or '')
    line = line:gsub('\\g%x%x%x%x%x%x%x%x[%x ][%x ][%x ][%x ][%x ][%x ][%x ][%x ]', '')
    line = line:gsub('\\c%x%x%x%x%x%x%x%x', ''):gsub('\\[rw]', '')
    line = line:gsub('^%s+', ''):gsub('%s+$', '')
    if line == '' then return nil end
    if not keep_unknown and line:find('\\', 1, true) then return nil end
    return line
end

-- 1 -> '1 kill', 3 -> '3 kills' (`many` defaults to `one` .. 's'; the number with F.number).
function F.count(n, one, many)
    return F.number(n) .. ' ' .. ((tonumber(n) == 1) and one or (many or (one .. 's')))
end

return F
