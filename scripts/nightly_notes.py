"""Notes from exact source commits of successfully published nightly releases."""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path

REPOSITORIES = {'Nonary/vibeshine', 'Nonary/Vibepollo', 'Nonary/moonlight-qt'}


def run(*args):
    return subprocess.check_output(args, text=True, encoding='utf-8').strip()


def exact_sha(value):
    if not isinstance(value, str) or not re.fullmatch(r'[0-9a-f]{40}', value):
        raise ValueError('Expected an exact lowercase 40-character source SHA')
    return value


def sequence(repo, tag, version=None, source=None):
    if repo not in REPOSITORIES:
        raise ValueError('Repository is not in the nightly allowlist')
    if repo == 'Nonary/moonlight-qt':
        match = re.fullmatch(r'v6\.[12]\.0-daily\.([1-9][0-9]*)', tag)
        if not match or (version is not None and tag != 'v' + version):
            raise ValueError('Invalid Moonlight nightly identity')
        build = int(match[1])
        if build > 65535:
            raise ValueError('Moonlight nightly sequence exceeds the Windows build field')
        return build
    match = re.fullmatch(r'daily-(20[0-9]{2})-([0-9]{2})-([0-9]{2})', tag)
    if not match:
        raise ValueError('Invalid host nightly tag')
    import datetime
    datetime.date(*map(int, match.groups()))
    day = ''.join(match.groups())
    if version is not None and not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+-daily\.d' + day + r'\+' + exact_sha(source)[:12], version):
        raise ValueError('Invalid host nightly version/source identity')
    return int(day)


def previous_release(repo, releases, metadata_reader, tag_resolver):
    """Published release state is the baseline; build runs never advance it."""
    candidates = []
    for release in releases:
        if release.get('draft') is not False or release.get('prerelease') is not True or not release.get('published_at'):
            continue
        tag = release.get('tag_name', '')
        try:
            sequence(repo, tag)
        except ValueError:
            continue
        candidates.append(release)
    for release in sorted(candidates, key=lambda r: (r['published_at'], r.get('id', 0)), reverse=True):
        tag = release['tag_name']
        assets = [a for a in release.get('assets', []) if a.get('name') == 'nightly-metadata.json']
        if assets:
            if len(assets) != 1:
                raise ValueError('Duplicate nightly metadata assets')
            metadata = metadata_reader(assets[0])
            if metadata.get('channel') != 'nightly':
                continue
            if metadata.get('schema_version') != 1 or metadata.get('tag') != tag:
                raise ValueError('Published nightly metadata identity mismatch')
            sha = exact_sha(metadata.get('source_commit'))
            if not isinstance(metadata.get('version'), str) or metadata.get('nightly_sequence') != sequence(repo, tag, metadata['version'], sha):
                raise ValueError('Published nightly version/sequence mismatch')
            if repo == 'Nonary/moonlight-qt' and 'windows_product_version' in metadata and metadata['windows_product_version'] != f'6.2.{metadata["nightly_sequence"]}':
                raise ValueError('Published nightly Windows product version mismatch')
            if tag_resolver(tag) != sha:
                raise ValueError('Published nightly tag does not match metadata source')
        else:
            # Migration: older published daily tags are authoritative exact targets.
            sha = exact_sha(tag_resolver(tag))
        return {'tag': tag, 'source_commit': sha}
    return None


