#!/usr/bin/env python3
"""Raise the version of Sacred-Bild (ddraw.dll) and Sacred Gear (SacredGear.dll) from the git history.

Every component has one flat text file with its version, a few lines like `1.2.3` and a newline:

    src/bild/SacredBild_version.txt      ddraw.dll
    gear/SacredGear_version.txt     SacredGear.dll

For each component the history since the last commit that changed its version file (a new file counts as that commit) is read:
  * any change to a file of the component that is not a test file     -> patch   (1.2.3 -> 1.2.4)
  * a commit message containing  feat(<component>)  or starting with  feat:   -> minor   (1.2.3 -> 1.3.0)
  * a commit message containing  break(<component>) or starting with  break:  -> major   (1.2.3 -> 2.0.0), as does  feat!: / feat(x)!:
The highest level wins. Without a non-test change nothing is bumped. A scope names the component (`feat(gear)`, `feat(sacred-gear)`,
`feat(bild)`, `feat(ddraw)`, ...), several can be listed (`feat(gear, bild)`); `feat:` without a scope counts for every component that
commit changes. Scopes are compared trimmed, case-insensitive, with '-' and '_' alike.

    python tools/bump_versions.py              # show what would change
    python tools/bump_versions.py --write      # edit the version files (and refresh mod-index.json)
    python tools/bump_versions.py --commit     # --write, one commit per component ("chore(sacred-gear): bump version a -> b (level)"), then the index
    python tools/bump_versions.py --check      # exit 1 when a bump is pending (used by the pre-push hook)
    python tools/bump_versions.py --ref SHA --check      # the same for another commit than HEAD (--write/--commit need HEAD)
"""
import argparse, os, re, subprocess, sys

LEVELS = ['patch', 'minor', 'major']
SCOPE = re.compile(r'\b(feat|break)\s*\(([^)\n]*)\)(!?)', re.I)
UNSCOPED = re.compile(r'^(feat|break)(!?):', re.I | re.M)
VERSION = re.compile(r'(\d+)(?:\.(\d+))?(?:\.(\d+))?')


class Component:
    def __init__(self, name, id, version_file, include, exclude, aliases):
        self.name, self.id, self.version_file = name, id, version_file
        self.include, self.exclude = include, exclude  # path prefixes ('dir/' or an exact file)
        self.aliases = {norm(a) for a in aliases} | {norm(id)}


def norm(name):
    return re.sub(r'[\s_]+', '-', name.strip().lower()).replace('--', '-')


COMPONENTS = [
    # ddraw.dll: the SacredBild sources and what they are built with. gear/ is a separate DLL.
    Component('SacredBild', 'sacred-bild', 'src/bild/SacredBild_version.txt',
              ['src/bild/', 'src/core/', 'CMakeLists.txt', 'CMakePresets.json', 'vcpkg.json', 'ports/', 'triplets/'],
              [], ['bild', 'sacredbild', 'ddraw', 'sacred-bild']),
    # SacredGear.dll: gear/ plus the sources it shares with SacredBild (sacred_core, src/core/: its code and the game's classes). The examples, the MDK, its generated copy and the
    # API annotations are documentation, not part of the DLL.
    Component('SacredGear', 'sacred-gear', 'gear/SacredGear_version.txt',
              ['gear/', 'src/core/'],
              ['gear/examples/', 'gear/dist/', 'gear/mdk/', 'gear/meta/'], ['gear', 'sacredgear', 'sacred-gear']),
]


def git(repo, *args, check=True):
    r = subprocess.run(['git', '-C', repo, *args], capture_output=True, text=True, encoding='utf-8', errors='replace')
    if check and r.returncode != 0:
        sys.exit('git %s failed: %s' % (' '.join(args), r.stderr.strip()))
    return r.stdout


def is_test_path(rel):
    """rel: path with forward slashes. Folders called test/tests/tools/changelog (release notes never change the DLLs) or ending with test(s), files ending with test(s) or test_*."""
    parts = rel.lower().split('/')
    for d in parts[:-1]:
        if d in ('test', 'tests', 'tools', 'changelog') or d.endswith(('test', 'tests')):
            return True
    stem = os.path.splitext(parts[-1])[0]
    return stem.endswith(('test', 'tests')) or stem.startswith('test_')


def belongs(comp, rel):
    """Is this changed file a non-test part of the component?"""
    if rel in (c.version_file for c in COMPONENTS) or is_test_path(rel):
        return False
    return any(rel.startswith(p) for p in comp.include) and not any(rel.startswith(p) for p in comp.exclude)


def bump(version, level):
    m = VERSION.fullmatch(version.strip())
    if not m:
        return None
    major, minor, patch = (int(x or 0) for x in m.groups())
    if level == 'major':
        return '%d.0.0' % (major + 1)
    if level == 'minor':
        return '%d.%d.0' % (major, minor + 1)
    return '%d.%d.%d' % (major, minor, patch + 1)


def read_version(text):
    first = text.strip().splitlines()[0].strip() if text.strip() else ''
    return first if VERSION.fullmatch(first) else None


def last_bump(repo, ref, rel):
    """The newest commit that changed the version file (its creation counts), or None. The file's history is followed through
    renames, and a commit that only moved it (a rename with the same content) is not a bump: moving the version file with its code
    must not drop the changes made since the last real bump."""
    raw = git(repo, 'log', '--follow', '--name-status', '--format=%x01%H', ref, '--', rel)
    for block in raw.split('\x01')[1:]:
        lines = [l for l in block.splitlines() if l.strip()]
        if not lines:
            continue
        sha, statuses = lines[0].strip(), [l.split('\t', 1)[0] for l in lines[1:]]
        if statuses and all(s == 'R100' for s in statuses):
            continue
        return sha
    return None


