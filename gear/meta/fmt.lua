---@meta _
-- Sacred Gear: formatting numbers, times and texts (gear.fmt).

-- ## Formatting (gear.fmt)
--: Small helpers that many mods need for their panels: a number with thousands separators, seconds as a clock or in words, a
--: text from outside cut to a length (by characters or by its width on screen), the game's colour and image markup taken out
--: of a line. The library is shipped in the DLL (`gear/lua/fmt.lua`) and runs once per mod, the first time the mod uses
--: `gear.fmt`.

-- path gear.fmt
--- Numbers, times and texts made ready to show.
---@class GearFmt
local GearFmt = {}

--- A whole number with thousands separators: `1234567` -> `'1,234,567'` (rounded; nil and non-numbers count as 0).
---@param n number
---@param sep? string The separator (default `','`).
---@return string
function GearFmt.number(n, sep) end

--- Seconds as a clock with hours: `3725` -> `'1:02:05'`.
---@param seconds number
---@return string
function GearFmt.duration(seconds) end

--- Seconds as minutes and seconds: `125` -> `'2:05'` (the minutes grow past 59).
---@param seconds number
---@return string
function GearFmt.clock(seconds) end

--- Seconds in the two biggest units: `'2d 03h'`, `'1h 02m'`, `'3m 05s'`, `'12s'`.
---@param seconds number
---@return string
function GearFmt.span(seconds) end

--- A text for one line: nil becomes `''`, control characters become spaces, and a text longer than `max` bytes is cut with
--- `'...'` (never inside a UTF-8 character). Use it for texts that come from a server or another player.
---@param text any
---@param max? integer Without it the text is not cut.
---@return string
function GearFmt.fit(text, max) end

--- `text` cut with `'...'` so that it is at most `width` wide in the font of `size` (measured with `gear.ui.text_size`,
--- cached).
---@param text string
---@param width number
---@param size? number Default 12.
---@return string
function GearFmt.fit_width(text, width, size) end

--- A line without the game's text markup (`\cAARRGGBB` colours, `\g...` inline images, `\r`, `\w`) and without leading and
--- trailing spaces. nil when nothing is left, and when other backslash codes remain (unless `keep_unknown`).
---@param line string
---@param keep_unknown? boolean
---@return string?
function GearFmt.plain(line, keep_unknown) end

--- A number and a noun: `(1, 'kill')` -> `'1 kill'`, `(3, 'kill')` -> `'3 kills'`.
---@param n number
---@param one string
---@param many? string Default `one .. 's'`.
---@return string
function GearFmt.count(n, one, many) end
