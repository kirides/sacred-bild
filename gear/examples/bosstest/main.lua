-- An automation run of the creature modifiers (creature:set_modifiers, the building block of world bosses).
--   Sacred.exe -gear-auto -gear-load=N -gear-mods=bosstest
-- The savegame needs a hostile creature with id 1266 that can reach the hero (the author's GAME10: a wolf below the
-- cliff); change WOLF below for another savegame. It saves into slot 90 (use a copy of the game folder) and writes
-- the checks to the log as "CHECK ok|FAIL ..."; it quits with 0 when all passed, 1 otherwise.
if not gear.auto then return end
local A = gear.auto
local WOLF = 1266
local SLOT = 90

local failed, total = 0, 0
local function check(ok, what)
  total = total + 1
  if not ok then failed = failed + 1 end
  print('CHECK ' .. (ok and 'ok' or 'FAIL') .. ' ' .. what)
end

local boss
local handler_hits = 0
gear.on('damage', function(hit)
  handler_hits = handler_hits + 1
  -- a mod's own handler runs first: the boss wolf hits the hero for 10 (the boss multiplies it afterwards)
  if boss and hit.attacker == WOLF and hit.victim == gear.world.hero().id then return 10 end
end)
local hero_drops, last_hero = {}, nil
gear.on('frame', function()
  local h = gear.world.hero()
  if not h or not boss then return end
  local hp = h:hp()
  if last_hero and hp < last_hero then hero_drops[#hero_drops + 1] = last_hero - hp end
  last_hero = hp
end)
local saved, changed = nil, 0
gear.on('game.saved', function(slot) saved = slot end)
gear.on('savegame.changed', function() changed = changed + 1 end)

gear.task(function()
  A.wait_for_menu(120)
  A.wait_for_game(180)
  gear.wait(3)
  local main = gear.world.hero()
  local x, y = main:position()
  local wolf = gear.world.object(WOLF)
  check(wolf and wolf:is_creature(), 'the test creature exists')
  local base = wolf:max_hp()

  -- health, validation
  check(wolf:set_modifiers{health = 5, damage = 3, taken = 0.5, reflect = 0.5, regen = 0.01}, 'set_modifiers accepted')
  check(wolf:max_hp() == base * 5 and wolf:hp() == base * 5, 'health x5 (' .. base .. ' -> ' .. tostring(wolf:max_hp()) .. ')')
  check(not pcall(function() wolf:set_modifiers{damage = 'x'} end), 'a bad modifier is an error')
  check(select(2, main:set_modifiers{health = 2}) ~= nil, 'heroes are refused')
  local m = wolf:modifiers()
  check(m and m.reflect == 0.5 and m.damage == 3, 'modifiers() reads them back')
  check(#gear.world.modified() == 1, 'world.modified() lists the boss')
  boss = wolf

  -- a fight: the wolf comes, the hero attacks
  wolf:teleport(x + 60, y + 60)
  main:order_attack(wolf)
  local maxchg = false
  for i = 1, 45 do
    gear.wait(2)
    local s = wolf:modifier_stats()
    if s and s.hits >= 6 and s.dealt >= 4 then break end
  end
  local s = wolf:modifier_stats()
  check(s and s.hits >= 3, 'the boss took hits (' .. tostring(s and s.hits) .. ')')
  check(s and s.dealt >= 2, 'the boss dealt hits (' .. tostring(s and s.dealt) .. ')')
  check(s and s.reflected > 0, 'damage was reflected (' .. tostring(s and s.reflected) .. ')')
  check(s and s.damage > 0 and s.reflected <= math.floor(s.damage / 2) + s.hits, 'reflected is half of the damage taken')
  local thirty = 0
  for _, d in ipairs(hero_drops) do if d == 30 then thirty = thirty + 1 end end
  check(thirty >= 2, 'the hero lost 30 per wolf hit (10 x damage 3): ' .. table.concat(hero_drops, ','))
  check(wolf:max_hp() == base * 5 or (s and s.max_hp_resets > 0), 'the boss maximum holds (or was written again after an engine reset)')
  check(handler_hits > 0, 'the damage event carries on for other handlers')

  -- save with the boss, load: the object stays (with the boosted maximum), the modifiers are forgotten
  print('save', gear.game.save(SLOT, 'bosstest'))
  gear.wait_until(function() return saved ~= nil end, 60)
  gear.wait(1)
  boss = nil
  print('load', gear.game.load(SLOT))
  gear.wait_until(function() return changed >= 1 end, 120)
  gear.wait(6)
  wolf = gear.world.object(WOLF)
  check(wolf ~= nil and wolf:exists(), 'the boss creature survives save and load')
  check(wolf and wolf:modifiers() == nil and #gear.world.modified() == 0, 'modifiers are forgotten on load')
  local boosted = wolf and wolf:max_hp()
  check(boosted and boosted > base, 'the savegame holds the boosted maximum (' .. tostring(boosted) .. ')')
  -- re-attach as a mod would: base_hp says what the creature's own maximum is
  check(wolf:set_modifiers{health = 5, base_hp = base}, 're-attach with base_hp')
  check(wolf:max_hp() == boosted, 'no second multiplication (' .. tostring(wolf:max_hp()) .. ')')
  check(wolf:set_modifiers(nil) and wolf:max_hp() == base, 'clearing restores the own maximum (' .. tostring(wolf:max_hp()) .. ')')
  check(wolf:modifiers() == nil and #gear.world.modified() == 0, 'cleared')

  -- spawn with modifiers
  local x2, y2 = gear.world.hero():position()
  local c, err = gear.world.spawn('WOLF.GRN', x2 + 40, y2 + 40, {modifiers = {health = 10, taken = 0.1, deflect = 0.5}})
  check(c and c:max_hp() >= 10 * 100 - 10, 'spawn with modifiers: ' .. tostring(c and c:max_hp()) .. ' ' .. tostring(err))
  check(c and c:modifiers() ~= nil, 'the spawned boss has modifiers')
  if c then c:remove() end
  gear.wait(1)
  check(#gear.world.modified() == 0, 'a removed creature is forgotten')

  print('delete', gear.game.delete_save(SLOT))
  print(('RESULT %d/%d'):format(total - failed, total))
  A.quit(failed == 0 and 0 or 1)
end)
