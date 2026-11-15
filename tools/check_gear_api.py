#!/usr/bin/env python3
"""Fails when Sacred Gear's LuaLS annotations (gear/meta/*.lua) and the code behind the Lua API have drifted apart.

    python tools/check_gear_api.py

It reads gear/lua/kernel.lua (the Lua side of the API) and gear/lua_host.cpp (the fields of the tables the native side
builds) as text and compares the names they define with the names the annotations declare, both ways:

  * in the code but not annotated  -> a new function, field, event, option or manifest key nobody documented;
  * annotated but not in the code  -> a stale annotation.

It also runs tools/gen_gear_api_docs.py --check (docs/GearApi.md is generated from the annotations).
Names are matched, not types and signatures: keep those right by hand. See docs/GearApi.md.
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gear_meta  # noqa: E402

ROOT = os.path.dirname(HERE)
KERNEL = os.path.join(ROOT, 'gear', 'lua', 'kernel.lua')
HOST = os.path.join(ROOT, 'gear', 'lua_host.cpp')
META = os.path.join(ROOT, 'gear', 'meta')

problems = []


def read(path):
    with open(path, encoding='utf-8', errors='replace', newline='') as f:
        return f.read().replace('\r\n', '\n')


def between(text, start, end, what):
    """The text from the line matching `start` up to (not including) the next match of `end`."""
    m = re.search(start, text, re.M)
    if not m:
        problems.append(f'cannot find {what} in the sources (pattern {start!r}): update tools/check_gear_api.py')
        return ''
    e = re.search(end, text[m.end():], re.M)
    return text[m.start(): m.end() + (e.start() if e else len(text))]


def compare(what, code, meta, hint=''):
    code = {n for n in code if not n.startswith('_')}
    meta = {n for n in meta if not n.startswith('_')}
    for n in sorted(code - meta):
        problems.append(f'{what}: "{n}" exists in the code but is not annotated{hint}')
    for n in sorted(meta - code):
        problems.append(f'{what}: "{n}" is annotated but not in the code (stale annotation?){hint}')


def keys_of_table_line(line):
    """Keys of a one-line table constructor: {a = 1, b = 2}."""
    inner = line[line.index('{') + 1: line.rindex('}')]
    return set(re.findall(r'(?:^|,)\s*(\w+)\s*=', inner))


def cpp_keys(src, start, what):
    """The field names the native function starting at `start` puts into the table it returns."""
    body = between(src, start, r'^    }\s*$', what)
    keys = set(re.findall(r'\b(?:set|num|flag)\("(\w+)"', body))
    keys |= set(re.findall(r'lua_setfield\(L, -2, "(\w+)"\)', body))
    return keys


def main():
    model = gear_meta.parse(META)
    kernel = read(KERNEL)
    host = read(HOST)

    def members(cls):
        return model.class_members(cls)

    def fields(cls):
        return {f.name for f in model.classes[cls].fields}

    # --- the gear table
    env = between(kernel, r'^local function make_env\(mod\)', r'^-- mod\.lua: a table literal', 'make_env')
    gear = set(re.findall(r'^\s*function gear\.(\w+)\(', env, re.M))
    gear |= set(re.findall(r'(?<![\w.])gear\.(\w+)\s*=(?!=)', env))
    ctor = between(env, r'^\s*local gear = \{', r'^\s*\}\s*$', 'the gear table constructor')
    gear |= set(re.findall(r'^\s{8}(\w+)\s*=', ctor, re.M))
    compare('gear', gear, members('Gear'))

    gear_info = re.search(r'mod = (\{[^\n]*\})', env)
    if gear_info:
        compare('gear.mod', keys_of_table_line(gear_info.group(1)), fields('GearModInfo'))

    # --- the sandbox
    os_line = re.search(r'os = (\{[^\n]*\})', env)
    if os_line:
        compare('os', keys_of_table_line(os_line.group(1)), fields('GearOs'))
    for name in re.findall(r'^\s*env\.(\w+)\s*=(?!=)', env, re.M):
        if name in ('print', 'require'):
            if name not in {g.name for g in model.globals}:
                problems.append(f'global {name} is not annotated')

    # --- gear.world
    world = set(re.findall(r'^\s*function world\.(\w+)\(', kernel, re.M))
    world |= set(re.findall(r'(?<![\w.])world\.(\w+)\s*=(?!=)', kernel))
    world |= set(re.findall(r'(?<![\w.])w\.(\w+)\s*=(?!=)', env))
    world |= keys_of_table_line(re.search(r'^local world = (\{[^\n]*\})', kernel, re.M).group(1))
    compare('gear.world', world, members('GearWorld'))

    creature = set(re.findall(r'^\s*function creature_mt:(\w+)\(', kernel, re.M))
    compare('creature', creature, {f.name for f in model.classes['Creature'].funcs})

    # --- gear.ui
    ui_src = between(kernel, r'^local function make_ui\(mod\)', r'^-- Custom events', 'make_ui')
    ui = set(re.findall(r'^\s*function ui\.(\w+)\(', ui_src, re.M))
    ui |= set(re.findall(r'(?<![\w.])ui\.(\w+)\s*=(?!=)', ui_src))
    ui |= keys_of_table_line(re.search(r'local ui = (\{[^\n]*\})', ui_src).group(1))
    compare('gear.ui', ui, members('GearUi'))

    # --- gear.auto
    auto_src = between(kernel, r'^local function make_auto\(\)', r'^-- What a mod\'s code sees', 'make_auto')
    auto = set(re.findall(r'^\s*function auto\.(\w+)\(', auto_src, re.M))
    auto |= keys_of_table_line(re.search(r'local auto = (\{[^\n]*\})', auto_src).group(1))
    compare('gear.auto', auto, members('GearAuto'))

    # --- data, json, pipes
    data_ctor = between(kernel, r'^local data = \{', r'^\}', 'data')
    compare('gear.data', set(re.findall(r'^\s{4}(\w+)\s*=', data_ctor, re.M)) | set(re.findall(r'^data\.(\w+) = ', kernel, re.M)),
            fields('GearData'))
    abil = set(re.findall(r'^function abilities:(\w+)\(', kernel, re.M))
    compare('gear.data.abilities', abil, {f.name for f in model.classes['GearAbilities'].funcs})
    abil_fields = set(re.findall(r"(\w+) = \{0x[0-9A-Fa-f]+, '", kernel)) | {'table', 'index'}
    compare('ability record', abil_fields, fields('GearAbility'))
    text_src = re.search(r'^\s{8}text = \{(.*?)^\s{8}\},', kernel, re.M | re.S)
    compare('gear.text', set(re.findall(r'^\s{12}(\w+) = function', text_src.group(1), re.M)) if text_src else set(), members('GearText'))
    settings_src = re.search(r'^\s{8}settings = \{(.*?)^\s{8}\},', kernel, re.M | re.S)
    compare('gear.settings', set(re.findall(r'^\s{12}(\w+) = function', settings_src.group(1), re.M)) if settings_src else set(),
            members('GearSettings'))
    view = set(re.findall(r'^\s*function view:(\w+)\(', kernel, re.M))
    compare('data table', view, {f.name for f in model.classes['GearTable'].funcs})
    otypes = set(re.findall(r'^function data\.object_types:(\w+)\(', kernel, re.M))
    compare('gear.data.object_types', otypes, {f.name for f in model.classes['GearObjectTypes'].funcs})
    kinds = re.search(r"for _, kind in ipairs\(\{([^}]*)\}\)", kernel)
    rec = set(re.findall(r"'(\w+)'", kinds.group(1))) if kinds else set()
    rec |= set(re.findall(r'^\s*function record_mt:(\w+)\(', kernel, re.M))
    compare('record', rec, {f.name for f in model.classes['GearRecord'].funcs})
    json = set(re.findall(r'^\s*function json\.(\w+)\(', kernel, re.M))
    json |= keys_of_table_line(re.search(r'^local json = (\{[^\n]*)', kernel, re.M).group(1) + '}')
    compare('gear.json', json, members('GearJson'))
    pipe_mod = re.search(r'pipe = \{(\w+) =', env)
    compare('gear.pipe', {pipe_mod.group(1)} if pipe_mod else set(), members('GearPipeModule'))
    pipe_src = between(kernel, r'^local function open_pipe\(', r'^local PIPE_EVENTS_PER_FRAME', 'open_pipe')
    pipe = set(re.findall(r'^\s*function p:(\w+)\(', pipe_src, re.M))
    pipe |= set(re.findall(r'e\.object\.(on_\w+)', kernel))
    pipe |= keys_of_table_line(re.search(r'local p = (\{[^\n]*\})', pipe_src).group(1))
    compare('pipe', pipe, members('GearPipe'))

    # --- events
    events = set(re.findall(r"kernel\.dispatch\('([\w.]+)'", kernel))
    meta_events = set()
    for fn in model.globals + [f for c in model.classes.values() for f in c.funcs]:
        if fn.name == 'on':
            for o in fn.overloads:
                m = re.search(r"event: '([\w.]+)'", o)
                if m:
                    meta_events.add(m.group(1))
    alias_events = {v.strip("'") for v, _ in model.aliases['GearEventName'].values}
    compare('events (gear.on overloads)', events, meta_events)
    compare('events (GearEventName)', events, alias_events)

    # --- mod.lua
    manifest = between(kernel, r'^local function read_manifest\(dir\)', r'^-- Mods in load order', 'read_manifest')
    compare('mod.lua', set(re.findall(r'\bm\.(\w+)', manifest)), fields('GearModManifest'))

    # --- options: every opts.X the kernel reads must be a field of some options class
    opt_fields = set()
    for name, c in model.classes.items():
        if name.endswith('Opts'):
            opt_fields |= {f.name for f in c.fields}
    for n in sorted(set(re.findall(r'\bopts\.(\w+)', kernel)) - opt_fields):
        problems.append(f'opts: "{n}" is read by kernel.lua but is a field of no *Opts class')
    rules = set(re.findall(r'\brules\.(\w+)', kernel))
    compare('pickup rules', rules, fields('GearPickupRules'))

    # --- tables the native side builds
    compare('ui state', cpp_keys(host, r'^    int nativeUiState\(', 'nativeUiState'), fields('GearUiState'))
    win = cpp_keys(host, r'^    void pushWindow\(', 'pushWindow') - {'w', 'h'} | {'bounds'}   # w, h: the bounds table's
    compare('ui window', win, fields('GearWindow'))
    compare('ui window bounds', {'x', 'y', 'w', 'h'}, fields('GearRect'))
    img = cpp_keys(host, r'^    int nativeUiImageInfo\(', 'nativeUiImageInfo') | {'id'}
    compare('ui image', img, fields('GearImageInfo'))
    compare('auto state', cpp_keys(host, r'^    int nativeAutoState\(', 'nativeAutoState'), fields('GearAutoState'))
    compare('auto ui node', cpp_keys(host, r'^    int nativeAutoUi\(', 'nativeAutoUi'), fields('GearAutoUiNode'))
    obj = cpp_keys(host, r'^    int nativeObject\(', 'nativeObject')
    compare('creature:info()', obj, fields('GearObjectInfo'))
    item = cpp_keys(host, r'^    int nativeItem\(', 'nativeItem')
    witem = between(kernel, r'^function world\.item\(id\)', r'^end', 'world.item')
    item |= set(re.findall(r'\bit\.(\w+)\s*(?:,\s*it\.\w+\s*)?=(?!=)', witem))
    item |= {'potion_color'}
    compare('item', item, fields('GearItem'))

    # --- generated reference
    r = subprocess.run([sys.executable, os.path.join(HERE, 'gen_gear_api_docs.py'), '--check'],
                       capture_output=True, text=True)
    if r.returncode != 0:
        problems.append((r.stdout + r.stderr).strip())

    if problems:
        print('Sacred Gear API drift:')
        for p in problems:
            print('  ' + p)
        print('Fix gear/meta/*.lua (and run python tools/gen_gear_api_docs.py), or update this check if the code changed shape.')
        return 1
    print('Sacred Gear API annotations match the code.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
