#!/usr/bin/env python3
"""Generate changelog/<component>/history.md from the version files changelog/<component>/vX.Y.Z.md.

Components: bild (ddraw.dll) and gear (SacredGear.dll), each with its own folder.
history.md is the version files, newest version first, separated by two line breaks (one blank line).
Every version file starts with its own `## vX.Y.Z (date)` heading and holds short player-facing bullets.
With --check nothing is written; the exit code is 1 when a history.md is missing or out of date.
"""
import argparse, os, re, sys

VERSION = re.compile(r'^v(\d+(?:\.\d+)*)\.md$')


def key(name):
    return tuple(int(x) for x in VERSION.match(name).group(1).split('.'))


def build(folder):
    files = sorted((n for n in os.listdir(folder) if VERSION.match(n)), key=key, reverse=True)
    if not files:
        return None
    parts = []
    for n in files:
        with open(os.path.join(folder, n), encoding='utf-8-sig') as fh:
            parts.append(fh.read().replace('\r\n', '\n').strip('\n'))
    return '\n\n'.join(parts) + '\n'


def main():
    a = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    a.add_argument('--root', default='changelog')
    a.add_argument('--check', action='store_true')
    o = a.parse_args()
    bad = n = 0
    for comp in sorted(os.listdir(o.root)):
        folder = os.path.join(o.root, comp)
        text = build(folder) if os.path.isdir(folder) else None
        if text is None:
            continue
        n += 1
        target = os.path.join(folder, 'history.md')
        have = open(target, encoding='utf-8', newline='').read().replace('\r\n', '\n') if os.path.exists(target) else None
        if have == text:
            continue
        if o.check:
            print('%s is out of date: run python tools/make_changelog.py' % target, file=sys.stderr)
            bad += 1
        else:
            with open(target, 'w', encoding='utf-8', newline='\n') as fh:
                fh.write(text)
    print('%d changelog histories %s' % (n, 'checked' if o.check else 'written'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
