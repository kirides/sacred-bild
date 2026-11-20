-- Contact sheet of the small UI images (<= 300x120) at 1:1 with their ids, to find the piece a screen uses:
-- Sacred.exe -gear-auto -gear-mods=rmltest-images (stays in the menus; screenshots sheet-01.bmp ... in GearAutomation\<instance>)
if not gear.auto then return end
local A, ui = gear.auto, gear.ui
local pages, page, list = {}, 1, {}
for _, i in ipairs(ui.images()) do
    local w, h = i.width or i.w or 0, i.height or i.h or 0
    if w <= 300 and h <= 120 and (w >= 24 or h >= 24) then list[#list + 1] = {id = i.id, w = w, h = h} end
end
local x, y, rowh, cur = 4, 4, 0, {}
pages[1] = cur
for _, e in ipairs(list) do
    local cw = math.max(e.w, 28)
    if x + cw + 4 > 1020 then x, y, rowh = 4, y + rowh + 14, 0 end
    if y + e.h + 14 > 764 then x, y, rowh, cur = 4, 4, 0, {}; pages[#pages + 1] = cur end
    cur[#cur + 1] = {id = e.id, w = e.w, h = e.h, x = x, y = y + 10}
    x = x + cw + 6
    rowh = math.max(rowh, e.h)
end
gear.on('draw', function()
    ui.rect(0, 0, 1024, 768, 0xFF404048, {layer = 'top'})
    for _, e in ipairs(pages[page] or {}) do
        ui.rect(e.x - 1, e.y - 1, e.w + 2, e.h + 2, 0xFF70505A, {layer = 'top'})
        ui.image(e.id, e.x, e.y, e.w, e.h, {layer = 'top'})
        ui.text(tostring(e.id), e.x, e.y - 11, {layer = 'top', size = 9, color = 0xFFFFFF80})
    end
end)
gear.task(function()
    A.wait_for_menu(120)
    gear.wait(2)
    for p = 1, #pages do page = p; gear.wait(0.8); A.screenshot(('sheet-%02d'):format(p)) end
    A.quit(0)
end)
