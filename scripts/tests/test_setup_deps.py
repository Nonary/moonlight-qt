"""Check that setup-deps.py reuses only complete deployments of the selected release.

Downloads are served from local fixture archives; no network access is needed.
"""
from pathlib import Path
from unittest import mock
import importlib.util
import json
import shutil
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
ASSET = 'fixture-universal.zip'

spec = importlib.util.spec_from_file_location('setup_deps', ROOT / 'setup-deps.py')
setup_deps = importlib.util.module_from_spec(spec)
spec.loader.exec_module(setup_deps)

RELEASES = {
    'v8': {'lib/libSDL2.dylib': b'sdl2-v8', 'lib/libavcodec.dylib': b'avcodec-v8',
           'include/SDL.h': b'/* v8 */', 'lib/only-in-v8.txt': b'old'},
    'v9': {'lib/libSDL2.dylib': b'sdl2-v9-newer', 'lib/libavcodec.dylib': b'avcodec-v9',
           'include/SDL.h': b'/* v9 */'},
    'no-include': {'lib/libSDL2.dylib': b'sdl2'},
}


class SetupDepsTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='moonlight-setup-deps-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.archives = self.root / 'archives'
        self.archives.mkdir()
        for tag, files in RELEASES.items():
            with zipfile.ZipFile(self.archives / f'{tag}.zip', 'w') as archive:
                for name, content in files.items():
                    archive.writestr(name, content)
        self.target = self.root / 'libs/mac'
        self.downloads = []

        def retrieve(url, destination):
            tag = url.split('/')[-2]
            self.downloads.append(tag)
            shutil.copyfile(self.archives / f'{tag}.zip', destination)

        for patcher in (mock.patch.object(setup_deps, 'get_platform_config',
                                          return_value=('mac', ASSET)),
                        mock.patch.object(setup_deps.urllib.request, 'urlretrieve', retrieve)):
            patcher.start()
            self.addCleanup(patcher.stop)

    def deploy(self, tag='v8', force=False):
        setup_deps.download_and_extract(force=force, root=str(self.root), tag=tag)

    def assert_deployed(self, tag):
        for name, content in RELEASES[tag].items():
            self.assertEqual((self.target / name).read_bytes(), content, name)
        marker = json.loads((self.target / setup_deps.COMPLETION_MARKER).read_text())
        self.assertEqual((marker['tag'], marker['asset']), (tag, ASSET))
        self.assertEqual(sorted(marker['files']), sorted(RELEASES[tag]))
        self.assertEqual(list((self.root / 'libs').glob('.mac.staging-*')), [])

    def test_complete_same_version_is_reused(self):
        self.deploy()
        self.deploy()
        self.assertEqual(self.downloads, ['v8'])
        self.assert_deployed('v8')

    def test_force_redownloads_complete_deployment(self):
        self.deploy()
        (self.target / 'lib/local-edit.txt').write_text('stale')
        self.deploy(force=True)
        self.assertEqual(self.downloads, ['v8', 'v8'])
        self.assert_deployed('v8')
        self.assertFalse((self.target / 'lib/local-edit.txt').exists())

    def test_force_flag_is_parsed(self):
        with mock.patch.object(setup_deps, 'download_and_extract') as deploy:
            setup_deps.main(['--force'])
            deploy.assert_called_once_with(force=True)
            deploy.reset_mock()
            setup_deps.main([])
            deploy.assert_called_once_with(force=False)

    def test_changed_tag_replaces_deployment(self):
        self.deploy('v8')
        self.deploy('v9')
        self.assertEqual(self.downloads, ['v8', 'v9'])
        self.assert_deployed('v9')
        self.assertFalse((self.target / 'lib/only-in-v8.txt').exists())

    def test_missing_required_file_is_redeployed(self):
        self.deploy()
        (self.target / 'lib/libavcodec.dylib').unlink()
        self.deploy()
        self.assertEqual(self.downloads, ['v8', 'v8'])
        self.assert_deployed('v8')

    def test_truncated_file_is_redeployed(self):
        self.deploy()
        (self.target / 'lib/libSDL2.dylib').write_bytes(b'')
        self.deploy()
        self.assertEqual(self.downloads, ['v8', 'v8'])
        self.assert_deployed('v8')

    def test_unmarked_legacy_deployment_is_redeployed(self):
        # A tree with lib/ and include/ but no completion record, as left by an
        # interrupted extraction from an earlier version of the script.
        (self.target / 'lib').mkdir(parents=True)
        (self.target / 'include').mkdir()
        self.deploy()
        self.assertEqual(self.downloads, ['v8'])
        self.assert_deployed('v8')

    def test_corrupt_marker_is_redeployed(self):
        self.deploy()
        (self.target / setup_deps.COMPLETION_MARKER).write_text('{not json')
        self.deploy()
        self.assertEqual(self.downloads, ['v8', 'v8'])
        self.assert_deployed('v8')

    def test_interrupted_extraction_is_retried(self):
        real_extract = setup_deps.extract_archive

        def interrupted(archive_path, destination):
            with zipfile.ZipFile(archive_path) as archive:
                archive.extract(archive.infolist()[0], destination)
            (Path(destination) / 'include').mkdir(exist_ok=True)
            raise KeyboardInterrupt

        with mock.patch.object(setup_deps, 'extract_archive', interrupted):
            with self.assertRaises(KeyboardInterrupt):
                self.deploy()
        self.assertFalse(self.target.exists())

        self.assertIs(setup_deps.extract_archive, real_extract)
        self.deploy()
        self.assertEqual(self.downloads, ['v8', 'v8'])
        self.assert_deployed('v8')

    def test_interrupted_upgrade_keeps_previous_deployment(self):
        self.deploy('v8')
        with mock.patch.object(setup_deps, 'extract_archive', side_effect=KeyboardInterrupt):
            with self.assertRaises(KeyboardInterrupt):
                self.deploy('v9')
        self.assert_deployed('v8')
        self.deploy('v9')
        self.assert_deployed('v9')

    def test_stale_staging_is_removed(self):
        stale = self.root / 'libs/.mac.staging-leftover'
        (stale / 'lib').mkdir(parents=True)
        self.deploy()
        self.assertFalse(stale.exists())
        self.assert_deployed('v8')

    def test_archive_without_required_contents_is_not_published(self):
        with self.assertRaises(SystemExit) as raised:
            self.deploy('no-include')
        self.assertNotEqual(raised.exception.code, 0)
        self.assertFalse(self.target.exists())
        self.assertEqual(list((self.root / 'libs').glob('.mac.staging-*')), [])

    def test_failed_download_keeps_previous_deployment(self):
        self.deploy('v8')
        with mock.patch.object(setup_deps.urllib.request, 'urlretrieve',
                               side_effect=OSError('offline')):
            with self.assertRaises(SystemExit) as raised:
                self.deploy('v9')
        self.assertNotEqual(raised.exception.code, 0)
        self.assert_deployed('v8')


if __name__ == '__main__':
    unittest.main()
