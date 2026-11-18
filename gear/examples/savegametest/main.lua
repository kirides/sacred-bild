if not gear.auto then return end
local A = gear.auto
local events = {}
gear.on('savegame.changed', function(kind, scope, n) events[#events + 1] = kind; print('EVENT changed', kind, scope, n) end)
gear.on('savegame.saving', function(scope) print('EVENT saving', scope) end)
local h = gear.store.game('t.json', {n = 0, list = {}}, {every = 0})
print('INIT kind', gear.savegame.kind(), 'scope', gear.savegame.scope(), 'n', h.data.n)
local saved
gear.on('game.saved', function(slot) saved = slot; print('EVENT game.saved', slot) end)
local function waitchanged(count)
  gear.wait_until(function() return #events >= count end, 120)
  gear.wait(4)
end
gear.task(function()
  A.wait_for_menu(120)
  A.wait_for_game(180)
  gear.wait(3)
  print('GAME1 kind', gear.savegame.kind(), 'scope', gear.savegame.scope(), 'n', h.data.n)
  h.data.n = 5; h.data.list[1] = 'a'; h:changed()
  print('save', gear.game.save(90, 'sgtest'))
  gear.wait_until(function() return saved ~= nil end, 60)
  gear.wait(1)
  h.data.n = 9; h:changed()   -- not saved
  print('load 10', gear.game.load(10))
  waitchanged(#events + 1)
  print('GAME2 scope', gear.savegame.scope(), 'n', h.data.n, #h.data.list)
  local c = #events
  print('load 90', gear.game.load(90))
  waitchanged(c + 1)
  print('GAME3 scope', gear.savegame.scope(), 'n', h.data.n, h.data.list[1])
  print('delete 90', gear.game.delete_save(90))
  A.quit(0)
end)
