# Emits the signature tables that locate SacredBild's patch sites at startup:
#   src/game/sacred_sigs.inc      named sacred.exe addresses (Sacred::Addr)
#   src/game/gameserver_sigs.inc  gameserver.exe addresses (GameServer::Addr)
#   tools/data/addresses.json     the address of every entry in each checked build (for check_hooks.py)
# Each signature comes from the English GOG build and must match exactly once in it and in every other build that is
# there (the German one: SACRED_DE / GAMESERVER_DE; skipped with a warning if missing).
# Usage: python tools/gen_sigs.py   (set SACRED_ENG / GAMESERVER_ENG / SACRED_DE / GAMESERVER_DE to your copies)
import json, os, sys
sys.path.insert(0, os.path.dirname(__file__))
import sigs
from funcs import _A

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SACRED_ENG = os.environ.get('SACRED_ENG', r'B:\Spiele\GOG Games\sacred gold GOG\Sacred.exe')
GAMESERVER_ENG = os.environ.get('GAMESERVER_ENG', r'B:\Spiele\GOG Games\sacred gold GOG\GameServer.exe')
SACRED_DE = os.environ.get('SACRED_DE', r'B:\Spiele\GOG Games\Sacred Gold\sacred.exe')
GAMESERVER_DE = os.environ.get('GAMESERVER_DE', r'B:\Spiele\GOG Games\Sacred Gold\gameserver.exe')


def other_builds(*paths):
    """Images of the builds besides the reference that are there; a warning for each one that isn't."""
    found = []
    for p in paths:
        if os.path.exists(p):
            found.append(sigs.Image(p))
        else:
            print(f'warning: {p} not found, signatures are not checked against it', file=sys.stderr)
    return found


