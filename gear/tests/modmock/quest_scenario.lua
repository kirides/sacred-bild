T.settings = { notices = true, refresh = 30 }
T.hero_items = { { type_name = 'Strange key', quest = true, type = 9, w = 1, h = 1 }, { type_name = 'Sword', type = 3 } }
T.ground = { { type_name = 'Lost ring', quest = true, x = 1100, y = 1000, type = 5 }, { type_name = 'Gold', quest = false, x = 1000, y = 1010 } }
T.keys.toggle(); frame()
expect(T.sent[1] and T.sent[1].payload.op == 'state' and T.sent[1].payload.watch == true, 'opening asks for the state and watches')
expect(has_text('Strange key') and not has_text('Sword'), 'quest items of the backpack')
expect(has_text('Lost ring') and has_text('2 tiles away'), 'quest items near the hero with distance')
server_says('questhelper', { op = 'state', available = false, reason = 'the server has no hero data of you yet' })
T.picks['tabs'] = 2; frame()
expect(has_text('no hero data'), 'the reason is shown, nothing made up')
server_says('questhelper', { op = 'state', available = true, total = 2, known = 1, catalog = 1, records = {
  { key = 100, value = 1, name = 'The lost sword', area = 'Ancaria', hint = 'Ask the smith.', label = 'started', known = true, others = { 'Bob' } },
  { key = 555, value = 7, known = false } } })
frame()
expect(has_text('The lost sword = 1 - started') and has_text('Ask the smith.') and has_text('Also: Bob') and has_text('record 555 = 7'), 'records with catalog and unknown ones')
server_says('questhelper', { op = 'state', available = true, total = 3, known = 1, catalog = 1, records = {
  { key = 100, value = 2, name = 'The lost sword', known = true }, { key = 555, value = 7, known = false }, { key = 777, value = 1, known = false } } })
frame()
expect(has_text('Quest record: The lost sword (changed)'), 'a change notice is drawn')
T.picks['tabs'] = 3; frame()
expect(has_text('What it cannot'), 'the about tab states the limits')
for _, junk in ipairs({ 5, {}, { op = 7 }, { op = 'state', available = true, records = 'x' }, { op = 'state', available = true, total = 'a', records = { 7, 'x', { key = 1 } } } }) do
  server_says('questhelper', junk)
end
T.picks['tabs'] = 2; frame()
expect(true, 'garbage does not break the panel')
server_says('questhelper', { op = 'state', available = true, total = 1, known = 0, records = { { key = 1, value = 1, hint = string.rep('h', 5000), others = { string.rep('n', 300) } } } }, 4)
frame()
expect(not has_text(string.rep('h', 500)), 'a state from another player is ignored')
server_says('questhelper', { op = 'state', available = true, total = 1, known = 0, records = { { key = 1, value = 1, hint = string.rep('h', 5000), others = { string.rep('n', 300) } } } })
frame()
expect(has_text(string.rep('h', 100)) and not has_text(string.rep('h', 500)), 'a huge hint is cut')
tick(60); T.panel_open.main = false; frame()
expect(T.sent[#T.sent].payload.op == 'unwatch', 'closing stops the updates')
T.supported = false; T.panel_open.main = true; frame()
expect(has_text('Records need a game server'), 'single player is explained')
