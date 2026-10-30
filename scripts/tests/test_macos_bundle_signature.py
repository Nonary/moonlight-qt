"""Check final DMG input signatures with real codesign and a tiny build fixture.

No app is launched, no DMG is mounted, and no signing credentials are required.
Only the build/deployment tools are stubbed; signature validation is real.
"""
from pathlib import Path
import json
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
                     missing_nested_signature=False, create_dmg='compatible',
                     signing_identity='', faults=(), diskutil_image=False):
        """Run generate-dmg.sh against stubs.

        create_dmg is 'compatible' (sindresorhus CLI), 'incompatible' (another
        tool named create-dmg) or 'missing'. faults names TEST_FAIL_* switches
        understood by the stubs below.
        """
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
# Real codesign for bundles. A requested Developer ID is replaced with an ad hoc
# signature, and DMG images (fake files here) record their signer as text.
import json, os, pathlib, subprocess, sys
arguments = sys.argv[1:]
with open(os.path.join(os.environ['TEST_ROOT'], 'codesign-log'), 'a') as log:
    log.write(json.dumps(arguments) + '\\n')
target = pathlib.Path(arguments[-1])
if target.suffix == '.dmg':
    if '--sign' in arguments:
        if os.environ.get('TEST_FAIL_DMG_SIGN') == '1':
            sys.exit(1)
        with target.open('a') as image:
            image.write('signed-by:' + arguments[arguments.index('--sign') + 1] + '\\n')
        sys.exit(0)
    if '--verify' in arguments:
        if os.environ.get('TEST_FAIL_DMG_VERIFY') == '1':
            sys.exit(1)
        sys.exit(0 if 'signed-by:' in target.read_text() else 1)
    sys.exit(1)
forwarded = []
skip = False
for index, argument in enumerate(arguments):
    if skip:
        skip = False
        continue
    if argument == '--timestamp':
        continue
    if argument == '--options':
        skip = True
        continue
    if index > 0 and arguments[index - 1] == '--sign':
        argument = '-'
    forwarded.append(argument)
subprocess.run(['/usr/bin/codesign', *forwarded], check=True)
if '--sign' in arguments and os.environ.get('TEST_CORRUPT_AFTER_SIGN') == '1':
    with (target / 'Contents/Helpers/Helper').open('ab') as output:
        output.write(b'invalidating nested code signature after signing')
''')
        if create_dmg == 'compatible':
            self.write_tool(tools, 'create-dmg', '''#!/bin/bash
# Answer the generate-dmg.sh CLI probe like sindresorhus/create-dmg does.
if [ "$1" = "--help" ]; then
  echo '  $ create-dmg <app> [destination]'
  exit 0
fi
# Record whether invalid nested code was caught before packaging started.
touch "$TEST_ROOT/packaging-started"
printf '%s\\n' "$@" > "$TEST_ROOT/dmg-arguments"
/usr/bin/codesign --verify --deep --strict "$1" || exit 1
echo 'create-dmg image' > "$2/Moonlight.dmg"
if [ "$TEST_FAIL_CREATE_DMG" = "1" ]; then
  # Leave a partial image behind, as a real failure might.
  exit 1
fi
for argument in "$@"; do
  case "$argument" in
    --identity=*) echo "signed-by:${argument#--identity=}" >> "$2/Moonlight.dmg";;
  esac
done
# sindresorhus/create-dmg exits 2 when the image was created but not signed.
if [ "$TEST_CREATE_DMG_SKIPPED_SIGNING" = "1" ]; then exit 2; fi
exit 0
''')
        elif create_dmg == 'incompatible':
            self.write_tool(tools, 'create-dmg', '''#!/bin/bash
# The Homebrew create-dmg/create-dmg script takes different arguments.
if [ "$1" = "--help" ]; then
  echo 'Usage:  create-dmg [options] <output_name.dmg> <source_folder>'
  exit 0
fi
touch "$TEST_ROOT/incompatible-create-dmg-invoked"
exit 1
''')
        else:
            self.assertEqual(create_dmg, 'missing')
        self.write_tool(tools, 'mkdir', '''#!/bin/bash
if [ "$TEST_FAIL_STAGING_MKDIR" = "1" ] && [[ "$*" == *dmg-staging* ]]; then exit 1; fi
exec /bin/mkdir "$@"
''')
        self.write_tool(tools, 'ln', '''#!/bin/bash
