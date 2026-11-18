#!/usr/bin/env python3
"""Builds the Sacred Gear MDK (mod development kit) from the sources in this repository.

    python tools/gen_gear_mdk.py                          writes out/mdk/sacred-gear-mdk/
    python tools/gen_gear_mdk.py --zip                    ... and out/mdk/sacred-gear-mdk.zip
    python tools/gen_gear_mdk.py --out DIR --version v1.2

Nothing in the kit is a hand-kept copy: meta/ is every gear/meta/*.lua (so new definition files are picked up),
docs/GearApi.md is generated from them the same way gen_gear_api_docs.py does, the examples are copied from
gear/examples, and the rest (README, template, tools) lives in gear/mdk/.
"""
import argparse
import os
import shutil
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gear_meta  # noqa: E402
import gen_gear_api_docs  # noqa: E402

ROOT = os.path.dirname(HERE)
META = os.path.join(ROOT, 'gear', 'meta')
MDK_SRC = os.path.join(ROOT, 'gear', 'mdk')
EXAMPLES = ['hello', 'uidemo', 'saveinfo']
NAME = 'sacred-gear-mdk'


def git(*args):
    try:
        return subprocess.check_output(['git', '-C', ROOT, *args], text=True, stderr=subprocess.DEVNULL).strip()
    except Exception:
        return ''


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


def copy_tree(src, dst):
    shutil.copytree(src, dst, ignore=shutil.ignore_patterns('__pycache__', '*.bmp'))


def build(out_dir, version, commit):
    if os.path.isdir(out_dir):
        shutil.rmtree(out_dir)
    os.makedirs(out_dir)

    # meta/: every definition file. A file gen_gear_meta does not list would be missing from the reference.
    metas = sorted(f for f in os.listdir(META) if f.endswith('.lua'))
    unlisted = [f for f in metas if f not in gear_meta.FILES]
    if unlisted:
        sys.exit('gear/meta files missing from FILES in tools/gear_meta.py (they would be left out of the API '
                 'reference): ' + ', '.join(unlisted))
    os.makedirs(os.path.join(out_dir, 'meta'))
    for f in metas:
        shutil.copyfile(os.path.join(META, f), os.path.join(out_dir, 'meta', f))

    shutil.copyfile(os.path.join(MDK_SRC, 'luarc.json'), os.path.join(out_dir, '.luarc.json'))
    os.makedirs(os.path.join(out_dir, '.vscode'))
    shutil.copyfile(os.path.join(MDK_SRC, 'vscode', 'extensions.json'), os.path.join(out_dir, '.vscode', 'extensions.json'))

    write(os.path.join(out_dir, 'docs', 'GearApi.md'), gen_gear_api_docs.generate())
    shutil.copyfile(os.path.join(ROOT, 'docs', 'Gear.md'), os.path.join(out_dir, 'docs', 'Gear.md'))

    copy_tree(os.path.join(MDK_SRC, 'template'), os.path.join(out_dir, 'template'))
    copy_tree(os.path.join(MDK_SRC, 'tools'), os.path.join(out_dir, 'tools'))
    for ex in EXAMPLES:
        copy_tree(os.path.join(ROOT, 'gear', 'examples', ex), os.path.join(out_dir, 'examples', ex))
    os.makedirs(os.path.join(out_dir, 'src'))
    write(os.path.join(out_dir, 'src', '.gitkeep'), '')

    with open(os.path.join(MDK_SRC, 'README.md'), encoding='utf-8') as f:
        readme = f.read().replace('@VERSION@', version).replace('@COMMIT@', commit or 'unknown')
    write(os.path.join(out_dir, 'README.md'), readme)
    shutil.copyfile(os.path.join(ROOT, 'LICENSE'), os.path.join(out_dir, 'LICENSE'))
    return out_dir


def make_zip(src_dir, zip_path):
    if os.path.exists(zip_path):
        os.remove(zip_path)
    parent = os.path.dirname(src_dir)
    with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED) as z:
        for base, _, files in os.walk(src_dir):
            for fn in sorted(files):
                full = os.path.join(base, fn)
                z.write(full, os.path.relpath(full, parent).replace(os.sep, '/'))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'mdk', NAME))
    ap.add_argument('--version', default=None, help='default: git describe')
    ap.add_argument('--zip', action='store_true', help=f'also write <out>/../{NAME}.zip')
    args = ap.parse_args()

    commit = git('rev-parse', '--short=9', 'HEAD')
    version = args.version or git('describe', '--tags', '--always') or ('git-' + commit if commit else 'dev')
    out = os.path.abspath(args.out)
    build(out, version, commit)
    print('wrote', out)
    if args.zip:
        z = os.path.join(os.path.dirname(out), NAME + '.zip')
        make_zip(out, z)
        print('wrote', z)
    return 0


if __name__ == '__main__':
    sys.exit(main())
