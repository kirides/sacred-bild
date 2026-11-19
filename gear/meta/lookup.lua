---@meta _
-- Sacred Gear: lookups, finding a thing of the game by a few letters (gear.lookup).

-- ## Lookups (gear.lookup)
--: A lookup finds one "thing" in a list of things of one kind: equipment items (named as the game's tooltip names them,
--: in the player's language), creatures, abilities, or a kind a mod registers. The search is forgiving: several words in
--: any order, no matter the case or the accents (`flugel` finds Flügel), one typo is forgiven (`scwert` finds Schwert), a
--: word may be a beginning, and the best matches come first. `search` returns the things; `open` shows a small window
--: where the player types and picks, and a callback gets the thing that was chosen and answers what the lookup returns
--: (`pick` waits for that inside a task). The library is shipped in the DLL (`gear/lua/lookup.lua`) and runs once per mod.

--- A thing of the built-in kind `'item'`: an equipment item type.
---@class GearLookupItem
---@field id integer The object type id.
---@field name string The name the game's tooltip shows (the player's language).
---@field internal string The internal type name.
---@field kind integer The type record's kind (the equipment class).

--- A thing of the built-in kind `'creature'`: a word of monster type names.
---@class GearLookupCreature
---@field word string Lower case, e.g. `'orc'` (a whole word of names like `ORC_WARRIOR.GRN`).
---@field name string The word with a capital.
---@field count integer How many monster types carry it.
---@field types integer[] Their type ids.
---@field names string[] The localized names of those types (the player's language, no duplicates), which the search matches as well.

--- A thing of the built-in kind `'ability'`.
---@class GearLookupAbility
---@field id integer
---@field name string The name in the game's texts.
---@field kind string `'combat_art'` or `'spell'`.

--- What a mod gives `register`.
---@class GearLookupKind
---@field things fun(): table[] Returns the list of things (called once; `refresh` asks again). Each thing is a table.
---@field label? string What the picker's title says.
---@field name? fun(thing: table): string The name to show (default `thing.name`).
---@field text? fun(thing: table): string The text that is searched (default the shown name).
---@field detail? fun(thing: table): string A short note after the name in the picker.

--- The options of `open` and `pick`.
---@class GearLookupOpts
---@field title? string The window's title.
---@field text? string The text the search starts with.
---@field filter? fun(thing: table): boolean Return false to leave a thing out.
---@field select? fun(thing: table): any Called with the chosen thing; what it returns is the result of the lookup (nil: the thing itself; false: refused, the picker stays open).
---@field done? fun(result: any, thing: table) Called with that result.
---@field multi? boolean Keep the picker open after a choice, for adding several things; the Done button closes it.
---@field closed? fun(cancelled: boolean) Called when the picker is gone.

-- path gear.lookup
--- Finding things of the game by name.
---@class GearLookup
local GearLookup = {}

--- Adds or replaces a kind of things to look up.
---@param kind string
---@param def GearLookupKind
function GearLookup.register(kind, def) end

--- The names of the kinds: `'ability'`, `'creature'`, `'item'` and the registered ones.
---@return string[]
function GearLookup.kinds() end

--- Forgets the list of things of `kind` so that `things` is asked again.
---@param kind string
function GearLookup.refresh(kind) end

--- The things of `kind` that match `text`, best first. An empty text gives the first things in their own order.
---@param kind string
---@param text string
---@param limit? integer Default 50.
---@param filter? fun(thing: table): boolean
---@return table[] things
function GearLookup.search(kind, text, limit, filter) end

--- The name shown for a thing of `kind`.
---@param kind string
---@param thing table
---@return string
function GearLookup.name(kind, thing) end

--- The lower case, accent-free words of a text (ä = a, ß = ss), the unit of the matching.
---@param text string
---@return string[]
function GearLookup.words(text) end

--- How well the words of a text match a query (a text or the list `words` makes): higher is better, nil when a query word
--- finds nothing.
---@param query string|string[]
---@param words string[]
---@return number? score
function GearLookup.score(query, words) end

--- Shows the picker for a kind. The player types, uses the list (mouse, Up and Down, Enter, or the controller's D-pad) and the
--- callbacks of `opts` hear the choice. One picker is open at a time; a new one replaces it.
---@param kind string
---@param opts? GearLookupOpts
function GearLookup.open(kind, opts) end

--- Whether the picker is open.
---@return boolean
function GearLookup.is_open() end

--- Closes the picker (`closed(true)` is called).
function GearLookup.close() end

--- Inside a task (`gear.task`): opens the picker and waits. Returns what `select` returned (the thing if there is no `select`),
--- nil when the player cancelled.
---@param kind string
---@param opts? GearLookupOpts
---@return any result
function GearLookup.pick(kind, opts) end

--- A one-line text field for a mod's own window: draws it, takes the keys while it has the focus (the keyboard only) and returns the
--- new text; the second result is true in the frame Enter was pressed.
---@param id string
---@param x number
---@param y number
---@param w number
---@param text string
---@param opts? {tooltip?: string, max?: integer, focus?: boolean}
---@return string text
---@return boolean entered
function GearLookup.text_field(id, x, y, w, text, opts) end

--- The id of the text field that has the focus, nil if none.
---@return string?
function GearLookup.focused() end

--- Takes the focus away from the text field.
function GearLookup.unfocus() end
