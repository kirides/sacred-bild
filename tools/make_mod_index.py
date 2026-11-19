#!/usr/bin/env python3
"""Write mod-index.json: the index the Gear Mod Manager reads to install and update Sacred-Bild (ddraw.dll) and Sacred Gear (SacredGear.dll).

The index (schema 1, kinds, install rules and GitHub release assets are described in the mod manager's docs/ModIndex.md) is generated from
ITEMS below and the version files, so a version is never copied by hand:

    python tools/make_mod_index.py            # rewrite mod-index.json
    python tools/make_mod_index.py --check    # CI: exit 1 with a diff when the file is out of date
"""
import argparse, difflib, json, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bump_versions as bv

REPOSITORY = 'kirides/sacred-bild'
IMAGES = 'https://raw.githubusercontent.com/%s/main/docs/images/' % REPOSITORY  # thumbnails: docs/images/mod-index-<id>.png (1420x800)
# What the release workflow publishes per push: SacredBild-v<version>.zip (ddraw.dll, SacredBild.ini, version file) and
# SacredGear-v<version>.zip (SacredGear.dll, version file), laid out like the game folder. Nightlies are pre-releases.
ITEMS = {
    'sacred-bild': {
        'kind': 'plugin',
        'path': '',
        'platforms': ['win'],
        'name': 'Sacred-Bild',
        'author': 'kirides',
        'description': 'Sacred Gold on a modern renderer (ddraw.dll): Direct3D 9 output, window scaling, controller support, UDP multiplayer and more.',
        'tags': ['graphics', 'controller', 'multiplayer'],
        'thumbnail': IMAGES + 'mod-index-sacred-bild.png',
        'install': {'files': ['ddraw.dll']},
        'download': {'github': REPOSITORY, 'asset': 'SacredBild-v*.zip', 'prerelease': True},
    },
    'sacred-gear': {
        'kind': 'plugin',
        'path': '',
        'platforms': ['win'],
        'name': 'Sacred Gear',
        'author': 'kirides',
        'description': 'The Lua mod kit for Sacred Gold (SacredGear.dll). Runs next to Sacred-Bild or on its own; the Gear mods need it.',
        'tags': ['tools'],
        'thumbnail': IMAGES + 'mod-index-sacred-gear.png',
        'install': {'files': ['SacredGear.dll']},
        'download': {'github': REPOSITORY, 'asset': 'SacredGear-v*.zip', 'prerelease': True},
    },
}
ORDER = ['id', 'kind', 'path', 'platforms', 'name', 'version', 'author', 'description', 'tags', 'thumbnail', 'install', 'download']


def build(repo):
    mods = []
    for comp in bv.COMPONENTS:
        with open(os.path.join(repo, comp.version_file), encoding='utf-8-sig') as fh:
            version = bv.read_version(fh.read())
        if version is None:
            sys.exit('%s does not hold a version (a.b.c)' % comp.version_file)
        item = dict(ITEMS[comp.id], id=comp.id, version=version)
        mods.append({k: item[k] for k in ORDER if k in item})
    return {
        'schema': 1,
        'name': 'Sacred-Bild',
        'author': 'kirides',
        'description': 'Sacred-Bild and Sacred Gear, the plugins for Sacred Gold. The Gear mods have an index of their own (sacred-gear-mods).',
        'homepage': 'https://github.com/' + REPOSITORY,
        'mods': mods,
        'indexes': [{'url': 'https://raw.githubusercontent.com/kirides/sacred-gear-mods/main/mod-index.json', 'name': 'Sacred Gear mods'}],
    }


def main(argv=None):
    a = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    a.add_argument('--repo', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
    a.add_argument('--check', action='store_true', help='write nothing; exit 1 when the file differs')
    o = a.parse_args(argv)
    out = os.path.join(o.repo, 'mod-index.json')
    text = json.dumps(build(o.repo), indent=2, ensure_ascii=False) + '\n'
    old = ''
    if os.path.exists(out):
        with open(out, encoding='utf-8-sig', newline='') as fh:
            old = fh.read().replace('\r\n', '\n')
    if o.check:
        if text == old:
            return 0
        sys.stdout.writelines(difflib.unified_diff(old.splitlines(True), text.splitlines(True), 'mod-index.json (on disk)', 'mod-index.json (generated)'))
        print('mod-index.json is out of date: run python tools/make_mod_index.py', file=sys.stderr)
        return 1
    if text != old:
        with open(out, 'w', encoding='utf-8', newline='\n') as fh:
            fh.write(text)
    print('mod-index.json: %s' % ', '.join('%s %s' % (m['id'], m['version']) for m in json.loads(text)['mods']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