if [ "$TEST_FAIL_STAGING_LN" = "1" ] && [[ "$*" == *dmg-staging* ]]; then exit 1; fi
if [ "$TEST_WRONG_STAGING_LINK" = "1" ] && [[ "$*" == *dmg-staging* ]]; then
  exec /bin/ln -s /tmp "${@: -1}"
fi
exec /bin/ln "$@"
''')
        self.write_tool(tools, 'ditto', '''#!/bin/bash
if [ "$TEST_FAIL_DITTO" = "1" ]; then exit 1; fi
if [ "$TEST_PARTIAL_DITTO" = "1" ]; then
  # Report success after copying only part of the bundle.
  /bin/mkdir -p "$2/Contents"
  cp "$1/Contents/Info.plist" "$2/Contents/Info.plist"
  exit 0
fi
exec /usr/bin/ditto "$@"
''')
        image_tool = '''#!/usr/bin/env python3
# Record the staged tree that would be imaged, then write a placeholder image.
import json, os, pathlib, subprocess, sys
arguments = sys.argv[1:]
tool = pathlib.Path(sys.argv[0]).name
if tool == 'diskutil':
    if os.environ.get('TEST_DISKUTIL_IMAGE') != '1':
        sys.exit(1)
    if arguments[0] == 'help':
        sys.exit(0)
    source, output = pathlib.Path(arguments[-2]), pathlib.Path(arguments[-1])
else:
    source = pathlib.Path(arguments[arguments.index('-srcfolder') + 1])
    output = pathlib.Path(arguments[-1])
app = source / 'Moonlight.app'
link = source / 'Applications'
verified = subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict', str(app)],
                          capture_output=True).returncode == 0
with open(os.path.join(os.environ['TEST_ROOT'], 'imaged-tree'), 'w') as record:
    json.dump({'tool': tool, 'entries': sorted(entry.name for entry in source.iterdir()),
               'app_verified': verified,
               'applications_link': os.readlink(link) if link.is_symlink() else None}, record)
if os.environ.get('TEST_FAIL_IMAGE') == '1':
    output.write_text('partial image')
    sys.exit(1)