def make_notes(repo, tag, version, source, previous, commits, ancestor=True):
    source = exact_sha(source)
    seq = sequence(repo, tag, version, source)
    first = previous is None
    old = exact_sha(previous['source_commit']) if previous else None
    compare = f'https://github.com/{repo}/compare/{old}...{source}' if old else None
    unchanged = old == source
    metadata = dict(schema_version=1, channel='nightly', tag=tag, version=version,
                    source_commit=source, nightly_sequence=seq,
                    previous_successful_tag=previous['tag'] if previous else None,
                    previous_source_commit=old, changelog_range=f'{old}..{source}' if old else None,
                    compare_url=compare, first_nightly=first, no_source_changes=unchanged,
                    commits_total=len(commits), commits_listed=len(commits))
    if repo == 'Nonary/moonlight-qt':
        metadata['windows_product_version'] = f'6.2.{seq}'
        return moonlight_notes(repo, tag, version, previous, commits, compare, ancestor), metadata
    notes = [f'## Nightly changes ({version})', '', f'Source: [{source}](https://github.com/{repo}/commit/{source})', '']
    if first:
        notes += ['First published nightly: no previous successful nightly baseline exists. No commit delta is claimed.']
    elif unchanged:
        notes += [f'No source changes since `{previous["tag"]}`; this nightly rebuilds the same source.']
    else:
        notes += [f'Since the preceding successfully published nightly `{previous["tag"]}`: [compare exact sources]({compare}).']
        if not ancestor:
            notes += ['', '**Warning: source history diverged or was rewound.** The list is the exact newly reachable commit set `previous_source_commit..source_commit`, not a forward ancestry claim.']
        if not commits:
            notes += ['', 'No newly reachable commits in this exact range. Source identity changed (for example, a rewind).']
        for sha, message in commits:
            exact_sha(sha)
            # Fence preserves the complete raw conventional commit message.
            fence = '`' * max(3, max((len(m[0]) for m in re.finditer(r'`+', message)), default=0) + 1)
            notes += ['', f'[{sha[:12]}](https://github.com/{repo}/commit/{sha})', fence + 'text', message.rstrip('\n'), fence]
    return '\n'.join(notes) + '\n', metadata


def moonlight_notes(repo, tag, version, previous, commits, compare, ancestor):
    import html
    notes = ['This is a nightly release of Moonlight and contains several bug fixes and may contain new features. Check the commit log for more information.',
             '', 'Moonlight builds are unsigned and may show an unverified publisher. macOS builds are not Developer ID signed or notarized. These nightly builds may trigger security warnings or false-positive antivirus alerts.']
    if version.startswith('6.2.0-daily.'):
        notes += ['', 'Windows, Mac and AppImage users of 6.1.0-daily.182 or earlier must download this update manually once; automatic nightly update notifications work after upgrading.']
    notes += ['', f'[SHA256 checksums](https://github.com/{repo}/releases/download/{tag}/SHA256SUMS)',
              '', '### Linux installation', '',
              'Download the Linux package for your architecture below (Flatpak is also built for aarch64). For Flatpak, install Flatpak through your distribution\'s software manager first, then run:',
              '', '```sh',
              'flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo',
              f'flatpak install --user --reinstall ./Moonlight-{version}-x86_64.flatpak',
              '# On arm64 devices, use the aarch64 bundle instead:',
              f'# flatpak install --user --reinstall ./Moonlight-{version}-aarch64.flatpak',
              'flatpak run --user com.moonlight_stream.Moonlight//master', '```', '',
              'This nightly uses the `master` branch; the regular Flathub package uses `stable`. To update a Flatpak nightly, repeat the install command with the newer downloaded bundle. On Steam Deck, install in Desktop Mode and add this nightly branch to Steam.',
              '', 'For AppImage:', '', '```sh',
              f'chmod +x ./Moonlight-{version}-x86_64.AppImage',
              f'./Moonlight-{version}-x86_64.AppImage', '```', '',
              'If your distribution lacks FUSE support, launch the AppImage with `--appimage-extract-and-run`.',
              '', '<details>', '<summary>Commit log</summary>', '']
    if previous:
        old_tag = previous['tag']
        notes += [f'Changes since [{old_tag}](https://github.com/{repo}/releases/tag/{old_tag}) | [Compare]({compare})', '']
        if not ancestor:
            notes += ['Source history changed; this log lists newly reachable commits.', '']
    else:
        notes += ['First nightly release; no preceding nightly is available for comparison.', '']
    for sha, message in commits:
        exact_sha(sha)
        subject = html.escape(message.splitlines()[0] if message.splitlines() else '(no commit subject)')
        subject = subject.replace('\\', '\\\\').replace('[', '\\[').replace(']', '\\]')
        notes += [f'- [{sha[:12]}](https://github.com/{repo}/commit/{sha}) {subject}']
    if previous and not commits:
        notes += ['No new commits in this comparison.']
    notes += ['', '</details>']
    return '\n'.join(notes) + '\n'


