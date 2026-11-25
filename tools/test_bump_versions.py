"""Tests for tools/bump_versions.py and tools/make_mod_index.py: python tools/test_bump_versions.py (builds throw-away git repositories)."""
import contextlib, io, json, os, subprocess, sys, tempfile, unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bump_versions as bv
import make_mod_index as mi

ENV = dict(os.environ, GIT_AUTHOR_NAME='t', GIT_AUTHOR_EMAIL='t@t', GIT_COMMITTER_NAME='t', GIT_COMMITTER_EMAIL='t@t',
           GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_SYSTEM=os.devnull)
os.environ.update({k: v for k, v in ENV.items() if k.startswith('GIT_')})  # bump_versions.py commits with the process environment
BILD, GEAR = 'src/SacredBild_version.txt', 'gear/SacredGear_version.txt'


class Repo:
    def __init__(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name
        self.n = 0
        self.git('init', '-q', '-b', 'main')

    def git(self, *a):
        r = subprocess.run(['git', '-C', self.dir, *a], capture_output=True, text=True, env=ENV)
        assert r.returncode == 0, r.stderr
        return r.stdout

    def write(self, rel, text):
        p = os.path.join(self.dir, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'w', encoding='utf-8', newline='') as f:
            f.write(text)

    def commit(self, msg, files):
        for rel, text in files.items():
            self.write(rel, text)
        self.n += 1
        self.git('add', '-A')
        self.git('commit', '-q', '-m', msg)

    def touch(self, rel, msg):
        self.commit(msg, {rel: '// %d\n' % self.n})

    def run(self, *args):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = bv.main(['--repo', self.dir, *args])
        return code, out.getvalue()

    def read(self, rel):
        with open(os.path.join(self.dir, rel), encoding='utf-8', newline='') as f:
            return f.read()


class Tests(unittest.TestCase):
    def setUp(self):
        self.r = Repo()
        self.addCleanup(self.r.tmp.cleanup)
        self.r.commit('start', {BILD: '1.2.3\n', GEAR: '0.9\n', 'src/main.cpp': '//\n', 'gear/main.cpp': '//\n'})

    def plan(self, *args):
        return self.r.run(*args)[1]

    def test_nothing_changed_nothing_bumped(self):
        code, out = self.r.run()
        self.assertEqual(code, 0)
        self.assertIn('nothing to bump', out)

    def test_test_and_doc_files_do_not_bump(self):
        for rel in ('src/net/udp_test.cpp', 'src/tests/a.cpp', 'gear/tests/a.lua', 'gear/examples/hello/main.lua', 'gear/mdk/README.md',
                    'gear/dist/mdk/x.lua', 'gear/meta/ui.lua', 'docs/Gear.md', 'tools/x.py', 'matchmaker/main.go'):
            self.r.touch(rel, 'not code of a DLL')
        self.assertIn('nothing to bump', self.plan())

    def test_a_change_bumps_only_its_own_component(self):
        self.r.touch('src/render/x.cpp', 'fix something')
        out = self.plan()
        self.assertIn('patch  SacredBild   1.2.3 -> 1.2.4', out)
        self.assertIn('same   SacredGear', out)
        self.r.touch('gear/lua/kernel.lua', 'fix gear')
        self.assertIn('patch  SacredGear   0.9 -> 0.9.1', self.plan())

    def test_the_shared_core_belongs_to_both(self):
        self.r.touch('src/log.cpp', 'shared')
        out = self.plan()
        self.assertEqual(out.count('patch'), 2)

    def test_feat_in_the_message_is_minor_trimmed_and_case_insensitive(self):
        self.r.touch('gear/x.cpp', 'Feat(  Sacred_Gear ): shiny')
        self.assertIn('0.9 -> 0.10.0', self.plan())

    def test_break_is_major_and_wins_over_feat(self):
        self.r.touch('src/x.cpp', 'feat(bild): a')
        self.r.touch('src/x.cpp', 'break(ddraw): b')
        self.assertIn('1.2.3 -> 2.0.0', self.plan())

    def test_bang_is_major(self):
        self.r.touch('src/x.cpp', 'feat(bild)!: renamed the ini')
        self.assertIn('1.2.3 -> 2.0.0', self.plan())

    def test_scope_list(self):
        self.r.touch('src/log.cpp', 'feat(gear, bild): both')
        self.assertEqual(self.plan().count('minor'), 2)

    def test_unscoped_feat_counts_for_the_components_the_commit_changed(self):
        self.r.touch('src/render/x.cpp', 'feat: new thing')
        out = self.plan()
        self.assertIn('minor  SacredBild   1.2.3 -> 1.3.0', out)
        self.assertIn('same   SacredGear', out)

    def test_feat_of_another_component_does_not_count(self):
        self.r.touch('src/x.cpp', 'feat(gear): not mine')
        self.assertIn('patch  SacredBild', self.plan())

    def test_feat_without_a_code_change_bumps_nothing(self):
        self.r.touch('src/tests/a.cpp', 'feat(bild): only tests')
        self.assertIn('nothing to bump', self.plan())

    def test_write_edits_the_version_only_and_keeps_crlf(self):
        self.r.commit('crlf', {BILD: '1.2.3\r\n'})
        self.r.touch('src/x.cpp', 'x')
        self.r.run('--write', '--no-index')
        self.assertEqual(self.r.read(BILD), '1.2.4\r\n')
        self.assertEqual(self.r.read(GEAR), '0.9\n')

    def test_commit_makes_the_next_run_quiet_and_counts_from_the_bump(self):
        self.r.touch('src/x.cpp', 'x')
        code, _ = self.r.run('--commit', '--no-index')
        self.assertEqual(code, 0)
        self.assertEqual(self.r.read(BILD), '1.2.4\n')
        self.assertIn('chore(sacred-bild): bump version 1.2.3 -> 1.2.4 (patch)', self.r.git('log', '-1', '--format=%s'))
        self.assertIn('nothing to bump', self.plan())
        self.r.touch('src/x.cpp', 'feat(bild): next')
        self.assertIn('1.2.4 -> 1.3.0', self.plan())

    def test_a_hand_made_version_change_is_a_bump_too(self):
        self.r.touch('src/x.cpp', 'break(bild): old news')
        self.r.commit('manual', {BILD: '2.0.0\n'})
        self.assertIn('nothing to bump', self.plan())

    def test_check_exit_code_and_other_ref(self):
        first = self.r.git('rev-parse', 'HEAD').strip()
        self.assertEqual(self.r.run('--check')[0], 0)
        self.r.touch('src/x.cpp', 'x')
        self.assertEqual(self.r.run('--check')[0], 1)
        self.assertEqual(self.r.run('--check', '--ref', first)[0], 0)
        with self.assertRaises(SystemExit):
            self.r.run('--write', '--ref', first)

    def test_moving_the_version_file_is_not_a_bump(self):
        self.r.touch('src/x.cpp', 'fix: something')
        self.r.git('mv', BILD, 'src/bild_version_moved.txt')
        self.r.git('commit', '-q', '-m', 'move the version file')
        base = bv.last_bump(self.r.dir, 'HEAD', 'src/bild_version_moved.txt')
        start = self.r.git('rev-list', '--max-parents=0', 'HEAD').strip()
        self.assertEqual(base, start)
        self.r.commit('move and change', {'src/bild_version_moved.txt': '1.2.4\n'})
        self.assertEqual(bv.last_bump(self.r.dir, 'HEAD', 'src/bild_version_moved.txt'), self.r.git('rev-parse', 'HEAD').strip())

    def test_a_missing_version_file_is_skipped(self):
        self.r.git('rm', '-q', GEAR)
        self.r.git('commit', '-q', '-m', 'drop')
        self.assertIn('skip   SacredGear', self.plan())

    def test_helpers(self):
        self.assertEqual(bv.bump('1.9.9', 'minor'), '1.10.0')
        self.assertEqual(bv.bump('3', 'patch'), '3.0.1')
        self.assertIsNone(bv.bump('v1', 'patch'))
        self.assertTrue(bv.is_test_path('x/Tests/a.cpp'))
        self.assertTrue(bv.is_test_path('net/udp_test.cpp'))
        self.assertTrue(bv.is_test_path('changelog/gear/v1.2.3.md'))
        self.assertTrue(bv.is_test_path('src/changelog/history.md'))
        self.assertFalse(bv.is_test_path('main.cpp'))


class HookTests(unittest.TestCase):
    def test_post_merge_hook_bumps_during_a_real_merge(self):
        tools = os.path.dirname(os.path.abspath(__file__))
        root = os.path.dirname(tools)
        r = Repo()
        self.addCleanup(r.tmp.cleanup)
        r.commit('start', {BILD: '1.2.3\n', GEAR: '0.9\n', 'src/main.cpp': '//\n', 'mod-index.json': '{}\n'})
        for f in ('bump_versions.py', 'make_mod_index.py'):
            r.write('tools/' + f, open(os.path.join(tools, f), encoding='utf-8').read())
        for f in ('_python.sh', 'post-merge'):
            r.write('.githooks/' + f, open(os.path.join(root, '.githooks', f), encoding='utf-8', newline='').read().replace('\r\n', '\n'))
            os.chmod(os.path.join(r.dir, '.githooks', f), 0o755)
        r.git('add', '-A'); r.git('commit', '-q', '-m', 'tools'); r.git('config', 'core.hooksPath', '.githooks')
        r.git('checkout', '-q', '-b', 'f'); r.touch('src/x.cpp', 'feat(bild): x'); r.git('checkout', '-q', 'main'); r.touch('notes.txt', 'other work')
        subprocess.run(['git', '-C', r.dir, 'merge', '--no-ff', '-m', 'merge f', 'f'], capture_output=True, text=True, env=ENV, check=True)
        self.assertEqual(r.read(BILD), '1.3.0\n')
        self.assertIn('bump version 1.2.3 -> 1.3.0', r.git('log', '-1', '--format=%s', '--', BILD))
        self.assertEqual(r.git('status', '--porcelain', '--untracked-files=no').strip(), '')
        self.assertFalse(os.path.exists(os.path.join(r.dir, '.git', 'MERGE_HEAD')))


class IndexTests(unittest.TestCase):
    def test_index_follows_the_version_files(self):
        r = Repo()
        self.addCleanup(r.tmp.cleanup)
        r.commit('start', {BILD: '1.2.3\n', GEAR: '0.9\n'})
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(mi.main(['--repo', r.dir]), 0)
            self.assertEqual(mi.main(['--repo', r.dir, '--check']), 0)
        index = json.loads(r.read('mod-index.json'))
        self.assertEqual([(m['id'], m['kind'], m['version']) for m in index['mods']], [('sacred-bild', 'plugin', '1.2.3'), ('sacred-gear', 'plugin', '0.9')])
        self.assertEqual(index['mods'][0]['install'], {'files': ['ddraw.dll']})
        r.write(BILD, '1.2.4\n')
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(mi.main(['--repo', r.dir, '--check']), 1)


if __name__ == '__main__':
    unittest.main()
