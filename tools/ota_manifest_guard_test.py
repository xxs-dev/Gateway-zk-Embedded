#!/usr/bin/env python3
"""Mainline guard tests, not candidate self-upgrade or rollback acceptance."""
import argparse
import ast
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import unittest
import zipfile

HOME = Path('/opt/modbus-gateway')
APPLY = Path(__file__).resolve().parents[1] / 'deploy/ota-apply.sh'
EVIDENCE = None


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data.encode() if isinstance(data, str) else data)


class MainlineGuardTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='mainline-ota-guard-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        if HOME.exists():
            shutil.rmtree(HOME)
        self.target = HOME / 'bin/production-smoke-test.sh'
        self.config = HOME / 'config/runtime/apps/sentinel.json'
        write(self.target, '#!/bin/sh\nexit 0\n')
        write(self.config, '{"keep":true}\n')
        self.stage = self.root / 'stage'
        write(self.stage / 'current_version.txt', 'jobId=PREVIOUS\nversion=previous\n')
        write(self.stage / 'applied_version.txt', 'previous\n')
        self.calls = self.root / 'systemctl.log'
        mock = self.root / 'mock'
        write(mock / 'systemctl', '#!/bin/sh\nprintf "%s\\n" "$*" >> "$SYSTEMCTL_LOG"\n')
        (mock / 'systemctl').chmod(0o755)
        self.env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1',
                        PATH=str(mock) + ':' + os.environ['PATH'], SYSTEMCTL_LOG=str(self.calls))

    def package(self, manifest=None, payload=b'#!/bin/sh\nexit 0\n'):
        members = {'bin/production-smoke-test.sh': payload}
        if manifest is not None:
            members['manifest.json'] = manifest.encode()
        path = self.root / 'guard.tar.gz'
        with tarfile.open(str(path), 'w:gz') as archive:
            for name, data in members.items():
                entry = tarfile.TarInfo(name)
                entry.size = len(data)
                entry.mode = 0o755 if name.endswith('.sh') else 0o644
                archive.addfile(entry, io.BytesIO(data))
        return path

    def invoke(self, path):
        result = subprocess.run(['sh', str(APPLY), str(path), 'mainline-guard-test',
                                 'MAINLINE_GUARD_TEST', str(self.root / 'backup'), str(self.stage)],
                                env=self.env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                universal_newlines=True, timeout=20)
        with (EVIDENCE / (self._testMethodName + '.log')).open('a') as output:
            output.write(result.stdout)
        return result

    def reject(self, path):
        result = self.invoke(path)
        self.assertNotEqual(0, result.returncode, result.stdout)
        self.assertEqual('#!/bin/sh\nexit 0\n', self.target.read_text())
        self.assertEqual('{"keep":true}\n', self.config.read_text())
        self.assertEqual('jobId=PREVIOUS\nversion=previous\n', (self.stage / 'current_version.txt').read_text())
        self.assertEqual('previous\n', (self.stage / 'applied_version.txt').read_text())
        self.assertFalse(self.calls.exists())

    def with_cleanup(self, files):
        return json.dumps({'files': files, 'cleanBeforeApply': [
            {'target': str(self.config.parent), 'patterns': [self.config.name]}]})

    def test_missing_root_manifest_rejected(self):
        self.reject(self.package())

    def test_empty_files_rejected_before_cleanup(self):
        self.reject(self.package(self.with_cleanup([])))

    def test_invalid_root_and_json_rejected(self):
        for source in ('null', '[]', 'true', '42', '"text"', '{broken'):
            with self.subTest(source=source):
                self.reject(self.package(source))

    def test_missing_or_nonarray_files_rejected(self):
        self.reject(self.package('{}'))
        for files in (None, {}, '', 'text', 2, False):
            with self.subTest(files=files):
                self.reject(self.package(self.with_cleanup(files)))

    def test_nonobject_entries_rejected_before_cleanup(self):
        for entry in (None, 'text', 2, []):
            with self.subTest(entry=entry):
                self.reject(self.package(self.with_cleanup([entry])))

    def test_allowed_script_install_and_requested_restart(self):
        payload = b'#!/bin/sh\n# Valid non-self-update fixture.\nexit 0\n'
        manifest = {'files': [{'path': 'bin/production-smoke-test.sh', 'target': str(self.target),
                              'sha256': hashlib.sha256(payload).hexdigest()}],
                    'restart': {'services': ['mqtt-driver@mqtt-service.service']}}
        result = self.invoke(self.package(json.dumps(manifest), payload))
        self.assertEqual(0, result.returncode, result.stdout)
        deadline = time.monotonic() + 8
        while not self.calls.exists() and time.monotonic() < deadline:
            time.sleep(0.1)
        self.assertTrue(self.calls.exists(), 'delayed restart was not observed')
        self.assertEqual(['restart mqtt-driver@mqtt-service.service'], self.calls.read_text().splitlines())
        self.assertEqual(payload, self.target.read_bytes())
        self.assertEqual(0o755, self.target.stat().st_mode & 0o777)
        self.assertEqual('mainline-guard-test\n', (self.stage / 'applied_version.txt').read_text())
        self.assertEqual('{"keep":true}\n', self.config.read_text())

    def test_first_entry_bad_checksum_preserves_file_and_version(self):
        manifest = {'files': [{'path': 'bin/production-smoke-test.sh', 'target': str(self.target),
                              'sha256': '0' * 64}], 'restart': {'services': []}}
        self.reject(self.package(json.dumps(manifest)))

    def test_scada_dispatch_preserved(self):
        # Stub only the external installer; exercise unchanged OTA dispatch.
        installer = HOME / 'bin/install-scada-project.sh'
        write(installer, '#!/bin/sh\nset -eu\nwhile [ "$#" -gt 0 ]; do\n'
              'if [ "$1" = --state-file ]; then shift; printf "fixture=installed\\n" > "$1"; fi\n'
              'shift\ndone\n')
        installer.chmod(0o755)
        write(HOME / 'config/runtime/device_identity.json', '{"machineCode":"SCADA_TEST"}')
        path = self.root / 'valid.kyscada'
        with zipfile.ZipFile(str(path), 'w') as archive:
            archive.writestr('manifest.json', '{"packageType":"scada"}')
        result = self.invoke(path)
        self.assertEqual(0, result.returncode, result.stdout)
        self.assertIn('SCADA success', result.stdout)
        self.assertIn('packageType=scada', (self.stage / 'current_version.txt').read_text())
        self.assertFalse(self.calls.exists())

    def test_python36_grammar_and_sh_bash_syntax(self):
        blocks = APPLY.read_text().split("<<'PY'\n")[1:]
        self.assertGreaterEqual(len(blocks), 2)
        for block in blocks:
            ast.parse(block.split('\nPY\n', 1)[0], feature_version=(3, 6))
        for shell in ('sh', 'bash'):
            subprocess.run([shell, '-n', str(APPLY)], check=True)