def message_levels(message, comp):
    """The highest level this commit message asks for the component (patch when it asks for none)."""
    level = 0
    for kind, inner, bang in SCOPE.findall(message):
        scopes = {norm(s) for s in re.split(r'[,;/&]', inner) if s.strip()}
        if comp.aliases & scopes:
            level = max(level, 2 if bang or kind.lower() == 'break' else 1)
    for kind, bang in UNSCOPED.findall(message):
        level = max(level, 2 if bang or kind.lower() == 'break' else 1)
    return level


def plan(repo, ref):
    """One entry per component: its current version, the pending level (None for nothing) and the commits behind it."""
    out = []
    for comp in COMPONENTS:
        text = git(repo, 'show', '%s:%s' % (ref, comp.version_file), check=False)
        version = read_version(text)
        base = last_bump(repo, ref, comp.version_file)
        if version is None or base is None:
            out.append({'comp': comp, 'skip': '%s is missing or is not a version (a.b.c)' % comp.version_file})
            continue
        rng = '%s..%s' % (base, ref)
        raw = git(repo, 'log', '--no-merges', '--name-only', '--format=%x01%H%x02%B%x02', rng)
        level, commits = -1, 0
        for block in raw.split('\x01')[1:]:
            head, msg, files = block.split('\x02', 2)
            touched = any(belongs(comp, f.strip()) for f in files.splitlines() if f.strip())
            if touched:  # a feat/break message counts only for a commit that really changed the component
                commits += 1
                level = max(level, message_levels(msg, comp))
        out.append({'comp': comp, 'version': version, 'level': LEVELS[level] if commits else None, 'commits': commits, 'base': base[:8]})
    return out


def commit_paths(repo, paths, message):
    """Commit exactly these files on HEAD with plumbing. `git commit -- <path>` refuses ("partial commit during a merge") when it runs in the
    post-merge hook, where MERGE_HEAD still exists, and a plain `git commit` would make a second merge commit."""
    git(repo, 'add', '--', *paths)
    tree = git(repo, 'write-tree').strip()
    sha = git(repo, 'commit-tree', tree, '-p', 'HEAD', '-m', message).strip()
    git(repo, 'update-ref', '-m', 'commit: ' + message.splitlines()[0], 'HEAD', sha)


def write_version(repo, comp, new):
    path = os.path.join(repo, comp.version_file)
    with open(path, 'rb') as fh:
        eol = '\r\n' if b'\r\n' in fh.read() else '\n'
    with open(path, 'w', encoding='utf-8', newline='') as fh:
        fh.write(new + eol)


def main(argv=None):
    a = argparse.ArgumentParser(description='Raise the Sacred-Bild and Sacred Gear versions from the git history.')
    a.add_argument('--repo', default='.', help='the repository (default: current folder)')
    a.add_argument('--ref', default='HEAD', help='the history to read (default HEAD; run it on main)')
    a.add_argument('--write', action='store_true', help='edit the version files')
    a.add_argument('--commit', action='store_true', help='--write and one commit per component, then refresh mod-index.json')
    a.add_argument('--check', action='store_true', help='exit 1 when a bump is pending')
    a.add_argument('--no-index', action='store_true', help='do not refresh mod-index.json')
    o = a.parse_args(argv)
    repo = os.path.abspath(git(o.repo, 'rev-parse', '--show-toplevel').strip())
    ref = git(repo, 'rev-parse', '--verify', o.ref + '^{commit}').strip()
    if (o.write or o.commit) and ref != git(repo, 'rev-parse', 'HEAD').strip():
        sys.exit('--write/--commit work on HEAD only')
    if o.commit and git(repo, 'diff', '--cached', '--name-only').strip():
        sys.exit('--commit needs an empty staging area (something is staged)')

    pending = []
    for r in plan(repo, ref):
        name = r['comp'].name
        if 'skip' in r:
            print('skip   %-12s %s' % (name, r['skip']))
        elif r['level'] is None:
            print('same   %-12s %s' % (name, r['version']))
        else:
            new = bump(r['version'], r['level'])
            r['new'] = new
            pending.append(r)
            print('%-6s %-12s %s -> %s  (%d commit(s) since %s)' % (r['level'], name, r['version'], new, r['commits'], r['base']))
    if not pending:
        print('nothing to bump')
        return 0
    if o.check:
        print('a version bump is pending: run python tools/bump_versions.py --commit', file=sys.stderr)
        return 1
    if not (o.write or o.commit):
        print('\n%d component(s) would change; run with --write or --commit' % len(pending))
        return 0

    for r in pending:
        comp = r['comp']
        write_version(repo, comp, r['new'])
        if o.commit:
            commit_paths(repo, [comp.version_file], 'chore(%s): bump version %s -> %s (%s)' % (comp.id, r['version'], r['new'], r['level']))
    index = os.path.join(repo, 'mod-index.json')
    tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'make_mod_index.py')
    if not o.no_index and os.path.isfile(index) and os.path.isfile(tool):
        subprocess.run([sys.executable, tool, '--repo', repo], check=False)
        if o.commit and git(repo, 'status', '--porcelain', '--', 'mod-index.json').strip():
            commit_paths(repo, ['mod-index.json'], 'chore: refresh mod-index.json after version bumps')
    print('%d component(s) %s' % (len(pending), 'bumped and committed' if o.commit else 'bumped (not committed)'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
