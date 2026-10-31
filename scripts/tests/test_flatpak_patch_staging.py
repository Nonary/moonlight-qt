import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[2]
MANIFEST = Path('packaging/flatpak/com.moonlight_stream.Moonlight.json')
PATCH = Path('app/deploy/linux/appimage/libplacebo-disable-internally-synchronized-queues.patch')


class FlatpakPatchStagingTests(unittest.TestCase):
    def test_both_release_lanes_stage_patch_inside_manifest_directory(self):
        for workflow in ('build-flatpak.yml', 'build-release.yml'):
            with self.subTest(workflow=workflow), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                for path in (MANIFEST, PATCH):
                    (root / path).parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(ROOT / path, root / path)
                script = (ROOT / '.github/workflows' / workflow).read_text()
                step = script.split('- name: Pin manifest to release source and version', 1)[1]
                match = re.search(r"python3 - <<'PY'\n(.*?)\n          PY", step, re.S)
                self.assertIsNotNone(match)
                source = 'a' * 40
                env = dict(os.environ, SOURCE_SHA=source, GITHUB_SHA=source,
                           CI_VERSION='6.2.0-daily.195')
                subprocess.run([sys.executable, '-c', textwrap.dedent(match[1])],
                               cwd=root, env=env, check=True)
                manifest = json.loads((root / MANIFEST).read_text())
                modules = {module['name']: module for module in manifest['modules']}
                patches = [s for s in modules['libplacebo']['sources'] if s['type'] == 'patch']
                self.assertEqual(len(patches), 1)
                staged = (root / MANIFEST.parent / patches[0]['path']).resolve()
                self.assertEqual(staged.parent, (root / MANIFEST.parent).resolve())
                self.assertEqual(staged.read_bytes(), (ROOT / PATCH).read_bytes())
                self.assertEqual((root / PATCH).read_bytes(), (ROOT / PATCH).read_bytes())
                self.assertEqual(modules['moonlight']['sources'][0]['commit'], source)
                self.assertEqual(modules['moonlight']['sources'][1]['commands'],
                                 ['echo 6.2.0-daily.195 > app/version.txt'])


if __name__ == '__main__':
    unittest.main()
