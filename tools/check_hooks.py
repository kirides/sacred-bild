# Verifies that every Detours hook declares as many stack arguments as the target pops (ret N), in the DE build
# and, for targets with a signature (tools/data/addresses.json), in the other reference builds.
# A mismatch corrupts the stack on the first call. Keep this table in sync with src/game/*.cpp.
import sys, bisect, json, os
sys.path.insert(0, 'tools')
from de import *
from funcs import _A
import sigs
from gen_sigs import SACRED_ENG

# address: (name, stack argument count excluding `this`)
HOOKS = {
    0x00645390: ('dxDriver7::init', 3),
    0x00645F70: ('dxDriver7::flip', 0),
    0x00646280: ('dxDriver7::lockBack', 1),
    0x006469D0: ('dxDriver7::drawLoadingScreen', 2),
    0x006322B0: ('cWorldView0::render', 1),
    0x007587B0: ('cUI_Manager::render', 1),
    0x00644260: ('cDxDevices::findMode', 4),
    0x0066E500: ('getClientCursorPos (cdecl)', 0),
    0x006555E0: ('cMouse::renderCursor', 2),
    0x004B1740: ('renderSavePortrait', 4),
    0x006A0C60: ('playVideo', 5),
    0x006A0B40: ('openMovieStream (cdecl)', 0),
    0x0060E3F0: ('cEngine::renderThreadRun (fastcall)', 0),
    0x0060AA90: ('frameLimiter (cdecl)', 0),
    0x006360E0: ('layerRecordCache', 1),
    0x00635F50: ('recordCache', 1),
    0x006404C0: ('recordMapFind (called)', 1),
    0x0065EA20: ('cTextureManager::init', 1),
    0x0062B000: ('cWorldView::renderTileRow', 3),
    0x00632A30: ('cWorldView::initRowWalk', 2),
    0x0062D530: ('cWorldView::drawTileLayers (called)', 1),
    0x00629420: ('cQuadBatcher::flush (called)', 1),
    0x0062DE70: ('cWorldView::drawWaterTiles (called)', 1),
    0x00617360: ('cEngine::worldMouse', 2),
    0x00623A20: ('pixelsToWorld (cdecl)', 0),
    0x00623C40: ('worldToPixels (cdecl)', 0),
    0x00599880: ('cCreature::render', 2),
    0x0044B400: ('cObject3D::render', 2),
    0x0044ABA0: ('cObject3D::drawModel', 5),
    # call-site redirect target: replacement takes the same (x, y) as cUI_Manager::isCursorOverUi
    0x0075A370: ('cUI_Manager::isCursorOverUi', 2),
    # call-site redirect targets (ui_canvas.cpp): replacements take no stack arguments
    0x006559E0: ('cMouse::getX', 0),
    0x006559F0: ('cMouse::getY', 0),
    0x006550F0: ('cMouse::instance', 0),
}

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


# DE address -> address in another build, from the signature report
with open(os.path.join(os.path.dirname(__file__), 'data', 'addresses.json')) as f:
    resolved = json.load(f)['sacred.exe']
eng = sigs.Image(SACRED_ENG)
eng_of = {int(v['451BBE74'], 16): int(v[f'{eng.timestamp:08X}'], 16) for v in resolved.values()}

ok = True
for addr, (name, args) in sorted(HOOKS.items()):
    i = bisect.bisect_right(_A, addr)
    end = _A[i] if i < len(_A) else addr + 0x4000
    rets = rets_of(rd, addr, end - addr)
    good = rets == {args * 4}
    line = f"{'OK ' if good else 'BAD'} {addr:08x} {name:34} declared {args} args, ret {sorted(rets)}"
    if addr in eng_of:
        e = eng_of[addr]
        eng_rets = rets_of(eng.rd, e, end - addr)
        good &= eng_rets == {args * 4}
        line += f"; ENG {e:08x} ret {sorted(eng_rets)}"
    else:
        line += "; ENG: no signature"
    ok &= good
    print(('OK ' if good else 'BAD') + line[3:])
sys.exit(0 if ok else 1)
