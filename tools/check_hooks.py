# Verifies that every Detours hook declares as many stack arguments as the target pops (ret N), in the English build
# and, for targets with a signature (tools/data/addresses.json), in the German one if it is there (SACRED_DE).
# A mismatch corrupts the stack on the first call. Keep this table in sync with src/game/*.cpp.
import sys, bisect, json, os, re
sys.path.insert(0, 'tools')
from eng import *
from funcs import _A
import sigs
from gen_sigs import SACRED_DE

# address: (name, stack argument count excluding `this`[, end of the function where the function table has no next
# entry close enough: unlisted handler funclets follow, and a linear sweep runs into inline switch tables])
HOOKS = {
    0x00645260: ('dxDriver7::init', 3),
    0x00645E40: ('dxDriver7::flip', 0),
    0x00646150: ('dxDriver7::lockBack', 1),
    0x006468A0: ('dxDriver7::drawLoadingScreen', 2),
    0x00632140: ('cWorldView0::render', 1),
    0x00758ED0: ('cUI_Manager::render', 1),
    0x00644130: ('cDxDevices::findMode', 4),
    0x0066E280: ('getClientCursorPos (cdecl)', 0),
    0x00655450: ('cMouse::renderCursor', 2),
    0x004B1450: ('renderSavePortrait', 4),
    0x006A0EA0: ('playVideo', 5),
    0x006A0D80: ('openMovieStream (cdecl)', 0),
    0x006487D0: ('captureScreenshot (cdecl)', 0),
    0x0060E350: ('cEngine::renderThreadRun (fastcall)', 0),
    0x0060A9F0: ('frameLimiter (cdecl)', 0),
    0x00635FE0: ('layerRecordCache', 1),
    0x00635E50: ('recordCache', 1),
    0x00640410: ('recordMapFind (called)', 1),
    0x0065E7A0: ('cTextureManager::init', 1),
    0x0080E680: ('cTextTable::load', 1),
    0x00676200: ('cMSS::cMSS', 3),
    0x0062AE90: ('cWorldView::renderTileRow', 3),
    0x006328C0: ('cWorldView::initRowWalk', 2),
    0x0062D3C0: ('cWorldView::drawTileLayers', 1),
    0x00629340: ('cQuadBatcher::flush', 1),
    # ground_mesh.cpp
    0x00629180: ('cQuadBatcher::add', 2),
    0x006292C0: ('cQuadBatcher::setTexture', 2),
    0x00643430: ('renderFlags::set (called)', 2),
    0x00643110: ('renderFlags::instance (cdecl, called)', 0),
    0x00417E70: ('worldState::instance (cdecl, called)', 0),
    0x0065ED90: ('cTextureManager::get (called)', 2),
    0x0062DD00: ('cWorldView::drawWaterTiles (called)', 1),
    # world_passes.cpp ([Debug] D3DStats)
    0x0062E410: ('cWorldView::drawObjects', 1),
    0x0062FF60: ('cWorldView::drawObjects2', 1),
    0x0044A9D0: ('cObject3D::drawModel', 5),
    0x006172C0: ('cEngine::worldMouse', 2),
    0x00623940: ('pixelsToWorld (cdecl)', 0),
    0x00623B60: ('worldToPixels (cdecl)', 0),
    # call-site redirect target: replacement takes the same (x, y) as cUI_Manager::isCursorOverUi
    0x0075AA90: ('cUI_Manager::isCursorOverUi', 2),
    # call-site redirect targets (ui_canvas.cpp): replacements take no stack arguments
    0x00655850: ('cMouse::getX', 0),
    0x00655860: ('cMouse::getY', 0),
    0x00654F60: ('cMouse::instance', 0),
    0x00655810: ('cMouse::getCursorPos', 2),
    # ui_anchor.cpp: hooks
    0x00759AF0: ('cUI_Manager::createGameWindows', 0),
    0x0075B4F0: ('cUI_Manager::showHelp', 2),
    0x006E6EA0: ('cUI_Popup::setText', 3),
    0x006E6FB0: ('cUI_Popup::setTextId', 3),
    0x006E7AF0: ('cUI_Popup::layout (fastcall)', 0),
    0x005DC430: ('cInventoryEntry::render', 2),
    0x00727B30: ('cUI_Window2::layoutChildren (called, fastcall)', 0),
    # ui_anchor.cpp: vtable slots of the anchored windows and popups (receiveEvent 1, render 1, isInside 2,
    # show 1, render2 3 arguments)
    **{a: (f'{n} +0x10 receiveEvent', 1) for n, a in (
        ('taskbar', 0x006E1BC0), ('inventory', 0x006C61C0), ('equipment', 0x006B6690), ('blacksmith', 0x006A4380),
        ('merchant', 0x006DBF50), ('minimap', 0x006D6C70), ('stats', 0x006A6110), ('console', 0x006ADF90),
        ('master', 0x006D1300), ('chest', 0x006ACFC0), ('netPortraits', 0x006D87B0), ('cube', 0x006E8BF0),
        ('trade', 0x006EA380), ('popup', 0x006E74E0))},
    0x006ADF90: ('console +0x10 receiveEvent', 1, 0x006AE46E),
    **{a: (f'{n} +0x14 render', 1) for n, a in (
        ('taskbar', 0x006E3D10), ('inventory', 0x006C63C0), ('equipment', 0x006B7700), ('cUI_Window2', 0x007277B0),
        ('minimap', 0x006D70F0), ('console', 0x006ADE60), ('master', 0x006D1180),
        ('chest', 0x006ACC00), ('netPortraits', 0x006D9020), ('cube', 0x006E8C00), ('trade', 0x006EAAE0),
        ('popup', 0x006E76C0))},
    0x006A9F20: ('stats +0x14 render', 1, 0x006AC2D1),
    **{a: (f'{n} +0x1c isInside', 2) for n, a in (('taskbar', 0x006DFD60), ('cUI_Control2', 0x00732420))},
    **{a: (f'{n} +0x24 show', 1) for n, a in (
        ('cUI_Window2', 0x00726E90), ('inventory', 0x006C5A50), ('blacksmith', 0x006A41F0), ('merchant', 0x006DBB70),
        ('stats', 0x006A5FA0), ('master', 0x006D1360), ('chest', 0x006ACD80), ('cube', 0x006E8BE0),
        ('trade', 0x006E98D0))},
    **{a: (f'{n} +0x44 render2', 3) for n, a in (
        ('blacksmith', 0x006A33A0), ('merchant', 0x006DBAC0), ('netPortraits', 0x006D8D70))},
    # aim_assist.cpp, ui_nav.cpp: the world pick (hooked) and what the controller calls
    0x00626C50: ('worldPick', 4),
    0x005FE000: ('cObjectManager::getData (called)', 1),
    0x00603E30: ('cObjectManager::hero (called)', 0),
    0x00548F60: ('cCreature::isEnemy (called)', 1),
    0x0084A961: ('rtDynamicCast (called, cdecl)', 0),
    # options_screen.cpp: the options window's vtable slots (render is cUI_Window2's) and the control functions
    0x00717040: ('cUI_Options +0x24 show', 1, 0x0071717D),
    0x00716E00: ('cUI_Options +0x44 render2', 3, 0x00717034),
    0x00732550: ('cUI_Control2::setFlags (called)', 1),
    0x007325C0: ('cUI_Control2::clearFlags (called)', 1),
    0x00732350: ('cUI_Control2::getAbsoluteRect (called)', 1),
    0x00753430: ('cUI_Slider::getValue (called, fastcall)', 0),
    0x007533B0: ('cUI_Slider::setValue (called)', 1),
    0x0060D6C0: ('cEngine::instance (called, cdecl)', 0),
    0x006113E0: ('cEngine::getViewOffset (called)', 2),
    0x006B3640: ('cUI_Book::lineAt (called)', 3),
}

