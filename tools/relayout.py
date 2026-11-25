#!/usr/bin/env python3
"""Moves the sources into the src/{core,bild,gear} layout, one stage at a time (docs/Development.md, "Source layout").

    python tools/relayout.py <stage> [--dry-run]      stage: core | bild | gear

Each stage `git mv`s the files its rules name, rewrites the #include lines of every C/C++ source (and the file names in the
resource scripts) so they still find the same files, and replaces the old paths in the build files, tools, docs and comments
with the new ones. A file name stays the same, so git sees every move as a 100% rename.

It is also the tool for a branch that was cut before a stage: rebase it, run the stage again (files already moved are skipped,
the files the branch added are moved by the same rules) and build.
"""
import argparse, fnmatch, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Rules per stage, first match wins: (pattern, destination). A pattern ending in '/' is a folder (everything below it keeps
# its sub-path under the destination folder); otherwise it is an fnmatch pattern for a file, which lands in the destination
# folder (ending in '/') or at the destination path.
STAGES = {
    'core': {
        'rules': [
            ('src/sacred/address.h', 'src/core/sacred/'),   # the typed addresses; the classes call SacredBild's table
            ('src/fmt.*', 'src/core/'),
            ('src/log.*', 'src/core/'),
            ('src/patch.*', 'src/core/'),
            ('src/sig.*', 'src/core/'),
            ('src/instance.*', 'src/core/'),
        ],
        'old_roots': {'bild': ['src'], 'gear': ['gear', 'src']},
        'new_roots': {'bild': ['src', 'src/core'], 'gear': ['gear', 'src/core']},
    },
    'bild': {
        'rules': [
            ('src/core/sacred/address.h', None),
            ('src/core/sacred/', 'src/bild/sacred/'),  # (a branch that ran the first version of the core stage)
            ('src/core/', None),                     # stays
            ('src/bild/', None),
            ('src/SacredBild_version.txt', 'src/bild/'),
            ('src/sacred/', 'src/bild/sacred/'),
            ('src/main.cpp', 'src/bild/'),
            ('src/pch.h', 'src/bild/'),
            ('src/exports.def', 'src/bild/'),
            ('src/version.rc', 'src/bild/'),
            ('src/proxy.*', 'src/bild/'),
            ('src/system_ddraw.*', 'src/bild/'),
            ('src/crash_dump.*', 'src/bild/'),
            ('src/spin_lock.h', 'src/bild/'),
            ('src/mem.h', 'src/bild/'),
            ('src/game/build.*', 'src/bild/engine/'),
            ('src/game/sacred_addr.h', 'src/bild/engine/'),
            ('src/game/sacred_sigs.inc', 'src/bild/engine/'),
            ('src/game/gameserver_addr.h', 'src/bild/engine/'),
            ('src/game/gameserver_sigs.inc', 'src/bild/engine/'),
            ('src/config.*', 'src/bild/settings/'),
            ('src/config/', 'src/bild/settings/config/'),
            ('src/settings_window.*', 'src/bild/settings/'),
            ('src/ui/form*', 'src/bild/settings/'),
            ('dist/SacredBild.ini', 'src/bild/settings/'),
            ('src/ddraw9/', 'src/bild/ddraw9/'),
            ('src/render/', 'src/bild/render/'),
            ('src/game/device_proxy.*', 'src/bild/render/'),
            ('src/game/frame_hooks.*', 'src/bild/render/'),
            ('src/game/ground_quads.*', 'src/bild/render/'),
            ('src/game/sprite_quads.*', 'src/bild/render/'),
            ('src/game/ground_mesh.*', 'src/bild/render/'),
            ('src/game/gpu_skin.*', 'src/bild/render/'),
            ('src/game/granny_*', 'src/bild/render/'),
            ('src/game/fast_math.*', 'src/bild/perf/'),
            ('src/game/sound_lock.*', 'src/bild/perf/'),
            ('src/game/file_cache.*', 'src/bild/perf/'),
            ('src/game/map_cache.*', 'src/bild/perf/'),
            ('src/game/resolution*', 'src/bild/display/'),
            ('src/game/window_fit.*', 'src/bild/display/'),
            ('src/game/focus.*', 'src/bild/display/'),
            ('src/game/zoom.*', 'src/bild/display/'),
            ('src/game/ui_canvas.*', 'src/bild/ui/'),
            ('src/game/ui_anchor.*', 'src/bild/ui/'),
            ('src/game/ui_state.*', 'src/bild/ui/'),
            ('src/game/ui_windows.*', 'src/bild/ui/'),
            ('src/game/controller.*', 'src/bild/controller/'),
            ('src/game/click_walk.*', 'src/bild/controller/'),
            ('src/game/hero_move.*', 'src/bild/controller/'),
            ('src/game/aim_assist.*', 'src/bild/controller/'),
            ('src/game/ui_nav.*', 'src/bild/controller/'),
            ('src/game/gear_nav.*', 'src/bild/controller/'),
            ('src/game/options_screen.*', 'src/bild/controller/'),
            ('src/input/', 'src/bild/controller/'),
            ('src/overlay/prompts*', 'src/bild/controller/'),
            ('src/overlay/', 'src/bild/overlay/'),
            ('src/net/', 'src/bild/net/'),
            ('src/game/gameserver.*', 'src/bild/net/'),
            ('src/game/language.*', 'src/bild/game/'),
            ('src/game/pets.*', 'src/bild/game/'),
            ('src/game/movie.*', 'src/bild/game/'),
            ('src/game/screenshot.*', 'src/bild/game/'),
            ('src/game/d3d_stats.*', 'src/bild/diag/'),
            ('src/game/file_io_stats.*', 'src/bild/diag/'),
            ('src/game/world_passes.*', 'src/bild/diag/'),
            ('src/game/skin_check.*', 'src/bild/diag/'),
            ('src/game/layout_check.*', 'src/bild/diag/'),
            ('src/profiler.*', 'src/bild/diag/'),
        ],
        'old_roots': {'bild': ['src', 'src/core'], 'gear': ['gear', 'src/core']},
        'new_roots': {'bild': ['src/bild', 'src/core'], 'gear': ['gear', 'src/core']},
    },
    'gear': {
        'rules': [
            ('gear/dist/', None),                    # generated, leaves git in its own stage
            ('gear/game/', 'src/gear/engine/'),
            ('gear/world.*', 'src/gear/world/'),
            ('gear/party.*', 'src/gear/world/'),
            ('gear/buffs.*', 'src/gear/world/'),
            ('gear/classes*', 'src/gear/world/'),
            ('gear/killstat.*', 'src/gear/world/'),
            ('gear/pfx.*', 'src/gear/world/'),
            ('gear/inventory.*', 'src/gear/items/'),
            ('gear/itemtip.*', 'src/gear/items/'),
            ('gear/itemicon.*', 'src/gear/items/'),
            ('gear/itemuse.*', 'src/gear/items/'),
            ('gear/pickup.*', 'src/gear/items/'),
            ('gear/potionpick.*', 'src/gear/items/'),
            ('gear/drops.*', 'src/gear/items/'),
            ('gear/master.*', 'src/gear/items/'),
            ('gear/ui.*', 'src/gear/ui/'),
            ('gear/text.*', 'src/gear/ui/'),
            ('gear/replace.*', 'src/gear/ui/'),
            ('gear/padnav.*', 'src/gear/ui/'),
            ('gear/keyblock.*', 'src/gear/ui/'),
            ('gear/input.*', 'src/gear/ui/'),
            ('gear/rml.*', 'src/gear/ui/'),
            ('gear/css_compat.*', 'src/gear/ui/'),
            ('gear/dds.*', 'src/gear/ui/'),
            ('gear/bcdec.h', 'src/gear/ui/'),
            ('gear/icons_css.inc', 'src/gear/ui/'),
            ('gear/', 'src/gear/'),                  # everything else keeps its place below the Gear root
        ],
        'old_roots': {'bild': ['src/bild', 'src/core'], 'gear': ['gear', 'src/core']},
        'new_roots': {'bild': ['src/bild', 'src/core'], 'gear': ['src/gear', 'src/core']},
    },
}

SOURCE_EXT = ('.cpp', '.h', '.hpp', '.inc', '.hlsl', '.rc')
# Text files whose path references follow the move (besides every moved file itself).
TEXT_GLOBS = ['src/*', 'gear/*', 'CMakeLists.txt', 'CMakePresets.json', 'cmake/*', '.github/workflows/*', '.githooks/*', '.luarc.json', '.clangd',
              '.gitignore', '.gitattributes', 'README.md', 'CLAUDE.md', 'docs/*.md', 'docs/**/*.md', 'tools/*.py', 'tools/*.ps1',
              'tools/**/*.cpp', 'tools/**/*.md', 'install.ps1', 'uninstall.ps1', 'import-language.ps1',
              'src/**/*', 'gear/**/*']
SKIP = ('tools/relayout.py', 'changelog/', 'mod-index.json', 'sacred-dsoal.json')
TEXT_EXT = SOURCE_EXT + ('.py', '.ps1', '.md', '.txt', '.lua', '.rml', '.rcss', '.json', '.yml', '.yaml', '.cmake', '.def', '.manifest',
                         '.cmd', '.sh', '')

INCLUDE = re.compile(r'^(\s*#\s*include\s+")([^"]+)(")', re.M)
RC_FILE = re.compile(r'^(\s*\w+\s+(?:RCDATA|RT_MANIFEST|ICON|BITMAP|CURSOR|FONT)\s+")([^"]+)(")', re.M)


def git(*args):
    r = subprocess.run(['git', '-C', ROOT, *args], capture_output=True, text=True, encoding='utf-8')
    if r.returncode:
        sys.exit('git %s: %s' % (' '.join(args), r.stderr.strip()))
    return r.stdout


def tracked():
    return [p for p in git('ls-files', '-z').split('\0') if p]


def destination(rules, path):
    for pattern, dest in rules:
        if pattern.endswith('/'):
            if path.startswith(pattern):
                return None if dest is None else dest + path[len(pattern):]
        elif fnmatch.fnmatchcase(path, pattern):
            if dest is None:
                return None
            return dest + os.path.basename(path) if dest.endswith('/') else dest
    return None


def component(path, roots):
    """'gear' for a file below a Gear root, else 'bild' (SacredBild and the shared core find their headers the same way)."""
    return 'gear' if any(path.startswith(r + '/') for r in roots['gear'][:1]) else 'bild'


def resolve(includer, name, roots, files):
    """The file `#include "name"` in `includer` finds: next to the includer first, then along the include roots."""
    for base in [os.path.dirname(includer)] + roots[component(includer, roots)]:
        cand = os.path.normpath(os.path.join(base, name)).replace(os.sep, '/')
        if cand in files:
            return cand
    return None


def spelling(includer, target, name, roots, files, relative):
    if not relative and resolve(includer, name, roots, files) == target:
        return name
    here = os.path.dirname(includer)
    if relative or os.path.dirname(target) == here:
        return os.path.relpath(target, here or '.').replace(os.sep, '/')
    for root in roots[component(includer, roots)]:
        if target.startswith(root + '/'):
            rel = target[len(root) + 1:]
            if resolve(includer, rel, roots, files) == target:
                return rel
    return os.path.relpath(target, here or '.').replace(os.sep, '/')


def rewrite_includes(text, old_path, new_path, stage, old_files, new_files, moves):
    is_rc = new_path.endswith(('.rc', '.hlsl'))

    def fix(m):
        name = m.group(2)
        target = resolve(old_path, name, stage['old_roots'], old_files)
        if target is None:
            return m.group(0)                        # a generated or system header (version_info.h): left alone
        target = moves.get(target, target)
        new = spelling(new_path, target, name, stage['new_roots'], new_files, is_rc or name.startswith('.'))
        return m.group(1) + new + m.group(3)

    text = INCLUDE.sub(fix, text)
    if new_path.endswith('.rc'):
        text = RC_FILE.sub(fix, text)
    return text


def reference_map(moves, old_files):
    """Old -> new spellings of paths in prose and build files: whole files, file stems ('src/game/ui_state.' for ui_state.*) and
    whole folders, where every file below them went to the same place."""
    out = dict(moves)
    stems, dirs = {}, {}
    for old, new in moves.items():
        stems.setdefault(os.path.splitext(old)[0] + '.', set()).add(os.path.splitext(new)[0] + '.')
        d_old, d_new = os.path.dirname(old), os.path.dirname(new)
        while d_old:
            # the folder maps if the file's sub-path below it is unchanged
            rest = old[len(d_old) + 1:]
            if new.endswith('/' + rest) or new == rest:
                dirs.setdefault(d_old + '/', set()).add(new[:len(new) - len(rest)])
            else:
                dirs.setdefault(d_old + '/', set()).add(None)
            d_old = os.path.dirname(d_old)
    for k, v in stems.items():
        if len(v) == 1:
            out.setdefault(k, v.pop())
    for k, v in dirs.items():
        moved_all = all(f in moves for f in old_files if f.startswith(k))
        if len(v) == 1 and None not in v and moved_all:
            out.setdefault(k, next(iter(v)))
    return out


def rewrite_references(text, refs, pattern):
    return pattern.sub(lambda m: refs[m.group(0)], text)


def main(argv=None):
    a = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    a.add_argument('stage', choices=list(STAGES))
    a.add_argument('--dry-run', action='store_true')
    o = a.parse_args(argv)
    stage = STAGES[o.stage]
    old_files = set(tracked())
    moves = {}
    for path in sorted(old_files):
        dest = destination(stage['rules'], path)
        if dest and dest != path:
            moves[path] = dest
    if not moves:
        print('nothing to move')
        return 0
    new_files = {moves.get(p, p) for p in old_files}
    if len(new_files) != len(old_files):
        sys.exit('two files would land on the same path')
    print('%d files move' % len(moves))
    if o.dry_run:
        for k, v in sorted(moves.items()):
            print('  %s -> %s' % (k, v))
        return 0

    for old, new in sorted(moves.items()):
        os.makedirs(os.path.join(ROOT, os.path.dirname(new)), exist_ok=True)
        git('mv', old, new)

    refs = reference_map(moves, old_files)
    pattern = re.compile(r'(?<![\w.-])(?<![\w.-]/)(?:%s)(?![\w])' % '|'.join(re.escape(k) for k in sorted(refs, key=len, reverse=True)))
    inverse = {v: k for k, v in moves.items()}
    text_files = set()
    for g in TEXT_GLOBS:
        text_files |= {f for f in new_files if fnmatch.fnmatchcase(f, g)}
    changed = 0
    for path in sorted(text_files):
        if path.startswith(SKIP) or not path.endswith(TEXT_EXT) or not os.path.isfile(os.path.join(ROOT, path)):
            continue
        if os.path.splitext(path)[1] == '' and '.' in os.path.basename(path)[1:]:
            continue
        full = os.path.join(ROOT, path)
        with open(full, 'rb') as f:
            raw = f.read()
        if b'\0' in raw:
            continue
        try:
            text = raw.decode('utf-8')
        except UnicodeDecodeError:
            continue
        new = text
        if path.endswith(SOURCE_EXT):
            new = rewrite_includes(new, inverse.get(path, path), path, stage, old_files, new_files, moves)
        new = rewrite_references(new, refs, pattern)
        if new != text:
            with open(full, 'wb') as f:
                f.write(new.encode('utf-8'))
            changed += 1
    print('%d files rewritten' % changed)
    return 0


if __name__ == '__main__':
    sys.exit(main())
