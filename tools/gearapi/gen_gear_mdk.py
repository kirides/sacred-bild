#!/usr/bin/env python3
"""Builds the Sacred Gear MDK (mod development kit) from the sources in this repository.

    python tools/gearapi/gen_gear_mdk.py            writes the kit to out/mdk/sacred-gear-mdk (not committed: CI builds and publishes it)
    python tools/gearapi/gen_gear_mdk.py --zip      ... and out/mdk/sacred-gear-mdk.zip
    python tools/gearapi/gen_gear_mdk.py --out DIR --check    fail if DIR is not what the sources produce

Nothing in the kit is a hand-kept copy: meta/ is every src/gear/meta/*.lua (so new definition files are picked up),
docs/GearApi.md is generated from them the same way gen_gear_api_docs.py does, the examples are copied from
src/gear/examples, and the rest (README, template, tools) lives in src/gear/mdk/.
"""
import argparse
import os
import shutil
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), *p)) for p in [(), ('..',), ('..', 'sigs'), ('..', 'gearapi')]]  # tools/, tools/sigs, tools/gearapi
import gear_meta  # noqa: E402
import gen_gear_api_docs  # noqa: E402

import paths  # noqa: E402

ROOT = paths.ROOT
META = paths.GEAR_META
MDK_SRC = paths.GEAR_MDK_SRC
EXAMPLES = ['hello', 'uidemo', 'saveinfo', 'hooks']
NAME = 'sacred-gear-mdk'


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


def copy_tree(src, dst):
    shutil.copytree(src, dst, ignore=shutil.ignore_patterns('__pycache__', '*.bmp'))


def build(out_dir):
    if os.path.isdir(out_dir):
        shutil.rmtree(out_dir)
    os.makedirs(out_dir)

    # meta/: every definition file. A file gen_gear_meta does not list would be missing from the reference.
    metas = sorted(f for f in os.listdir(META) if f.endswith('.lua'))
    unlisted = [f for f in metas if f not in gear_meta.FILES]
    if unlisted:
        sys.exit('src/gear/meta files missing from FILES in tools/gearapi/gear_meta.py (they would be left out of the API '
                 'reference): ' + ', '.join(unlisted))
    os.makedirs(os.path.join(out_dir, 'meta'))
    for f in metas:
        shutil.copyfile(os.path.join(META, f), os.path.join(out_dir, 'meta', f))

    shutil.copyfile(os.path.join(MDK_SRC, 'luarc.json'), os.path.join(out_dir, '.luarc.json'))
    os.makedirs(os.path.join(out_dir, '.vscode'))
    shutil.copyfile(os.path.join(MDK_SRC, 'vscode', 'extensions.json'), os.path.join(out_dir, '.vscode', 'extensions.json'))

    write(os.path.join(out_dir, 'docs', 'GearApi.md'), gen_gear_api_docs.generate())
    shutil.copyfile(paths.GEAR_MD, os.path.join(out_dir, 'docs', 'Gear.md'))

    copy_tree(os.path.join(MDK_SRC, 'example-mod'), os.path.join(out_dir, 'example-mod'))
    copy_tree(os.path.join(MDK_SRC, 'tools'), os.path.join(out_dir, 'tools'))
    for ex in EXAMPLES:
        copy_tree(os.path.join(paths.GEAR_EXAMPLES, ex), os.path.join(out_dir, 'examples', ex))
    os.makedirs(os.path.join(out_dir, 'src'))
    write(os.path.join(out_dir, 'src', '.gitkeep'), '')

    with open(os.path.join(MDK_SRC, 'README.md'), encoding='utf-8') as f:
        readme = f.read()
    write(os.path.join(out_dir, 'README.md'), readme)
    shutil.copyfile(os.path.join(ROOT, 'LICENSE'), os.path.join(out_dir, 'LICENSE'))
    return out_dir


def make_zip(src_dir, zip_path):
    os.makedirs(os.path.dirname(zip_path), exist_ok=True)
    if os.path.exists(zip_path):
        os.remove(zip_path)
    with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED) as z:
        for base, _, files in os.walk(src_dir):
            for fn in sorted(files):
                full = os.path.join(base, fn)
                z.write(full, os.path.relpath(full, src_dir).replace(os.sep, '/'))


def snapshot(d):
    out = {}
    for base, _, files in os.walk(d):
        for fn in files:
            full = os.path.join(base, fn)
            with open(full, 'rb') as f:
                out[os.path.relpath(full, d).replace(os.sep, '/')] = f.read().replace(b'\r\n', b'\n')
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', default=paths.GEAR_MDK_OUT)
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--zip', action='store_true')
    ap.add_argument('--zip-path', default=os.path.join(ROOT, 'out', 'mdk', NAME + '.zip'))
    args = ap.parse_args()
    out = os.path.abspath(args.out)
    if args.check:
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            fresh = build(os.path.join(tmp, 'mdk'))
            if not os.path.isdir(out) or snapshot(fresh) != snapshot(out):
                print('%s is out of date: run python tools/gearapi/gen_gear_mdk.py' % paths.rel(out))
                return 1
        print('%s is up to date' % paths.rel(out))
        return 0
    build(out)
    print('wrote', out)
    if args.zip:
        make_zip(out, args.zip_path)
        print('wrote', args.zip_path)
    return 0


if __name__ == '__main__':
    sys.exit(main())