# The typed functions in src/game/sacred_addr.h (Thiscall<R(Self, Args...)> / Cdecl<R(Args...)>, with "ENG address"
# in the comment on their line or the line above): their stack arguments come from the signature, so every one is
# checked whether or not the table above lists it, and a table entry that disagrees with it is an error.
def declared():
    path = os.path.join(os.path.dirname(__file__), '..', 'src', 'game', 'sacred_addr.h')
    with open(path, encoding='utf-8') as f:
        text = f.read()
    out = {}
    for m in re.finditer(r'inline (Thiscall|Cdecl)<', text):
        depth, i = 1, m.end()
        while depth:
            depth += {'<': 1, '>': -1}.get(text[i], 0)
            i += 1
        signature = text[m.end():i - 1]
        name = re.match(r'\s*(\w+)\{\};', text[i:]).group(1)
        line_end = text.find('\n', i)
        line_start = text.rfind('\n', 0, m.start()) + 1
        above = text[text.rfind('\n', 0, line_start - 1) + 1:line_start]
        eng = re.search(r'ENG ([0-9A-F]{8})', text[line_start:line_end]) or re.search(r'//.*ENG ([0-9A-F]{8})', above)
        if not eng:
            continue
        args, depth, cur = [], 0, ''
        for c in signature[signature.index('(') + 1:signature.rindex(')')]:
            depth += {'<': 1, '(': 1, '>': -1, ')': -1}.get(c, 0)
            if c == ',' and depth == 0:
                args.append(cur.strip())
                cur = ''
            else:
                cur += c
        if cur.strip():
            args.append(cur.strip())
        slots = 0
        if m.group(1) == 'Thiscall':
            slots = sum(2 if re.match(r'(const )?(double|u?int64_t)\b', a) else 1 for a in args[1:])
        out[int(eng.group(1), 16)] = (name, slots)
    return out

