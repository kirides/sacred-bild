T.settings = { title = 'Bow ranger', note = 'fast', tags = 'bow, pvp , ,a,b,c,d,e,toolongtagtoolongtagtoolong', share_carried = true }
T.hero_items = { { type_name = 'Ring of X', quality_rank = 3, type = 1 }, { type_name = 'Sword', quality_rank = 5, type = 2 } }
frame()
expect(#T.sent == 0, 'closed panel asks nothing')
T.keys.toggle()
frame()
expect(T.sent[1] and T.sent[1].payload.op == 'list' and T.sent[1].to == 'server' and T.sent[1].payload.per == 12, 'opening asks the first page')
tick(1)
frame()
local list = { op = 'list', total = 2, page = 1, pages = 1, builds = {
  { id = 'b1', name = 'Bob', class_name = 'Seraphim', level = 30, title = 'x', verified = true },
  { id = 'b2', name = string.rep('N', 500), class_name = 5, level = 'high', title = '<b>t</b>', verified = false },
  7, { name = 'noid' } } }
server_says('buildsharer', list, 3)
frame()
expect(not has_text('Bob'), 'a message from another player is ignored')
server_says('buildsharer', list, 0)
frame()
expect(has_text('Bob') and has_text('[verified]'), 'the list shows the builds')
expect(has_text('2 builds, page 1 of 1'), 'count line')
T.clicks['b1'] = true; tick(1); frame()
local last = T.sent[#T.sent]
expect(last.payload.op == 'get' and last.payload.id == 'b1', 'click fetches the build')
server_says('buildsharer', { op = 'build', build = { id = 'b1', name = 'Bob', class_name = 'Seraphim', level = 30, title = 'T', note = 'N', verified = true, tags = { 'pvp' },
  skills = { { name = 'Weapon Lore', level = 12, bonus = 3 } }, arts = { { name = 'Blazing Disc' } }, equipment = { { name = 'Bow', tier = 'rare', level = 30, sockets = 2 } },
  carried = { 'Potion' } } })
frame()
expect(has_text('Weapon Lore') and has_text('Bow') and has_text('Potion') and has_text('server-verified'), 'detail shows skills, equipment, carried')
T.clicks['save'] = true; frame()
expect(T.files['saved_builds.json'] ~= nil and #T.encoded.builds == 1, 'a build is saved to the file')
T.picks['tabs'] = 3; frame()
expect(has_text('Bob'), 'saved tab lists it')
T.picks['tabs'] = 2; frame()
expect(has_text('Title: Bow ranger') and has_text('Carried items sent: 2'), 'my build tab')
T.clicks['publish'] = true; tick(1); frame()
local pub = T.sent[#T.sent].payload
expect(pub.op == 'publish' and pub.title == 'Bow ranger' and pub.level == 41, 'publish sends title and level')
expect(#pub.tags == 5, 'at most five tags')
expect(#pub.tags[5] <= 16, 'tags are cut to 16 characters')
expect(pub.carried[1] == 'Sword' and pub.carried[2] == 'Ring of X', 'best items first')
expect(pub.skills == nil and pub.equipment == nil, 'no skills or equipment from the client')
server_says('buildsharer', { op = 'published', build = { verified = true, class_name = 'Battle Mage', level = 41, skills = {}, equipment = {}, arts = {} } })
frame()
expect(has_text('Published: the server read your hero.') and has_text('server-verified'), 'published status')
server_says('buildsharer', { op = 'error', error = string.rep('e', 999) })
frame()
expect(has_text(string.rep('e', 70)) and not has_text(string.rep('e', 100)), 'a long error is cut')
T.picks['tabs'] = 1; T.picks['class'] = 3; tick(1); frame()
expect(T.sent[#T.sent].payload.class == 2 and T.sent[#T.sent].payload.page == 1, 'class filter is sent')
for _, junk in ipairs({ 5, 'x', {}, { op = 5 }, { op = 'list' }, { op = 'list', builds = 'no' }, { op = 'build', build = 4 }, { op = 'mine', build = 'x' } }) do
  server_says('buildsharer', junk)
end
frame()
expect(true, 'garbage does not break the panel')
T.supported = false; T.panel_open.main = false; frame(); T.keys.toggle(); frame()
expect(has_text('This server has no custom net support'), 'an unsupported server is explained')
tick(100)
