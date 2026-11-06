#!/usr/bin/env python3
"""Release subjects from verified source history, independently of artifact history.

Requires local full history/tags and gh read access. Never fetches a moving branch.
Missing source objects must be fetched explicitly by the caller before retrying.
"""
import argparse
import json
import os
import re
import subprocess
import sys


class NotesError(Exception):
    pass


def run(args):
    proc = subprocess.run(args, text=True, capture_output=True)
    if proc.returncode:
        raise NotesError(f"{' '.join(args)} failed ({proc.returncode}): {proc.stderr.strip()}")
    return proc.stdout.strip()


def git(*args):
    return run(['git', *args])


def version(value):
    if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', value):
        raise NotesError(f'Invalid release version: {value!r}; expected X.Y.Z')
    return tuple(map(int, value.split('.')))


def commit(ref):
    return git('rev-parse', '--verify', ref + '^{commit}')


def immutable(sha):
    if not isinstance(sha, str) or not re.fullmatch(r'[0-9a-f]{40}', sha):
        raise NotesError(f'Expected immutable full commit SHA, got {sha!r}')
    if commit(sha) != sha:
        raise NotesError(f'Expected commit SHA, not tag object: {sha}')
    return sha


def ancestor(old, new):
    proc = subprocess.run(['git', 'merge-base', '--is-ancestor', old, new],
                          text=True, capture_output=True)
    if proc.returncode not in (0, 1):
        raise NotesError(f'git ancestry check failed: {proc.stderr.strip()}')
    return proc.returncode == 0


def same_tree(artifact, source):
    if git('rev-parse', artifact + '^{tree}') != git('rev-parse', source + '^{tree}'):
        raise NotesError(f'Source tree does not match artifact: {source} vs {artifact}')


class GitHub:
    def __init__(self, repo):
        self.repo = repo

    def api(self, path, optional=False, pages=False):
        args = ['gh', 'api', f'repos/{self.repo}/{path}']
        if pages:
            args += ['--paginate', '--slurp']
        proc = subprocess.run(args, text=True, capture_output=True)
        if optional and proc.returncode and '(HTTP 404)' in proc.stderr:
            return None
        if proc.returncode:
            raise NotesError(f'GitHub lookup {path} failed: {proc.stderr.strip()}')
        try:
            return json.loads(proc.stdout)
        except ValueError as exc:
            raise NotesError(f'Invalid GitHub JSON for {path}') from exc

    def body(self, tag):
        data = self.api(f'releases/tags/{tag}', optional=True)
        if data is None:
            return ''
        if not isinstance(data, dict) or not isinstance(data.get('body'), (str, type(None))):
            raise NotesError(f'Invalid release response for {tag}')
        return data.get('body') or ''

    def pulls(self, artifact):
        pages = self.api(f'commits/{artifact}/pulls?per_page=100', pages=True)
        if not isinstance(pages, list) or any(not isinstance(p, list) for p in pages):
            raise NotesError('Invalid paginated PR response')
        return [pr for page in pages for pr in page]


MARKER = 'release-notes-source-v1'


def metadata(body):
    # Generated provenance occupies its own HTML-comment line. A commit title
    # or documentation mentioning the marker is ordinary release-note prose.
    records = [line.strip() for line in body.splitlines()
               if line.lstrip().startswith('<!-- release-notes-source-')]
    if not records:
        return None
    match = re.fullmatch(r'<!-- release-notes-source-v1 (.*?) -->', records[0])
    if len(records) != 1 or not match:
        raise NotesError('Malformed, duplicate or unsupported release source metadata')
    try:
        data = json.loads(match[1])
    except ValueError as exc:
        raise NotesError('Malformed release source metadata JSON') from exc
    if not isinstance(data, dict):
        raise NotesError('Invalid release source metadata')
    return data

def resolve(api, tag, artifact, pinned=None, body=None, allow_direct_merge=False):
    """Metadata is authoritative if present; otherwise bootstrap exact PR evidence."""
    data = metadata(api.body(tag) if body is None else body)
    if data is not None:
        if data.get('tag') != tag or data.get('artifact') != artifact:
            raise NotesError(f'Metadata tag/artifact mismatch for {tag}')
        source = immutable(data.get('source'))
        if data.get('tree') != git('rev-parse', artifact + '^{tree}'):
            raise NotesError(f'Metadata tree mismatch for {tag}')
    else:
        pulls = api.pulls(artifact)
        if any(not isinstance(pr, dict) or not isinstance(pr.get('base'), dict)
               or not isinstance(pr.get('head'), dict) for pr in pulls):
            raise NotesError('Invalid PR record')
        matches = [pr for pr in pulls if pr.get('merged_at')
                   and pr.get('merge_commit_sha') == artifact
                   and pr.get('base', {}).get('ref') == 'master'
                   and pr.get('title') == tag]
        if len(matches) > 1:
            raise NotesError(f'Ambiguous merged release PRs for {tag}')
        if matches:
            source = immutable(matches[0].get('head', {}).get('sha'))
        else:
            # A normal, non-merge source commit is self-provenancing. A release
            # wrapper needs exact provenance. An explicit manual selection can
            # identify a non-wrapper merge as its own complete source history.
            subject = git('show', '-s', '--format=%s', artifact)
            parents = git('show', '-s', '--format=%P', artifact).split()
            if re.match(r'^v?[0-9]+\.[0-9]+\.[0-9]+(?:\s|$)', subject) or (len(parents) > 1 and not allow_direct_merge):
                raise NotesError(f'Missing release source provenance for {tag} ({artifact})')
            source = artifact
    same_tree(artifact, source)
    if pinned and immutable(pinned) != source:
        raise NotesError(f'Pinned PR source disagrees with provenance for {tag}')
    return source