def main():
    global EVIDENCE, APPLY
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--apply', type=Path, default=APPLY)
    parser.add_argument('--parent-ns')
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error('root required for private mounts')
    if not args.parent_ns:
        return subprocess.call(['unshare', '--mount', '--ipc', '--fork', sys.executable, '-B',
                                str(Path(__file__).resolve()), *sys.argv[1:],
                                '--parent-ns', os.readlink('/proc/self/ns/mnt')])
    if args.parent_ns == os.readlink('/proc/self/ns/mnt'):
        parser.error('private mount namespace required')
    subprocess.run(['mount', '--make-rprivate', '/'], check=True)
    for path in ('/opt', '/run'):
        subprocess.run(['mount', '-t', 'tmpfs', '-o', 'mode=755', 'tmpfs', path], check=True)
    EVIDENCE = args.evidence.resolve()
    EVIDENCE.mkdir(parents=True, exist_ok=False)
    APPLY = args.apply.resolve()
    before = hashlib.sha256(APPLY.read_bytes()).hexdigest()
    write(EVIDENCE / 'isolation.json', json.dumps({
        'parentNamespace': args.parent_ns, 'namespace': os.readlink('/proc/self/ns/mnt'),
        'mounts': [json.loads(subprocess.check_output(['findmnt', '--json', '--target', path],
                   universal_newlines=True)) for path in ('/opt', '/run')],
        'applySha256': before,
        'testSha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'scope': 'Mainline guard only; mocked systemctl, stub SCADA installer, no binary execution or rollback claim'
    }, indent=2))
    with (EVIDENCE / 'result.txt').open('w') as output:
        result = unittest.TextTestRunner(stream=output, verbosity=2).run(
            unittest.defaultTestLoader.loadTestsFromTestCase(MainlineGuardTest))
    print((EVIDENCE / 'result.txt').read_text())
    unchanged = before == hashlib.sha256(APPLY.read_bytes()).hexdigest()
    write(EVIDENCE / 'source-integrity.json', json.dumps({
        'unchanged': unchanged, 'bytecodeDisabled': sys.dont_write_bytecode}, indent=2))
    return 0 if result.wasSuccessful() and unchanged else 1


if __name__ == '__main__':
    sys.exit(main())