def load_previous(repo):
    if repo not in REPOSITORIES:
        raise ValueError('Repository is not in the nightly allowlist')
    pages = json.loads(run('gh', 'api', '--paginate', '--slurp', f'repos/{repo}/releases?per_page=100'))
    def resolve(tag):
        return exact_sha(run('git', 'rev-parse', 'refs/tags/' + tag + '^{commit}'))
    def read_metadata(asset):
        asset_id = asset.get('id')
        if not isinstance(asset_id, int) or asset_id <= 0:
            raise ValueError('Invalid metadata asset ID')
        return json.loads(run('gh', 'api', '-H', 'Accept: application/octet-stream', f'repos/{repo}/releases/assets/{asset_id}'))
    return previous_release(repo, [r for page in pages for r in page], read_metadata, resolve)


def has_source_changes(source, previous):
    source = exact_sha(source)
    return previous is None or exact_sha(previous['source_commit']) != source


def generate(repo, tag, version, source, assets, notes):
    exact_sha(source)
    sequence(repo, tag, version, source)
    if run('git', 'rev-parse', source + '^{commit}') != source:
        raise ValueError('Pinned source commit cannot be resolved exactly')
    previous = load_previous(repo)
    if repo == 'Nonary/moonlight-qt' and not has_source_changes(source, previous):
        raise ValueError('Source unchanged from the last published nightly; refusing duplicate publication')
    commits, ancestor = [], True
    if previous:
        old = previous['source_commit']
        result = subprocess.run(['git', 'merge-base', '--is-ancestor', old, source], check=False)
        if result.returncode not in (0, 1):
            raise ValueError('Could not verify source ancestry')
        ancestor = result.returncode == 0
        # NUL framing preserves multiline bodies and avoids interpolated shell commands.
        raw = run('git', 'log', '--reverse', '--format=%H%x00%B%x00', old + '..' + source, '--')
        fields = raw.split('\0')
        for index in range(0, len(fields) - 1, 2):
            commits.append((exact_sha(fields[index].strip()), fields[index + 1]))
    body, metadata = make_notes(repo, tag, version, source, previous, commits, ancestor)
    root = Path(assets)
    if repo == 'Nonary/moonlight-qt':
        metadata['artifacts'] = artifact_metadata(root, source)
    # API-visible identity is generated only after the existing qualification gates.
    # The asset stays available for installed clients using the old contract.
    if repo != 'Nonary/moonlight-qt':
        evidence = root / 'daily-manifest.json'
        if evidence.is_file():
            metadata['qualification'] = json.loads(evidence.read_text(encoding='utf-8'))
    if '<!-- nightly-identity: ' in body:
        raise ValueError('Commit notes contain a reserved nightly identity marker')
    body = '<!-- nightly-identity: ' + json.dumps(metadata, separators=(',', ':')) + ' -->\n' + body
    (root / 'nightly-metadata.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
    with Path(notes).open('a', encoding='utf-8') as output:
        output.write('\n' + body)
    # Include metadata and existing manifest/provenance; never hash SHA256SUMS itself.
    files = sorted(p for p in root.iterdir() if p.name != 'SHA256SUMS')
    if any(not p.is_file() or p.is_symlink() or '\n' in p.name or '\r' in p.name for p in files):
        raise ValueError('Unsafe checksum input')
    (root / 'SHA256SUMS').write_text(''.join(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}\n' for p in files), encoding='utf-8')


def artifact_metadata(root, source):
    source = exact_sha(source)
    packages = sorted(p for p in Path(root).iterdir() if p.name not in ('nightly-metadata.json', 'SHA256SUMS'))
    if any(not p.is_file() or p.is_symlink() for p in packages):
        raise ValueError('Unsafe package metadata input')
    return [dict(name=p.name, source_commit=source, sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for p in packages]


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    for name in ('repo', 'tag', 'version', 'source', 'assets', 'notes'):
        parser.add_argument('--' + name, required=True)
    generate(**vars(parser.parse_args()))
