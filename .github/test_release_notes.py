"""Offline regressions: real disposable Git graphs, fixture-only GitHub access."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location('release_notes', Path(__file__).with_name('release_notes.py'))
notes = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(notes)


class API:
    def __init__(self):
        self.prs = {}
        self.bodies = {}

    def body(self, tag):
        return self.bodies.get(tag, '')

    def pulls(self, sha):
        return self.prs.get(sha, [])

    def release(self, tag, artifact, source, **changes):
        pr = dict(merged_at='2026-01-01', title=tag, merge_commit_sha=artifact,
                  base={'ref': 'master'}, head={'sha': source})
        pr.update(changes)
        self.prs.setdefault(artifact, []).append(pr)


class GraphTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='release-notes-test-')
        self.cwd = os.getcwd()
        os.chdir(self.tmp.name)
        self.api = API()
        self.git('init', '-q', '-b', 'master')
        self.git('config', 'user.email', 'offline@example.invalid')
        self.git('config', 'user.name', 'Offline Test')
        self.root = self.add('chore: root')

    def tearDown(self):
        os.chdir(self.cwd)
        self.tmp.cleanup()

    def git(self, *args):
        return subprocess.check_output(['git', *args], text=True, stderr=subprocess.PIPE).strip()

    def add(self, subject, content=None):
        path = Path('data')
        path.write_text((path.read_text() if path.exists() else '') + (content or subject) + '\n')
        self.git('add', 'data')
        self.git('commit', '-q', '-m', subject)
        return self.git('rev-parse', 'HEAD')

    def generate(self, version, sha, pin=None):
        return notes.generate(version, sha, pin, api=self.api)

    def squash_pair(self):
        self.git('checkout', '-q', '-b', 'dev')
        old_source = self.add('feat: already released')
        self.git('checkout', '-q', 'master')
        self.git('merge', '--squash', 'dev')
        self.git('commit', '-q', '-m', 'v2.11.5 (#791)')
        old = self.git('rev-parse', 'HEAD')
        self.git('tag', '-a', 'v2.11.5', '-m', 'annotated')
        self.api.release('v2.11.5', old, old_source)
        self.git('checkout', '-q', 'dev')
        self.git('merge', '-q', 'master', '-m', 'reconcile released master')
        self.add('feat(query): new query')
        self.add('fix!: reclaim remap')
        self.add('perf: faster')
        source = self.add('docs: guide')
        self.git('checkout', '-q', 'master')
        self.git('merge', '--squash', 'dev')
        self.git('commit', '-q', '-m', 'v3.0.1 (#984)')
        artifact = self.git('rev-parse', 'HEAD')
        self.api.release('v3.0.1', artifact, source)
        return old, old_source, artifact, source

    def test_linear_numeric_future_and_unrelated(self):
        self.git('tag', 'v2.9.0')
        self.add('fix: prior')
        self.git('tag', 'v2.10.0')
        target = self.add('feat: desired')
        self.add('fix: future')
        self.git('tag', 'v9.0.0')
        self.git('checkout', '-q', '--orphan', 'unrelated')
        unrelated = self.add('feat: unrelated')
        self.git('tag', 'v2.10.9', unrelated)
        body = self.generate('2.11.0', target)
        self.assertIn('v2.10.0...v2.11.0', body)
        self.assertIn('- desired', body)
        self.assertNotIn('- prior', body)
        self.assertNotIn('- future', body)
        self.assertNotIn('- unrelated', body)

    def test_future_version_on_ancestor_is_not_previous(self):
        self.git('tag', 'v9.0.0', self.root)
        self.add('fix: previous change')
        self.git('tag', 'v2.10.0')
        target = self.add('feat: desired only')
        body = self.generate('2.11.0', target)
        self.assertIn('v2.10.0...v2.11.0', body)
        self.assertNotIn('v9.0.0', body)
        self.assertNotIn('previous change', body)

    def test_squash_reconciliation_pin_metadata_roundtrip(self):
        old, old_source, artifact, source = self.squash_pair()
        body = self.generate('3.0.1', artifact, source)
        for title in ['✨ Features', '🐛 Bug fixes', '⚡ Performance', '📚 Documentation']:
            self.assertIn(title, body)
        for absent in ['already released', '(#791)', '(#984)', 'reconcile released master']:
            self.assertNotIn(absent, body)
        self.assertEqual(body.count('- new query'), 1)
        self.assertEqual(body.count(notes.MARKER), 1)
        self.assertEqual(body, self.generate('3.0.1', artifact, source))
        data = notes.metadata(body)
        self.assertEqual(data['source'], source)
        self.assertEqual(data['artifact'], artifact)
        self.git('tag', 'v3.0.1', artifact)
        self.api.bodies['v3.0.1'] = body
        del self.api.prs[artifact]
        self.git('checkout', '-q', 'dev')
        target = self.add('fix: next only')
        next_body = self.generate('3.0.2', target)
        self.assertIn('- next only', next_body)
        self.assertNotIn('- new query', next_body)
        self.assertFalse(notes.ancestor(artifact, target))
        self.assertTrue(notes.ancestor(source, target))

    def test_merge_release_and_moving_dev(self):
        self.git('tag', 'v1.0.0')
        self.git('checkout', '-q', '-b', 'dev')
        source = self.add('feat: merged change')
        self.git('checkout', '-q', 'master')
        self.git('merge', '--no-ff', '-q', 'dev', '-m', 'v1.1.0')
        artifact = self.git('rev-parse', 'HEAD')
        self.api.release('v1.1.0', artifact, source)
        self.git('checkout', '-q', 'dev')
        self.add('fix: not released')
        body = self.generate('1.1.0', artifact, source)
        self.assertIn('- merged change', body)
        self.assertNotIn('not released', body)
        self.assertNotIn('chore: root', body)

    def test_explicit_manual_nonwrapper_merge_source(self):
        self.git('tag', 'v1.0.0')
        self.git('checkout', '-q', '-b', 'dev')
        self.add('feat: manual merged source')
        self.git('checkout', '-q', 'master')
        self.git('merge', '--no-ff', '-q', 'dev', '-m', 'Merge dev changes')
        artifact = self.git('rev-parse', 'HEAD')
        with self.assertRaisesRegex(notes.NotesError, 'Missing release source'):
            self.generate('1.1.0', artifact)
        body = notes.generate('1.1.0', artifact, api=self.api, allow_direct_merge=True)
        self.assertIn('- manual merged source', body)
        self.assertEqual(notes.metadata(body)['source'], artifact)
        self.assertNotIn('chore: root', body)

    def test_manual_mode_cannot_relabel_release_wrapper(self):
        _, _, artifact, _ = self.squash_pair()
        self.api.prs[artifact] = []
        with self.assertRaisesRegex(notes.NotesError, 'Missing release source'):
            notes.generate('3.0.1', artifact, api=self.api, allow_direct_merge=True)

    def test_missing_and_ambiguous_provenance(self):
        _, _, artifact, source = self.squash_pair()
        pr = self.api.prs[artifact][0]
        self.api.prs[artifact] = []
        with self.assertRaisesRegex(notes.NotesError, 'Missing release source'):
            self.generate('3.0.1', artifact)
        self.api.prs[artifact] = [pr, pr]
        with self.assertRaisesRegex(notes.NotesError, 'Ambiguous'):
            self.generate('3.0.1', artifact)
        self.api.prs[artifact] = [dict(pr, title='v3.0.2')]
        with self.assertRaisesRegex(notes.NotesError, 'Missing release source'):
            self.generate('3.0.1', artifact, source)

    def test_missing_previous_provenance_never_falls_back_to_root(self):
        old, _, artifact, _ = self.squash_pair()
        self.api.prs[old] = []
        with self.assertRaisesRegex(notes.NotesError, 'Missing release source'):
            self.generate('3.0.1', artifact)

    def test_tree_missing_object_and_pin_mismatch(self):
        _, old_source, artifact, source = self.squash_pair()
        self.api.prs[artifact][0]['head']['sha'] = old_source
        with self.assertRaisesRegex(notes.NotesError, 'tree does not match'):
            self.generate('3.0.1', artifact)
        self.api.prs[artifact][0]['head']['sha'] = 'f' * 40
        with self.assertRaisesRegex(notes.NotesError, 'failed'):
            self.generate('3.0.1', artifact)
        self.api.prs[artifact][0]['head']['sha'] = source
        with self.assertRaisesRegex(notes.NotesError, 'Pinned PR source'):
            self.generate('3.0.1', artifact, old_source)

    def test_wrong_base_unmerged_or_merge_sha(self):
        _, _, artifact, _ = self.squash_pair()
        pr = self.api.prs[artifact][0]
        for changes in [dict(base={'ref': 'dev'}), dict(merged_at=None),
                        dict(merge_commit_sha=self.root)]:
            self.api.prs[artifact] = [dict(pr, **changes)]
            with self.assertRaisesRegex(notes.NotesError, 'Missing release source'):
                self.generate('3.0.1', artifact)

    def test_metadata_rejects_corruption(self):
        _, _, artifact, source = self.squash_pair()
        body = self.generate('3.0.1', artifact)
        valid = notes.metadata(body)
        for field, value in [('artifact', self.root), ('source', self.root),
                             ('tree', 'f' * 40), ('tag', 'v9.0.0')]:
            corrupt = dict(valid, **{field: value})
            self.api.bodies['v3.0.1'] = '<!-- ' + notes.MARKER + ' ' + json.dumps(corrupt) + ' -->'
            with self.assertRaises(notes.NotesError):
                notes.resolve(self.api, 'v3.0.1', artifact)
        for malformed in [body + body, '<!-- release-notes-source-v2 {} -->',
                          '<!-- release-notes-source-v1 {bad} -->']:
            with self.assertRaises(notes.NotesError):
                notes.metadata(malformed)
        self.api.bodies['v3.0.1'] = body
        self.assertEqual(notes.resolve(self.api, 'v3.0.1', artifact), source)

    def test_invalid_pr_response_fails_clearly(self):
        self.api.prs[self.root] = [{'base': None, 'head': {}}]
        with self.assertRaisesRegex(notes.NotesError, 'Invalid PR record'):
            self.generate('1.0.0', self.root)

    def test_wrapper_only_source_delta_fails(self):
        self.git('tag', 'v1.0.0')
        source = self.add('v1.1.0 (#2)')
        self.api.release('v1.1.0', source, source)
        with self.assertRaisesRegex(notes.NotesError, 'wrapper-only'):
            self.generate('1.1.0', source)

    def test_divergent_previous_source_fails(self):
        old, _, artifact, _ = self.squash_pair()
        # Same tree but fabricated independent source history is not ancestry.
        tree = self.git('rev-parse', old + '^{tree}')
        divergent = self.git('commit-tree', tree, '-m', 'feat: independent')
        self.api.prs[old][0]['head']['sha'] = divergent
        with self.assertRaisesRegex(notes.NotesError, 'not an ancestor'):
            self.generate('3.0.1', artifact)

    def test_first_root_and_rollup(self):
        target = self.add('feat: first')
        body = self.generate('1.0.0', target)
        self.assertIn('chore: root', body)
        self.assertIn('Maintenance &amp; internal', body)
        self.git('tag', 'pre-v2-rollup', self.root)
        body = self.generate('2.0.0', target)
        self.assertNotIn('chore: root', body)
        self.assertIn('pre-v2-rollup...v2.0.0', body)

    def test_invalid_version_target_tag_and_empty(self):
        for value in ['v1.0.0', '1.2', '1.0.0-rc1', '1.0.0\n', '-1.0.0']:
            with self.assertRaises(notes.NotesError):
                self.generate(value, self.root)
        with self.assertRaisesRegex(notes.NotesError, 'immutable'):
            self.generate('1.0.0', 'master')
        self.git('tag', 'v1.0.0')
        target = self.add('feat: next')
        with self.assertRaisesRegex(notes.NotesError, 'existing tag'):
            self.generate('1.0.0', target)
        with self.assertRaisesRegex(notes.NotesError, 'Empty'):
            self.generate('1.0.1', self.root)

    def test_git_log_failure_is_not_hidden(self):
        actual = notes.git

        def fail(*args):
            if args[0] == 'log':
                raise notes.NotesError('git log failed (128)')
            return actual(*args)

        with patch.object(notes, 'git', side_effect=fail):
            with self.assertRaisesRegex(notes.NotesError, 'git log failed'):
                self.generate('1.0.0', self.root)

    def test_cli_wrapper_only_uses_stub_gh(self):
        script = Path(__file__).with_name('release-notes.sh').resolve()
        bin_dir = Path('bin')
        bin_dir.mkdir()
        stub = bin_dir / 'gh'
        stub.write_text('#!/usr/bin/python3\nimport json,sys\n'
                        'assert "commits/" in sys.argv[2], sys.argv\n'
                        'print(json.dumps([[]]))\n')
        stub.chmod(0o755)
        env = dict(os.environ, PATH=str(bin_dir.resolve()) + ':' + os.environ['PATH'])
        proc = subprocess.run(['bash', str(script), '1.0.0', self.root],
                              env=env, text=True, capture_output=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn('chore: root', proc.stdout)
        proc = subprocess.run(['bash', str(script), 'bad', self.root],
                              env=env, text=True, capture_output=True)
        self.assertEqual(proc.returncode, 1)
        self.assertIn('Invalid release version', proc.stderr)
        self.git('checkout', '-q', '-b', 'dev')
        self.add('feat: manual CLI merge')
        self.git('checkout', '-q', 'master')
        self.git('merge', '--no-ff', '-q', 'dev', '-m', 'Merge dev changes')
        artifact = self.git('rev-parse', 'HEAD')
        proc = subprocess.run(['bash', str(script), '1.0.0', artifact, '--allow-direct-merge'],
                              env=env, text=True, capture_output=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn('- manual CLI merge', proc.stdout)
        self.assertEqual(notes.metadata(proc.stdout)['source'], artifact)
        # Exercise a real nonzero git-log subprocess through the Bash entrypoint.
        git_stub = bin_dir / 'git'
        git_stub.write_text('#!/bin/sh\nif [ "$1" = log ]; then echo forced-log-failure >&2; exit 128; fi\n'
                            'exec /usr/bin/git "$@"\n')
        git_stub.chmod(0o755)
        proc = subprocess.run(['bash', str(script), '1.0.0', self.root],
                              env=env, text=True, capture_output=True)
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(proc.stdout, '')
        self.assertIn('forced-log-failure', proc.stderr)


class AdapterTests(unittest.TestCase):
    def test_announcement_hides_internal_and_provenance(self):
        body = notes.render(['feat: visible', 'chore: internal'], 'offline/repo', '1.0.0', None)
        body += '\n<!-- ' + notes.MARKER + ' {} -->\n'
        args = ['sed', '-e', '/<details>/,/<\\/details>/d',
                '-e', '/^<!-- release-notes-source-v1 /d']
        workflow = Path(__file__).with_name('workflows').joinpath('release.yml').read_text()
        self.assertIn("sed -e '/<details>/,/<\\/details>/d' -e '/^<!-- release-notes-source-v1 /d'", workflow)
        proc = subprocess.run(args, input=body, text=True, capture_output=True, check=True)
        self.assertIn('✨ Features', proc.stdout)
        self.assertIn('visible', proc.stdout)
        self.assertNotIn('internal', proc.stdout)
        self.assertNotIn(notes.MARKER, proc.stdout)

    def test_metadata_marker_in_prose(self):
        data = dict(tag='v1.0.0', artifact='a' * 40, source='b' * 40, tree='c' * 40)
        prose = '- preserve release-notes-source-v1 in documentation\n'
        body = prose + '\n<!-- ' + notes.MARKER + ' ' + json.dumps(data) + ' -->\n'
        self.assertEqual(notes.metadata(body), data)
        self.assertIsNone(notes.metadata(prose))
        with self.assertRaises(notes.NotesError):
            notes.metadata('<!-- release-notes-source-v1 malformed\n')

    def test_api_failures_and_pages(self):
        api = notes.GitHub('offline/repo')
        def result(code=0, out='', err=''):
            return subprocess.CompletedProcess([], code, out, err)
        with patch.object(notes.subprocess, 'run', return_value=result(1, err='HTTP 403')):
            with self.assertRaisesRegex(notes.NotesError, 'GitHub lookup'):
                api.body('v1.0.0')
        with patch.object(notes.subprocess, 'run', return_value=result(1, err='gh: Not Found (HTTP 404)')):
            self.assertEqual(api.body('v1.0.0'), '')
        with patch.object(notes.subprocess, 'run', return_value=result(out='not json')):
            with self.assertRaisesRegex(notes.NotesError, 'Invalid GitHub JSON'):
                api.pulls('f' * 40)
        with patch.object(notes.subprocess, 'run', return_value=result(out='[[{"number":1}],[]]')):
            self.assertEqual(api.pulls('f' * 40), [{'number': 1}])
        with patch.object(notes.subprocess, 'run', return_value=result(out='{}')):
            with self.assertRaisesRegex(notes.NotesError, 'paginated'):
                api.pulls('f' * 40)


if __name__ == '__main__':
    unittest.main()
