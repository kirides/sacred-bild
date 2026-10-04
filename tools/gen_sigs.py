# Emits the signature tables that locate SacredBild's patch sites at startup:
#   src/game/sacred_sigs.inc      named sacred.exe addresses (Sacred::Addr)
#   src/game/gameserver_sigs.inc  gameserver.exe addresses (GameServer::Addr)
#   tools/data/addresses.json     the address of every entry in each reference build (for check_hooks.py)
# Each signature comes from the DE build and must match exactly once in every reference build.
# Usage: python tools/gen_sigs.py   (set SACRED_DE / SACRED_ENG / GAMESERVER_DE / GAMESERVER_ENG to other copies)
import json, os, sys
sys.path.insert(0, os.path.dirname(__file__))
import sigs
from funcs import _A

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SACRED_DE = os.environ.get('SACRED_DE', r'B:\Spiele\GOG Games\Sacred Gold\sacred.exe')
SACRED_ENG = os.environ.get('SACRED_ENG', r'B:\Spiele\GOG Games\sacred gold GOG\Sacred.exe')
GAMESERVER_DE = os.environ.get('GAMESERVER_DE', r'B:\Spiele\GOG Games\Sacred Gold\gameserver.exe')
GAMESERVER_ENG = os.environ.get('GAMESERVER_ENG', r'B:\Spiele\GOG Games\sacred gold GOG\GameServer.exe')