output.write_text('native image\\n')
'''
        self.write_tool(tools, 'hdiutil', image_tool)
        self.write_tool(tools, 'diskutil', image_tool)
        path = os.environ['PATH']
        if create_dmg == 'missing':
            # Hide any create-dmg installed on the machine running the tests.
            path = ':'.join(directory for directory in path.split(':')
                            if not os.access(os.path.join(directory, 'create-dmg'), os.X_OK))
        environment = os.environ.copy()
        environment.update(PATH=f'{tools}:{path}', CI_VERSION=version,
                           SIGNING_IDENTITY=signing_identity, SIGNING_PROVIDER_SHORTNAME='',
                           NOTARY_KEYCHAIN_PROFILE='', TEST_ROOT=str(root),
                           TEST_DISKUTIL_IMAGE=str(int(diskutil_image)),
                           TEST_MISSING_APP_SIGNATURE=str(int(missing_app_signature)),
                           TEST_MISSING_NESTED_SIGNATURE=str(int(missing_nested_signature)),
                           TEST_CORRUPT_AFTER_SIGN=str(int(corrupt_after_sign)))
        for fault in faults:
            environment[fault] = '1'
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


    def assert_no_package(self, root, result, version='6.2.0'):
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertNotIn('Build successful', result.stdout)
        installer = root / 'build/installer-Release'
        self.assertFalse((installer / f'Moonlight-{version}.dmg').exists(), output)
        self.assertFalse((installer / 'Moonlight.dmg').exists(), output)
        self.assertFalse((root / 'build/build-Release/dmg-staging').exists(), output)

    def imaged_tree(self, root):
        path = root / 'imaged-tree'
        return json.loads(path.read_text()) if path.exists() else None

    def dmg_codesign_calls(self, root):
        log = root / 'codesign-log'
        calls = [json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []
        return [call for call in calls if call[-1].endswith('.dmg')]

    def test_native_fallback_images_verified_staging(self):
        for create_dmg in ('missing', 'incompatible'):
            with self.subTest(create_dmg=create_dmg):
                root, result = self.run_pipeline('6.2.0', create_dmg=create_dmg)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn('Build successful', result.stdout)
                tree = self.imaged_tree(root)
                self.assertEqual(tree['tool'], 'hdiutil')
                self.assertEqual(tree['entries'], ['Applications', 'Moonlight.app'])
                self.assertTrue(tree['app_verified'])
                self.assertEqual(tree['applications_link'], '/Applications')
                dmg = root / 'build/installer-Release/Moonlight-6.2.0.dmg'
                self.assertEqual(dmg.read_text(), 'native image\n')
                self.assertFalse((root / 'build/build-Release/dmg-staging').exists())
                self.assertFalse((root / 'incompatible-create-dmg-invoked').exists())
                # Unsigned packages are never signed by the fallback.
                self.assertEqual(self.dmg_codesign_calls(root), [])

    def test_native_fallback_prefers_diskutil(self):
        root, result = self.run_pipeline('6.2.0', create_dmg='missing', diskutil_image=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.imaged_tree(root)['tool'], 'diskutil')
        self.assertTrue((root / 'build/installer-Release/Moonlight-6.2.0.dmg').is_file())

    def test_native_fallback_staging_failures_do_not_package(self):
        for fault in ('TEST_FAIL_STAGING_MKDIR', 'TEST_FAIL_DITTO', 'TEST_PARTIAL_DITTO',
                      'TEST_FAIL_STAGING_LN', 'TEST_WRONG_STAGING_LINK'):
            with self.subTest(fault=fault):
                root, result = self.run_pipeline('6.2.0', create_dmg='missing', faults=(fault,))
                self.assert_no_package(root, result)
                # The image is never created from an incomplete staging tree.
                self.assertIsNone(self.imaged_tree(root), result.stdout + result.stderr)

    def test_native_fallback_image_failure_removes_partial_image(self):
        root, result = self.run_pipeline('6.2.0', create_dmg='missing', faults=('TEST_FAIL_IMAGE',))
        self.assert_no_package(root, result)
        self.assertIsNotNone(self.imaged_tree(root))

    def test_unsigned_create_dmg_failure_does_not_fall_back(self):
        root, result = self.run_pipeline('6.2.0', faults=('TEST_FAIL_CREATE_DMG',))
        self.assert_no_package(root, result)
        self.assertIsNone(self.imaged_tree(root))

    def test_unsigned_create_dmg_signing_skip_status_is_accepted(self):
        root, result = self.run_pipeline('6.2.0', faults=('TEST_CREATE_DMG_SKIPPED_SIGNING',))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue((root / 'build/installer-Release/Moonlight-6.2.0.dmg').is_file())
        self.assertIsNone(self.imaged_tree(root))

    def test_signed_create_dmg_signs_image(self):
        root, result = self.run_pipeline('6.2.0', signing_identity='Test Identity')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('--identity=Test Identity', (root / 'dmg-arguments').read_text().splitlines())
        dmg = root / 'build/installer-Release/Moonlight-6.2.0.dmg'
        self.assertIn('signed-by:Test Identity', dmg.read_text())
        self.assertIsNone(self.imaged_tree(root))

    def test_signed_create_dmg_failure_fails_closed(self):
        root, result = self.run_pipeline('6.2.0', signing_identity='Test Identity',
                                         faults=('TEST_FAIL_CREATE_DMG',))
        self.assert_no_package(root, result)
        # A failed signed create-dmg must not degrade to an unsigned native image.
        self.assertIsNone(self.imaged_tree(root))
        self.assertEqual(self.dmg_codesign_calls(root), [])

    def test_signed_native_fallback_signs_and_verifies_image(self):
        for create_dmg in ('missing', 'incompatible'):
            with self.subTest(create_dmg=create_dmg):
                root, result = self.run_pipeline('6.2.0', create_dmg=create_dmg,
                                                 signing_identity='Test Identity')
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                dmg = root / 'build/installer-Release/Moonlight-6.2.0.dmg'
                self.assertEqual(dmg.read_text(), 'native image\nsigned-by:Test Identity\n')
                calls = self.dmg_codesign_calls(root)
                self.assertEqual(len(calls), 2, calls)
                self.assertIn('--sign', calls[0])
                self.assertEqual(calls[0][calls[0].index('--sign') + 1], 'Test Identity')
                self.assertIn('--verify', calls[1])

    def test_signed_native_fallback_signing_failures_do_not_package(self):
        for fault in ('TEST_FAIL_DMG_SIGN', 'TEST_FAIL_DMG_VERIFY'):
            with self.subTest(fault=fault):
                root, result = self.run_pipeline('6.2.0', create_dmg='missing',
                                                 signing_identity='Test Identity',
                                                 faults=(fault,))
                self.assert_no_package(root, result)


if __name__ == '__main__':
    unittest.main()