def previous(api, requested, artifact, source):
    tags = git('tag', '--list').splitlines()
    candidates = [t for t in tags if re.fullmatch(r'v[0-9]+\.[0-9]+\.[0-9]+', t)
                  and version(t[1:]) < version(requested)]
    for tag in sorted(candidates, key=lambda t: version(t[1:]), reverse=True):
        old = commit('refs/tags/' + tag)
        old_source = resolve(api, tag, old)
        related = ancestor(old, artifact) or ancestor(old, source)
        if ancestor(old_source, source):
            return tag, old, old_source
        if related:
            raise NotesError(f'Previous source {tag} is not an ancestor of current source')
    # Root/rollup is only a first-release boundary, not a provenance fallback.
    if candidates:
        raise NotesError('No compatible previous release source among older version tags')
    if 'pre-v2-rollup' in tags:
        old = commit('refs/tags/pre-v2-rollup')
        if not ancestor(old, source):
            raise NotesError('pre-v2-rollup is not a source ancestor')
        return 'pre-v2-rollup', old, old
    return None, None, None


def render(subjects, repo, requested, prev):
    buckets = {key: [] for key in ('feat', 'fix', 'perf', 'docs', 'other')}
    for subject in subjects:
        match = re.match(r'^([a-zA-Z]+)(?:\([^)]*\))?!?:\s*(.*)$', subject)
        key = match[1] if match and match[1] in buckets and match[1] != 'other' else 'other'
        buckets[key].append('- ' + (match[2] if key != 'other' else subject))
    parts = []
    for key, title in [('feat', '✨ Features'), ('fix', '🐛 Bug fixes'),
                       ('perf', '⚡ Performance'), ('docs', '📚 Documentation')]:
        if buckets[key]:
            parts.append(f'### {title}\n\n' + '\n'.join(buckets[key]) + '\n')
    if buckets['other']:
        parts.append('<details>\n<summary>🔧 Maintenance &amp; internal</summary>\n\n'
                     + '\n'.join(buckets['other']) + '\n\n</details>')
    if prev:
        parts.append(f'**Full changelog**: https://github.com/{repo}/compare/{prev}...v{requested}')
    return '\n\n'.join(parts)


def generate(requested, target, pinned=None, repo='RayforceDB/rayforce', api=None,
             allow_direct_merge=False):
    version(requested)
    artifact = immutable(target)
    api = api or GitHub(repo)
    tag = 'v' + requested
    tags = git('tag', '--list').splitlines()
    if tag in tags and commit('refs/tags/' + tag) != artifact:
        raise NotesError(f'Artifact target disagrees with existing tag {tag}')
    source = resolve(api, tag, artifact, pinned, body='',
                     allow_direct_merge=allow_direct_merge)
    prev, old, old_source = previous(api, requested, artifact, source)
    args = ['log', '--no-merges', '--reverse', '--format=%s', source]
    if old:
        args += ['--not', old_source, old]
    subjects = git(*args).splitlines()
    if not subjects or all(re.match(r'^v?[0-9]+\.[0-9]+\.[0-9]+(?:\s|$)', s) for s in subjects):
        raise NotesError('Empty or wrapper-only release source delta; explicit release decision required')
    data = dict(tag=tag, artifact=artifact, source=source,
                tree=git('rev-parse', artifact + '^{tree}'))
    return (render(subjects, repo, requested, prev) + '\n\n<!-- ' + MARKER + ' '
            + json.dumps(data, sort_keys=True, separators=(',', ':')) + ' -->\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('version')
    parser.add_argument('artifact_sha')
    parser.add_argument('source_sha', nargs='?', default=os.getenv('RELEASE_SOURCE_SHA') or None)
    parser.add_argument('--allow-direct-merge', action='store_true',
                        help='manual dispatch: the selected non-wrapper merge is the source')
    args = parser.parse_args()
    try:
        print(generate(args.version, args.artifact_sha, args.source_sha,
                       os.getenv('GITHUB_REPOSITORY', 'RayforceDB/rayforce'),
                       allow_direct_merge=args.allow_direct_merge), end='')
    except (NotesError, OSError) as exc:
        print(f'release notes: {exc}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