# (name, DE address, kind). Kinds: func = entry point (window starts there), code = an instruction or an operand
# inside one, data = a global used by some instruction (resolved through that instruction's address operand).
SACRED = [
    # dxDriver7
    ('dxDriver7_init', 0x00645390, 'func'),
    ('dxDriver7_flip', 0x00645F70, 'func'),
    ('dxDriver7_lockBack', 0x00646280, 'func'),
    ('dxDriver7_drawLoadingScreen', 0x006469D0, 'func'),
    ('cDxDevices_findMode', 0x00644260, 'func'),
    ('mainWindowCreateReturn', 0x00813337, 'code'),
    ('activateAppResumeAnd', 0x00812445, 'code'),
    ('activateAppPauseOr', 0x00812478, 'code'),
    # mouse and UI
    ('getClientCursorPos', 0x0066E500, 'func'),
    ('cMouse_instance', 0x006550F0, 'func'),
    ('cMouse_renderCursor', 0x006555E0, 'func'),
    ('renderSavePortrait', 0x004B1740, 'func'),
    ('playVideo', 0x006A0C60, 'func'),
    ('openMovieStream', 0x006A0B40, 'func'),
    ('g_pUiManager', 0x017ECB3C, 'data'),
    ('cUI_Manager_isCursorOverUi', 0x0075A370, 'func'),
    ('cUI_Manager_render', 0x007587B0, 'func'),
    ('worldCursorUiTestCall', 0x00611F83, 'code'),
    ('cEngine_worldMouse', 0x00617360, 'func'),
    ('cEventMouseDown_vtable', 0x008950A8, 'data'),
    ('cEventMouseUp_vtable', 0x00897248, 'data'),
    # world-side reads of the cursor (ui_canvas): calls to cMouse::getX / getY / instance
    *[(f'worldGetXCalls[{n}]', a, 'code') for n, a in enumerate((0x00611FF4, 0x0061203D, 0x006127DC, 0x00612868, 0x00617E9C, 0x00617F09))],
    *[(f'worldGetYCalls[{n}]', a, 'code') for n, a in enumerate((0x00611FE7, 0x00612030, 0x006127ED, 0x00612879, 0x00617EAD, 0x00617EFC))],
    *[(f'worldMouseReads[{n}]', a, 'code') for n, a in enumerate((0x004FB6C1, 0x0060F308, 0x00610567, 0x00611F63, 0x0062772E))],
    # UI-side reads of the cursor (ui_canvas, frames): cMouse_instance() followed by reads of +4/+8 only, and the
    # calls of cMouse::getX / getY / getCursorPos; every other read of the position in the exe is a world one above.
    *[(f'uiMouseReads[{n}]', a, 'code') for n, a in enumerate((
        0x005DC2D6, 0x006A31C4, 0x006A451D, 0x006A8ADC, 0x006A949E, 0x006A9C4A, 0x006AC8D7, 0x006AEBBD, 0x006B2705,
        0x006B2815, 0x006B7497, 0x006B78BA, 0x006B7A50, 0x006BB1C4, 0x006BF82A, 0x006C1271, 0x006C1F2F, 0x006C235C,
        0x006C6EE7, 0x006C76BA, 0x006C9585, 0x006C9D12, 0x006D2BB2, 0x006D4A91, 0x006D6357, 0x006D71D1, 0x006D796C,
        0x006D8DD5, 0x006D8E67, 0x006DA834, 0x006DCCA8, 0x006DDFA4, 0x006E435C, 0x006E7459, 0x006EA5C6, 0x006F1326,
        0x006F744B, 0x006F8A06, 0x0070F211, 0x00716693, 0x007211BF, 0x00721891, 0x00721B33, 0x007280EE, 0x0072BA32,
        0x0072F919, 0x00731834, 0x00731ABC, 0x00732173, 0x0074B49D, 0x0074F7EB, 0x0075282B, 0x00753A9D, 0x007544B0,
        0x0075837D))],
    *[(f'uiGetXCalls[{n}]', a, 'code') for n, a in enumerate((0x005DC42B, 0x006B64B9, 0x006B6C67, 0x006DAB75, 0x006DBD01, 0x006DD793))],
    *[(f'uiGetYCalls[{n}]', a, 'code') for n, a in enumerate((0x005DC43B, 0x006B64C8, 0x006B6C76, 0x006DAB84, 0x006DBD10, 0x006DD7A2))],
    *[(f'uiCursorPosCalls[{n}]', a, 'code') for n, a in enumerate((0x005DC455, 0x006ACEB4, 0x006BFF32, 0x006EA170, 0x006EA1F9))],
    ('cMouse_getCursorPos', 0x006559A0, 'func'),
    ('cUI_Manager_createGameWindows', 0x007593D0, 'func'),
    ('cUI_Manager_showHelp', 0x0075ADD0, 'func'),
    ('cUI_Popup_setText', 0x006E6AE0, 'func'),
    ('cUI_Popup_setTextId', 0x006E6BF0, 'func'),
    ('cUI_Popup_layout', 0x006E7730, 'func'),
    ('cInventoryEntry_render', 0x005DC3F0, 'func'),
    ('cUI_Window2_layoutChildren', 0x007273C0, 'func'),
    # textures
    ('g_pTextureManager', 0x013E57B8, 'data'),
    ('cTextureManager_init', 0x0065EA20, 'func'),
    # engine and frame
    ('cEngine_renderThreadRun', 0x0060E3F0, 'func'),
    ('frameLimiter', 0x0060AA90, 'func'),
    ('renderLimiterReturn', 0x0060EB0E, 'code'),
    ('captureLockBackReturn', 0x0061374B, 'code'),
    ('captureScreenshot', 0x00648900, 'func'),
    # world view
    ('pixelsToWorld', 0x00623A20, 'func'),
    ('worldToPixels', 0x00623C40, 'func'),
    ('g_unzoomedProjection', 0x0182CCF0, 'data'),
    ('cWorldView0_render', 0x006322B0, 'func'),
    ('cWorldView_renderTileRow', 0x0062B000, 'func'),
    ('cWorldView_drawTileLayers', 0x0062D530, 'func'),
    ('cQuadBatcher_flush', 0x00629420, 'func'),
    ('cWorldView_drawWaterTiles', 0x0062DE70, 'func'),
    ('cWorldView_initRowWalk', 0x00632A30, 'func'),
    ('g_viewCameraX', 0x00AD5918, 'data'),
    ('g_viewCameraY', 0x00AD591C, 'data'),
    # map data record caches
    ('layerRecordCache', 0x006360E0, 'func'),
    ('recordCache', 0x00635F50, 'func'),
    ('recordMapFind', 0x006404C0, 'func'),
    ('g_recordStamp', 0x00CD59D0, 'data'),
    # network
    ('g_pGameClient', 0x0182CB70, 'data'),
    ('initNetworkNagleTest', 0x007D294E, 'code'),
    # language files (language.cpp)
    ('cTextTable_load', 0x0080DBF0, 'func'),
    ('textTableAllocCall', 0x0080DC60, 'code'),
    ('cMSS_ctor', 0x006764E0, 'func'),
    ('g_soundPakPath', 0x009D5624, 'data'),
    ('g_language', 0x017E5CB4, 'data'),
    ('g_languageCodes', 0x00897394, 'data'),
]