# (name, ENG address, kind). Kinds: func = entry point (window starts there), code = an instruction or an operand
# inside one, data = a global used by some instruction (resolved through that instruction's address operand).
SACRED = [
    # dxDriver7
    ('dxDriver7_init', 0x00645260, 'func'),
    ('dxDriver7_flip', 0x00645E40, 'func'),
    ('dxDriver7_lockBack', 0x00646150, 'func'),
    ('dxDriver7_drawLoadingScreen', 0x006468A0, 'func'),
    ('cDxDevices_findMode', 0x00644130, 'func'),
    ('mainWindowCreateReturn', 0x00813C87, 'code'),
    ('activateAppResumeAnd', 0x00812D95, 'code'),
    ('activateAppPauseOr', 0x00812DC8, 'code'),
    # mouse and UI
    ('getClientCursorPos', 0x0066E280, 'func'),
    ('cMouse_instance', 0x00654F60, 'func'),
    ('cMouse_renderCursor', 0x00655450, 'func'),
    ('renderSavePortrait', 0x004B1450, 'func'),
    ('playVideo', 0x006A0EA0, 'func'),
    ('openMovieStream', 0x006A0D80, 'func'),
    ('g_pUiManager', 0x017EEBBC, 'data'),
    ('cUI_Manager_isCursorOverUi', 0x0075AA90, 'func'),
    ('cUI_Manager_render', 0x00758ED0, 'func'),
    ('worldCursorUiTestCall', 0x00611EE3, 'code'),
    ('worldCursorSpecialRenderReturn', 0x00612E6F, 'code'),
    ('cEngine_worldMouse', 0x006172C0, 'func'),
    ('cEventMouseDown_vtable', 0x0089704C, 'data'),
    ('cEventMouseUp_vtable', 0x00899248, 'data'),
    # world-side reads of the cursor (ui_canvas): calls to cMouse::getX / getY / instance
    *[(f'worldGetXCalls[{n}]', a, 'code') for n, a in enumerate((0x00611F54, 0x00611F9D, 0x0061273C, 0x006127C8, 0x00617DFC, 0x00617E69))],
    *[(f'worldGetYCalls[{n}]', a, 'code') for n, a in enumerate((0x00611F47, 0x00611F90, 0x0061274D, 0x006127D9, 0x00617E0D, 0x00617E5C))],
    *[(f'worldMouseReads[{n}]', a, 'code') for n, a in enumerate((0x004FB881, 0x0060F268, 0x006104C7, 0x00611EC3, 0x0062764E))],
    # UI-side reads of the cursor (ui_canvas, frames): cMouse_instance() followed by reads of +4/+8 only, and the
    # calls of cMouse::getX / getY / getCursorPos; every other read of the position in the exe is a world one above.
    *[(f'uiMouseReads[{n}]', a, 'code') for n, a in enumerate((
        0x005DC316, 0x006A3404, 0x006A475D, 0x006A8E1C, 0x006A97DE, 0x006A9F8A, 0x006ACC27, 0x006AEEDD, 0x006B2A25,
        0x006B2B35, 0x006B77F7, 0x006B7C1A, 0x006B7DB0, 0x006BB524, 0x006BFA9A, 0x006C14E1, 0x006C219F, 0x006C25CC,
        0x006C7167, 0x006C793A, 0x006C9805, 0x006C9F92, 0x006D2E32, 0x006D4D11, 0x006D65D7, 0x006D7451, 0x006D7BEC,
        0x006D9055, 0x006D90E7, 0x006DAAB4, 0x006DCEF8, 0x006DE1F4, 0x006E45AC, 0x006E7819, 0x006EA956, 0x006F1666,
        0x006F778B, 0x006F8D46, 0x0070F811, 0x00716E53, 0x0072192F, 0x00722001, 0x007222A3, 0x0072885E, 0x0072C1A2,
        0x0072FFF9, 0x00731F14, 0x0073219C, 0x00732853, 0x0074BBED, 0x0074FEDB, 0x00752F4B, 0x007541BD, 0x00754BD0,
        0x00758A9D))],
    *[(f'uiGetXCalls[{n}]', a, 'code') for n, a in enumerate((0x005DC46B, 0x006B6819, 0x006B6FC7, 0x006DADF5, 0x006DBF81, 0x006DD9E3))],
    *[(f'uiGetYCalls[{n}]', a, 'code') for n, a in enumerate((0x005DC47B, 0x006B6828, 0x006B6FD6, 0x006DAE04, 0x006DBF90, 0x006DD9F2))],
    *[(f'uiCursorPosCalls[{n}]', a, 'code') for n, a in enumerate((0x005DC495, 0x006AD204, 0x006C01A2, 0x006EA500, 0x006EA589))],
    ('cMouse_getCursorPos', 0x00655810, 'func'),
    ('cUI_Manager_createGameWindows', 0x00759AF0, 'func'),
    ('cUI_Manager_showHelp', 0x0075B4F0, 'func'),
    ('cUI_Popup_setText', 0x006E6EA0, 'func'),
    ('cUI_Popup_setTextId', 0x006E6FB0, 'func'),
    ('cUI_Popup_layout', 0x006E7AF0, 'func'),
    ('cInventoryEntry_render', 0x005DC430, 'func'),
    ('cUI_Window2_layoutChildren', 0x00727B30, 'func'),
    # textures
    ('g_pTextureManager', 0x013E7838, 'data'),
    ('cTextureManager_init', 0x0065E7A0, 'func'),
    # engine and frame
    ('cEngine_renderThreadRun', 0x0060E350, 'func'),
    ('frameLimiter', 0x0060A9F0, 'func'),
    ('renderLimiterReturn', 0x0060EA6E, 'code'),
    ('captureLockBackReturn', 0x006136AB, 'code'),
    ('captureScreenshot', 0x006487D0, 'func'),
    # world view
    ('pixelsToWorld', 0x00623940, 'func'),
    ('worldToPixels', 0x00623B60, 'func'),
    ('g_unzoomedProjection', 0x0182ED70, 'data'),
    ('cWorldView0_render', 0x00632140, 'func'),
    ('cWorldView_renderTileRow', 0x0062AE90, 'func'),
    ('cWorldView_drawTileLayers', 0x0062D3C0, 'func'),
    ('cQuadBatcher_flush', 0x00629340, 'func'),
    ('cQuadBatcher_add', 0x00629180, 'func'),
    ('cQuadBatcher_setTexture', 0x006292C0, 'func'),
    ('renderFlags_instance', 0x00643110, 'func'),
    ('renderFlags_set', 0x00643430, 'func'),
    ('worldState_instance', 0x00417E70, 'func'),
    ('cTextureManager_get', 0x0065ED90, 'func'),
    ('cWorldView_drawWaterTiles', 0x0062DD00, 'func'),
    ('cWorldView_drawObjects', 0x0062E410, 'func'),
    ('cWorldView_drawObjects2', 0x0062FF60, 'func'),
    ('cObject3D_drawModel', 0x0044A9D0, 'func'),
    ('cWorldView_initRowWalk', 0x006328C0, 'func'),
    ('g_viewCameraX', 0x00AD7998, 'data'),
    ('g_viewCameraY', 0x00AD799C, 'data'),
    # map data record caches
    ('layerRecordCache', 0x00635FE0, 'func'),
    ('recordCache', 0x00635E50, 'func'),
    ('recordMapFind', 0x00640410, 'func'),
    ('g_recordStamp', 0x00CD7A50, 'data'),
    # network
    ('g_pGameClient', 0x0182EBF0, 'data'),
    ('initNetworkNagleTest', 0x007D310E, 'code'),
    # language files (language.cpp)
    ('cTextTable_load', 0x0080E680, 'func'),
    ('textTableAllocCall', 0x0080E6F0, 'code'),
    ('cMSS_ctor', 0x00676200, 'func'),
    ('g_soundPakPath', 0x009D760C, 'data'),
    ('g_language', 0x017E7D34, 'data'),
    ('g_languageCodes', 0x00899394, 'data'),
    # controller: aim assist on the world pick (aim_assist.cpp), the options window (options_screen.cpp)
    ('worldPick', 0x00626C50, 'func'),
    ('g_pObjectManager', 0x00AD5C40, 'data'),
    ('cObjectManager_getData', 0x005FE000, 'func'),
    ('cObjectManager_hero', 0x00603E30, 'func'),
    ('rtDynamicCast', 0x0084A961, 'func'),
    ('cObject_typeDescriptor', 0x008EB648, 'data'),
    ('cCreature_typeDescriptor', 0x008EB660, 'data'),
    ('cCreature_isEnemy', 0x00548F60, 'func'),
    ('cUI_Options_vtable', 0x00897078, 'data'),
    ('cUI_Control2_setFlags', 0x00732550, 'func'),
    ('cUI_Control2_clearFlags', 0x007325C0, 'func'),
    ('cUI_Control2_getAbsoluteRect', 0x00732350, 'func'),
    ('cUI_Slider_getValue', 0x00753430, 'func'),
    ('cUI_Slider_setValue', 0x007533B0, 'func'),
    ('cEngine_instance', 0x0060D6C0, 'func'),
    ('cEngine_getViewOffset', 0x006113E0, 'func'),
    ('cUI_Book_lineAt', 0x006B3640, 'func'),
    ('cUI_Savegame_selectRow', 0x0071C360, 'func'),
    ('textResources_instance', 0x006725E0, 'func'),
    ('textResources_get', 0x00672C90, 'func'),
    ('g_pFontManager', 0x00CDCA70, 'data'),
    ('g_hasAddon', 0x0182EBEC, 'data'),
    ('g_portalTexts', 0x017EA420, 'data'),
    ('cUI_NetworkInfo_cellRect', 0x006F7130, 'func'),
    ('g_pNetPlayers', 0x0182EBE8, 'data'),
    ('cNetPlayers_count', 0x007DB2C0, 'func'),
    # controller: walking by move orders (hero_move.cpp)
    ('cEngine_sendOrder', 0x00617030, 'func'),
    ('cOrder_vtable', 0x0089095C, 'data'),
    ('g_worldInputFlags', 0x00AB73DC, 'data'),
]

# gameserver.exe (the same addresses in both builds) has no function table here: each entry gives the instruction
# its window starts at.
GAMESERVER = [
    ('g_pApp', 0x00634238, 'data', 0x004C2F24),                 # mov eax, [g_pApp] (cNetServer setup)
    ('firstContactTimeoutImm', 0x004DC523, 'code', 0x004DC521),  # add edx, 5000 (cNetServer_watchdogThread)
]


def resolve(ref, others, name, va, kind, start=None):
    if kind == 'data':
        refs = [sigs.operand_of(ref, start, va)] if start is not None else sigs.xrefs(ref, va, 40)
        best = None
        for insn, op in refs:
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
    sacred = sigs.Image(SACRED_ENG, _A), other_builds(SACRED_DE)
    server = sigs.Image(GAMESERVER_ENG), other_builds(GAMESERVER_DE)
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
