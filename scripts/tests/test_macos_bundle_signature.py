"""Check final DMG input signatures with real codesign and a tiny build fixture.

No app is launched, no DMG is mounted, and no signing credentials are required.
Only the build/deployment tools are stubbed; signature validation is real.
"""
from pathlib import Path
import os
import plistlib
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(sys.platform == 'darwin', 'Requires macOS codesign')
class MacosBundleSignatureTests(unittest.TestCase):
    def run_pipeline(self, version, corrupt_after_sign=False, missing_app_signature=False,
                     missing_nested_signature=False):
        temporary = tempfile.TemporaryDirectory(prefix='moonlight-dmg-signature-')
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        (root / 'app').mkdir()
        (root / 'app/version.txt').write_text('6.2.0\n')
        (root / 'setup-deps.py').write_text('')
        tools = root / 'tools'
        tools.mkdir()
        self.write_tool(tools, 'git', '#!/bin/bash\necho 0123456789abcdef0123456789abcdef01234567\n')
        self.write_tool(tools, 'qmake', '''#!/usr/bin/env python3
import os, pathlib, plistlib, shutil, subprocess
bundle = pathlib.Path('app/Moonlight.app')
(bundle / 'Contents/MacOS').mkdir(parents=True)
(bundle / 'Contents/Helpers').mkdir()
with (bundle / 'Contents/Info.plist').open('wb') as output:
    plistlib.dump({'CFBundleExecutable': 'Moonlight',
                  'CFBundleIdentifier': 'org.moonlight.test.packaging',
                  'CFBundlePackageType': 'APPL',
                  'CFBundleVersion': '6.2.0',
                  'CFBundleShortVersionString': '6.2.0'}, output)
for destination in ('Contents/MacOS/Moonlight', 'Contents/Helpers/Helper'):
    shutil.copyfile('/usr/bin/true', bundle / destination)
    (bundle / destination).chmod(0o755)
subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-',
                str(bundle / 'Contents/Helpers/Helper')], check=True)
''')
        self.write_tool(tools, 'make', '#!/bin/bash\nexit 0\n')
        self.write_tool(tools, 'dsymutil', '#!/bin/bash\nmkdir -p "$3"\n')
        self.write_tool(tools, 'macdeployqt', '''#!/usr/bin/env python3
import os, pathlib, subprocess, sys
bundle = pathlib.Path(sys.argv[1])
subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', str(bundle)], check=True)
subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict', str(bundle)], check=True)
if os.environ.get('TEST_MISSING_APP_SIGNATURE') == '1':
    subprocess.run(['/usr/bin/codesign', '--remove-signature', str(bundle)], check=True)
if os.environ.get('TEST_MISSING_NESTED_SIGNATURE') == '1':
    subprocess.run(['/usr/bin/codesign', '--remove-signature',
                    str(bundle / 'Contents/Helpers/Helper')], check=True)
''')
        self.write_tool(tools, 'codesign', '''#!/usr/bin/env python3
import os, pathlib, subprocess, sys
subprocess.run(['/usr/bin/codesign', *sys.argv[1:]], check=True)
if '--sign' in sys.argv and os.environ.get('TEST_CORRUPT_AFTER_SIGN') == '1':
    with (pathlib.Path(sys.argv[-1]) / 'Contents/Helpers/Helper').open('ab') as output:
        output.write(b'invalidating nested code signature after signing')
''')
        self.write_tool(tools, 'create-dmg', '''#!/bin/bash
# Record whether invalid nested code was caught before packaging started.
touch "$TEST_ROOT/packaging-started"
printf '%s\\n' "$@" > "$TEST_ROOT/dmg-arguments"
/usr/bin/codesign --verify --deep --strict "$1" || exit 1
touch "$2/Moonlight.dmg"
''')
        environment = os.environ.copy()
        environment.update(PATH=f'{tools}:{environment["PATH"]}', CI_VERSION=version,
                           SIGNING_IDENTITY='', SIGNING_PROVIDER_SHORTNAME='',
                           NOTARY_KEYCHAIN_PROFILE='', TEST_ROOT=str(root),
                           TEST_MISSING_APP_SIGNATURE=str(int(missing_app_signature)),
                           TEST_MISSING_NESTED_SIGNATURE=str(int(missing_nested_signature)),
                           TEST_CORRUPT_AFTER_SIGN=str(int(corrupt_after_sign)))
        result = subprocess.run(['/bin/bash', str(ROOT / 'scripts/generate-dmg.sh'), 'Release'],
                                cwd=root, env=environment, text=True, capture_output=True)
        return root, result

    @staticmethod
    def write_tool(directory, name, source):
        path = directory / name
        path.write_text(source)
        path.chmod(0o755)

    def test_nightly_metadata_is_covered_by_final_signature(self):
        root, result = self.run_pipeline('6.2.0-daily.183')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        bundle = root / 'build/build-Release/app/Moonlight.app'
        with (bundle / 'Contents/Info.plist').open('rb') as source:
            metadata = plistlib.load(source)
        self.assertEqual(metadata['CFBundleVersion'], '6.2.183')
        self.assertEqual(metadata['MoonlightNightlyVersion'], '6.2.0-daily.183')
        self.assertEqual(metadata['MoonlightSourceCommit'], '0123456789abcdef0123456789abcdef01234567')
        subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict', str(bundle)], check=True)
        signature = subprocess.run(['/usr/bin/codesign', '--display', '--verbose=2', str(bundle)],
                                   text=True, capture_output=True, check=True).stderr
        self.assertIn('Signature=adhoc', signature)
        self.assertIn('TeamIdentifier=not set', signature)
        self.assertNotIn('Authority=', signature)
        dmg_arguments = (root / 'dmg-arguments').read_text().splitlines()
        self.assertIn('--no-code-sign', dmg_arguments)
        self.assertFalse(any(argument.startswith('--identity') for argument in dmg_arguments))
        self.assertTrue((root / 'build/installer-Release/Moonlight-6.2.0-daily.183.dmg').is_file())

    def test_unsigned_non_nightly_still_packages(self):
        root, result = self.run_pipeline('6.2.0')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue((root / 'build/installer-Release/Moonlight-6.2.0.dmg').is_file())

    def test_missing_app_signature_is_created(self):
        root, result = self.run_pipeline('6.2.0-daily.183', missing_app_signature=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        bundle = root / 'build/build-Release/app/Moonlight.app'
        subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict', str(bundle)], check=True)

    def test_unsigned_non_qt_dependency_is_signed(self):
        root, result = self.run_pipeline('6.2.0-daily.183', missing_nested_signature=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        bundle = root / 'build/build-Release/app/Moonlight.app'
        subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict', str(bundle)], check=True)

    def test_invalid_nested_code_is_rejected_before_packaging(self):
        root, result = self.run_pipeline('6.2.0-daily.183', corrupt_after_sign=True)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((root / 'packaging-started').exists(), result.stdout + result.stderr)
        self.assertFalse((root / 'build/installer-Release/Moonlight-6.2.0-daily.183.dmg').exists())


if __name__ == '__main__':
    unittest.main()