# gameserver.exe has no function table here: each entry gives the instruction its window starts at.
GAMESERVER = [
    ('g_pApp', 0x00634238, 'data', 0x004C2F24),                 # mov eax, [g_pApp] (cNetServer setup)
    ('firstContactTimeoutImm', 0x004DC523, 'code', 0x004DC521),  # add edx, 5000 (cNetServer_watchdogThread)
]


def resolve(ref, others, name, va, kind, start=None):
    if kind == 'data':
        refs = [sigs.operand_of(ref, start, va)] if start is not None else sigs.xrefs(ref, va)
        best = None
        for insn, op in refs[:40]:
            r = sigs.make(ref, op, others, start=start)
            if r and (best is None or len(r[0]) < len(best[0])):
                best = r
        if not best:
            return None
        sig, off, found = best
        return sig, off, 'Abs32', [va] + [o.u32(f) for o, f in zip(others, found)]
    r = sigs.make(ref, va, others, start=start, forward_only=kind == 'func')
    if not r:
        return None
    sig, off, found = r
    return sig, off, 'Match', [va] + found


def emit(path, header, namespace, entries, ref, others, table):
    lines = [f'// Generated by tools/gen_sigs.py -- do not edit by hand.',
             f'// Checked against: ' + ', '.join(f'{os.path.basename(i.path)} {i.timestamp:08X}' for i in [ref] + others),
             f'static const Sig::Entry {table}[] = {{']
    report = {}
    failed = []
    for e in entries:
        name, va, kind = e[:3]
        start = e[3] if len(e) > 3 else None
        r = resolve(ref, others, name, va, kind, start)
        if not r:
            failed.append(name)
            print(f'FAILED {name} {va:#010x}', file=sys.stderr)
            continue
        sig, off, take, addrs = r
        report[name] = {f'{i.timestamp:08X}': f'{a:08X}' for i, a in zip([ref] + others, addrs)}
        where = ', '.join(f'{a:08X}' for a in addrs)
        lines.append(f'    {{"{name}", "{sigs.fmt(sig)}", {off}, Sig::Take::{take}, &{namespace}::{name}}}, // {where}')
        print(f'{name:28} {take:5} +{off:<3} {where}')
    lines.append('};')
    with open(os.path.join(ROOT, path), 'w', newline='\n') as f:
        f.write(header + '\n'.join(lines) + '\n')
    return report, failed


def main():
    sacred = sigs.Image(SACRED_DE, _A), [sigs.Image(SACRED_ENG)]
    server = sigs.Image(GAMESERVER_DE), [sigs.Image(GAMESERVER_ENG)]
    report = {}
    r, f1 = emit('src/game/sacred_sigs.inc', '', 'Sacred::Addr', SACRED, *sacred, 'kAddressSigs')
    report['sacred.exe'] = r
    r, f2 = emit('src/game/gameserver_sigs.inc', '', 'GameServer::Addr', GAMESERVER, *server, 'kAddressSigs')
    report['gameserver.exe'] = r
    with open(os.path.join(ROOT, 'tools', 'data', 'addresses.json'), 'w', newline='\n') as f:
        json.dump(report, f, indent=1)
    if f1 or f2:
        sys.exit(f'{len(f1) + len(f2)} entries without a signature')


if __name__ == '__main__':
    main()