ok = True
for addr, (name, args) in declared().items():
    if addr in HOOKS:
        if HOOKS[addr][1] != args:
            print(f'BAD {addr:08x} {name:34} sacred_addr.h declares {args} args, the table {HOOKS[addr][1]}')
            ok = False
    else:
        HOOKS[addr] = (f'{name} (declared)', args)


def rets_of(read, addr, length):
    code = read(addr, length)
    rets = set()
    for ins in md.disasm(code, addr):
        if ins.mnemonic != 'ret':
            continue
        rets.add(int(ins.op_str, 16) if ins.op_str else 0)
        # nop/int3 padding up to the next 16-byte boundary: the function ends here (the table misses some functions)
        nxt = ins.address + ins.size
        pad = code[nxt - addr:((nxt + 15) & ~15) - addr]
        if pad and set(pad) <= {0x90, 0xCC}:
            break
    return rets


# English address -> German address, from the signature report
with open(os.path.join(os.path.dirname(__file__), 'data', 'addresses.json')) as f:
    resolved = json.load(f)['sacred.exe']
de = sigs.Image(SACRED_DE) if os.path.exists(SACRED_DE) else None
if de is None:
    print(f'warning: {SACRED_DE} not found, checking the English build only', file=sys.stderr)
de_of = {int(v['452F85C7'], 16): int(v[f'{de.timestamp:08X}'], 16)
         for v in resolved.values() if de and f'{de.timestamp:08X}' in v}

for addr, (name, args, *known_end) in sorted(HOOKS.items()):
    i = bisect.bisect_right(_A, addr)
    end = known_end[0] if known_end else _A[i] if i < len(_A) else addr + 0x4000
    rets = rets_of(rd, addr, end - addr)
    good = rets == {args * 4}
    line = f"{'OK ' if good else 'BAD'} {addr:08x} {name:34} declared {args} args, ret {sorted(rets)}"
    if addr in de_of:
        d = de_of[addr]
        de_rets = rets_of(de.rd, d, end - addr)
        good &= de_rets == {args * 4}
        line += f"; DE {d:08x} ret {sorted(de_rets)}"
    elif de:
        line += "; DE: no signature"
    ok &= good
    print(('OK ' if good else 'BAD') + line[3:])
sys.exit(0 if ok else 1)
