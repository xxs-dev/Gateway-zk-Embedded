#!/usr/bin/env python3
"""Exercise OTA scripts in a private mount namespace, never on a device."""
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

REPO = Path(__file__).resolve().parents[1]
HOME = Path('/opt/modbus-gateway')
EVIDENCE = None
APPLY = None
BASELINE = None
CANDIDATE = None


def write(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content.encode() if isinstance(content, str) else content)


class OtaManifestTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='ota-manifest-test-')
        self.root = Path(self.temp.name)
        self.addCleanup(self.temp.cleanup)
        if HOME.exists():
            shutil.rmtree(HOME)
        (HOME / 'bin').mkdir(parents=True)
        self.stage = self.root / 'staging'
        self.backup = self.root / 'backup'
        write(self.stage / 'current_version.txt', 'jobId=PREVIOUS\nversion=previous\n')
        write(self.stage / 'applied_version.txt', 'previous\n')
        write(HOME / 'bin/sentinel', b'keep-program\n')
        write(HOME / 'config/runtime/apps/sentinel.json', b'{"keep":true}\n')
        self.env = dict(os.environ, WATCHDOG_RUN_DIR=str(self.root / 'watchdog'),
                        PYTHONDONTWRITEBYTECODE='1')
        mock = self.root / 'mock'
        write(mock / 'systemctl', '#!/bin/sh\nprintf "%s\\n" "$*" >> "$SYSTEMCTL_LOG"\n')
        (mock / 'systemctl').chmod(0o755)
        self.env['PATH'] = str(mock) + ':' + os.environ['PATH']
        self.env['SYSTEMCTL_LOG'] = str(self.root / 'systemctl.log')

    def package(self, members, name='guard.tar.gz'):
        archive = self.root / name
        with tarfile.open(str(archive), 'w:gz') as stream:
            for path, data in members.items():
                data = data.encode() if isinstance(data, str) else data
                info = tarfile.TarInfo(path)
                info.size = len(data)
                info.mode = 0o755 if path.endswith('.sh') else 0o644
                stream.addfile(info, io.BytesIO(data))
        return archive

    def run_apply(self, archive, script=None, version='guard-test', job='GUARD_TEST'):
        result = subprocess.run(['sh', str(script or APPLY), str(archive), version,
                                 job, str(self.backup), str(self.stage)],
                                env=self.env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, universal_newlines=True, timeout=20)
        with (EVIDENCE / (self._testMethodName + '.log')).open('a') as log:
            log.write(result.stdout)
        return result

    def wait_restart(self):
        deadline = time.monotonic() + 8
        while (self.root / 'watchdog/applying').exists() and time.monotonic() < deadline:
            time.sleep(0.1)
        self.assertFalse((self.root / 'watchdog/applying').exists())

    def assert_rejected(self, archive):
        result = self.run_apply(archive)
        self.assertNotEqual(0, result.returncode, result.stdout)
        self.assertEqual('jobId=PREVIOUS\nversion=previous\n',
                         (self.stage / 'current_version.txt').read_text())
        self.assertEqual('previous\n', (self.stage / 'applied_version.txt').read_text())
        self.assertEqual(b'keep-program\n', (HOME / 'bin/sentinel').read_bytes())
        self.assertEqual(b'{"keep":true}\n', (HOME / 'config/runtime/apps/sentinel.json').read_bytes())
        self.assertFalse((self.root / 'systemctl.log').exists())
        self.assertFalse((self.root / 'watchdog/applying').exists())

    def test_missing_manifest_fails_closed(self):
        self.assert_rejected(self.package({'bin/payload': b'not-an-ota-manifest'}))

    def test_target_python36_and_shell_syntax(self):
        source = APPLY.read_text()
        blocks = source.split("<<'PY'\n")[1:]
        self.assertGreaterEqual(len(blocks), 2)
        for block in blocks:
            ast.parse(block.split('\nPY\n', 1)[0], feature_version=(3, 6))
        subprocess.run(['sh', '-n', str(APPLY)], check=True)

    def test_empty_files_fails_before_clean(self):
        self.assert_rejected(self.package({'manifest.json': json.dumps({
            'files': [], 'cleanBeforeApply': [
                {'target': str(HOME / 'config/runtime/apps'), 'patterns': ['sentinel.json']}
            ]})}))

    def test_invalid_manifest_shapes_fail_before_clean(self):
        clean = [{'target': str(HOME / 'config/runtime/apps'), 'patterns': ['sentinel.json']}]
        cases = [None, [], 'text', True, {}, {'files': None}, {'files': {}},
                 {'files': ''}, {'files': 'not-an-array'}, {'files': 2}, {'files': False}]
        for manifest in cases:
            with self.subTest(manifest=manifest):
                if isinstance(manifest, dict):
                    manifest['cleanBeforeApply'] = clean
                self.assert_rejected(self.package({'manifest.json': json.dumps(manifest)}))
        self.assert_rejected(self.package({'manifest.json': '{invalid json'}))

    def test_files_entries_must_be_objects_before_clean(self):
        for item in (None, 'text', 1, []):
            with self.subTest(item=item):
                self.assert_rejected(self.package({'manifest.json': json.dumps({
                    'files': [item], 'cleanBeforeApply': [
                        {'target': str(HOME / 'config/runtime/apps'), 'patterns': ['sentinel.json']}
                    ]})}))

    def test_valid_program_manifest_and_restart(self):
        data = b'valid-program-fixture\n'
        manifest = {'files': [{'path': 'bin/MqttDriver',
                    'target': str(HOME / 'bin/MqttDriver'),
                    'sha256': hashlib.sha256(data).hexdigest()}],
                    'restart': {'services': ['mqtt-driver@mqtt-service.service']}}
        result = self.run_apply(self.package({'manifest.json': json.dumps(manifest),
                                             'bin/MqttDriver': data}))
        self.assertEqual(0, result.returncode, result.stdout)
        self.wait_restart()
        self.assertEqual(data, (HOME / 'bin/MqttDriver').read_bytes())
        self.assertEqual('guard-test\n', (self.stage / 'applied_version.txt').read_text())
        self.assertEqual(['restart mqtt-driver@mqtt-service.service'],
                         (self.root / 'systemctl.log').read_text().splitlines())

    def test_scada_branch_delegates_without_program_files(self):
        # Only the external installer is stubbed; OTA routing/state handling is real.
        installer = HOME / 'bin/install-scada-project.sh'
        write(installer, '#!/bin/sh\nset -eu\nwhile [ "$#" -gt 0 ]; do\n'
              'if [ "$1" = --state-file ]; then shift; printf "fixture=installed\\n" > "$1"; fi\n'
              'shift\ndone\n')
        installer.chmod(0o755)
        write(HOME / 'config/runtime/device_identity.json', '{"machineCode":"SCADA_TEST"}')
        archive = self.root / 'valid.kyscada'
        with zipfile.ZipFile(str(archive), 'w') as stream:
            stream.writestr('manifest.json', '{"packageType":"scada"}')
        result = self.run_apply(archive)
        self.assertEqual(0, result.returncode, result.stdout)
        self.assertIn('SCADA success', result.stdout)
        self.assertIn('packageType=scada', (self.stage / 'current_version.txt').read_text())
        self.assertFalse((self.root / 'systemctl.log').exists())

    def test_existing_self_upgrade_and_partial_rollback(self):
        shutil.rmtree(HOME)
        # A disposable native Git fixture avoids Windows worktree .git paths in WSL.
        fixture = self.root / 'repository'
        write(fixture / 'deploy/ota-apply.sh', BASELINE.read_bytes())
        shutil.copy2(REPO / 'deploy/ota-rollback.sh', fixture / 'deploy/ota-rollback.sh')
        write(fixture / 'tools/ota_apply_self_upgrade_test.sh',
              (REPO / 'tools/ota_apply_self_upgrade_test.sh').read_bytes())
        for command in (['git', 'init', '-q'], ['git', 'add', '.'],
                        ['git', '-c', 'user.name=OTA Fixture', '-c', 'user.email=fixture@localhost',
                         '-c', 'commit.gpgsign=false', 'commit', '--no-verify', '-qm', 'baseline']):
            subprocess.run(command, cwd=str(fixture), check=True, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, env=self.env)
        write(fixture / 'deploy/ota-apply.sh', APPLY.read_bytes())
        for path in (fixture / 'deploy').iterdir():
            path.chmod(0o755)
        result = subprocess.run(['sh', str(fixture / 'tools/ota_apply_self_upgrade_test.sh')],
                                env=self.env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                universal_newlines=True, timeout=90)
        write(EVIDENCE / (self._testMethodName + '.log'), result.stdout)
        self.assertEqual(0, result.returncode, result.stdout)

    def test_exact_candidate_bootstrap_restart_and_partial_rollback(self):
        if CANDIDATE is None:
            self.skipTest('--candidate required for artifact verification')
        provenance = json.loads((CANDIDATE / 'provenance.json').read_text())
        success = provenance['success']
        failure = provenance['partialFailure']
        payloads = {}
        for record in (success, failure):
            artifact = CANDIDATE / record['file']
            self.assertEqual(record['sha256'], hashlib.sha256(artifact.read_bytes()).hexdigest())
            self.assertEqual(record['sizeBytes'], artifact.stat().st_size)
            with tarfile.open(str(artifact), 'r:gz') as archive:
                manifest = json.load(archive.extractfile('manifest.json'))
                self.assertEqual(record['manifest'], manifest)
                self.assertEqual(provenance['sourceCommit'], manifest['sourceCommit'])
                allowed = {'ota-apply.sh', 'MqttDriver'} if record is success else {'ota-apply.sh', 'ota-rollback.sh'}
                self.assertEqual(set(archive.getnames()), {'manifest.json'} | {'bin/' + name for name in allowed})
                self.assertEqual({item['target'] for item in manifest['files']},
                                 {str(HOME / 'bin' / name) for name in allowed})
                self.assertNotIn('cleanBeforeApply', manifest)
                self.assertNotIn('updateIdentity', manifest)
                self.assertEqual(['mqtt-driver@mqtt-service.service'] if record is success else [],
                                 manifest['restart']['services'])
                mismatches = []
                for item in manifest['files']:
                    data = archive.extractfile(item['path']).read()
                    sha = hashlib.sha256(data).hexdigest()
                    self.assertEqual(record['payloadSha256'][Path(item['path']).name], sha)
                    self.assertEqual(0o755, archive.getmember(item['path']).mode)
                    if sha != item['sha256']:
                        mismatches.append(item['path'])
                    payloads[(record['version'], item['path'])] = data
                self.assertEqual([] if record is success else ['bin/ota-rollback.sh'], mismatches)
        fixed = payloads[(success['version'], 'bin/ota-apply.sh')]
        fixture_apply = payloads[(failure['version'], 'bin/ota-apply.sh')]
        self.assertEqual(APPLY.read_bytes(), fixed)
        self.assertEqual(fixed + b'\n# Test-only partial-apply rollback receipt.\n', fixture_apply)
        mqtt = payloads[(success['version'], 'bin/MqttDriver')]
        rollback = (REPO / 'deploy/ota-rollback.sh').read_bytes()
        write(HOME / 'bin/ota-apply.sh', BASELINE.read_bytes())
        write(HOME / 'bin/MqttDriver', mqtt)
        write(HOME / 'bin/ota-rollback.sh', rollback)
        for path in (HOME / 'bin').iterdir():
            path.chmod(0o755)
        result = self.run_apply(CANDIDATE / success['file'], HOME / 'bin/ota-apply.sh',
                                success['version'], 'BOOTSTRAP_TEST')
        self.assertEqual(0, result.returncode, result.stdout)
        self.wait_restart()
        self.assertEqual(fixed, (HOME / 'bin/ota-apply.sh').read_bytes())
        self.assertEqual(mqtt, (HOME / 'bin/MqttDriver').read_bytes())
        self.assertEqual(['restart mqtt-driver@mqtt-service.service'],
                         (self.root / 'systemctl.log').read_text().splitlines())
        previous_state = (self.stage / 'current_version.txt').read_bytes()
        result = self.run_apply(CANDIDATE / failure['file'], HOME / 'bin/ota-apply.sh',
                                failure['version'], 'PARTIAL_TEST')
        self.assertNotEqual(0, result.returncode, result.stdout)
        self.assertIn('manifest checksum mismatch for bin/ota-rollback.sh', result.stdout)
        self.assertEqual(fixture_apply, (HOME / 'bin/ota-apply.sh').read_bytes())
        self.assertEqual(rollback, (HOME / 'bin/ota-rollback.sh').read_bytes())
        self.assertEqual(fixed, (self.backup / 'PARTIAL_TEST/opt/modbus-gateway/bin/ota-apply.sh').read_bytes())
        self.assertFalse((self.backup / 'PARTIAL_TEST/opt/modbus-gateway/bin/ota-rollback.sh').exists())
        result = subprocess.run(['sh', str(HOME / 'bin/ota-rollback.sh'),
                                 str(CANDIDATE / failure['file']), failure['version'],
                                 'PARTIAL_TEST', str(self.backup), str(self.stage)],
                                env=self.env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                universal_newlines=True, timeout=20)
        write(EVIDENCE / 'exact-candidate-rollback.log', result.stdout)
        self.assertEqual(0, result.returncode, result.stdout)
        self.assertEqual(fixed, (HOME / 'bin/ota-apply.sh').read_bytes())
        self.assertEqual(mqtt, (HOME / 'bin/MqttDriver').read_bytes())
        self.assertEqual(rollback, (HOME / 'bin/ota-rollback.sh').read_bytes())
        self.assertEqual(previous_state, (self.stage / 'current_version.txt').read_bytes())
        self.assertEqual(success['version'] + '\n', (self.stage / 'applied_version.txt').read_text())
        self.assertEqual(b'{"keep":true}\n', (HOME / 'config/runtime/apps/sentinel.json').read_bytes())
        self.assertEqual(['restart mqtt-driver@mqtt-service.service'],
                         (self.root / 'systemctl.log').read_text().splitlines())


def main():
    global EVIDENCE, APPLY, BASELINE, CANDIDATE
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--apply', type=Path, default=REPO / 'deploy/ota-apply.sh')
    parser.add_argument('--baseline-apply', type=Path)
    parser.add_argument('--candidate', type=Path)
    parser.add_argument('--test', action='append')
    parser.add_argument('--parent-ns')
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error('root is required for isolated mounts')
    if not args.parent_ns:
        return subprocess.call(['unshare', '--mount', '--ipc', '--fork', sys.executable,
                                '-B', str(Path(__file__).resolve()), *sys.argv[1:],
                                '--parent-ns', os.readlink('/proc/self/ns/mnt')])
    if args.parent_ns == os.readlink('/proc/self/ns/mnt'):
        parser.error('private mount namespace required')
    subprocess.run(['mount', '--make-rprivate', '/'], check=True)
    for path in ('/opt', '/run'):
        subprocess.run(['mount', '-t', 'tmpfs', '-o', 'mode=755', 'tmpfs', path], check=True)
    EVIDENCE = args.evidence.resolve()
    EVIDENCE.mkdir(parents=True, exist_ok=False)
    APPLY = args.apply.resolve()
    BASELINE = (args.baseline_apply or args.apply).resolve()
    CANDIDATE = args.candidate.resolve() if args.candidate else None
    write(EVIDENCE / 'isolation.json', json.dumps({
        'parentMountNamespace': args.parent_ns,
        'mountNamespace': os.readlink('/proc/self/ns/mnt'),
        'mounts': [json.loads(subprocess.check_output(
            ['findmnt', '--json', '--target', path], universal_newlines=True))
            for path in ('/opt', '/run')],
        'applySha256': hashlib.sha256(APPLY.read_bytes()).hexdigest(),
        'baselineApplySha256': hashlib.sha256(BASELINE.read_bytes()).hexdigest(),
        'testSha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
    }, indent=2))
    suite = (unittest.TestSuite(OtaManifestTest(name) for name in args.test) if args.test
             else unittest.defaultTestLoader.loadTestsFromTestCase(OtaManifestTest))
    def snapshot():
        return {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                for path in CANDIDATE.iterdir() if path.is_file()} if CANDIDATE else {}
    before = snapshot()
    with (EVIDENCE / 'result.txt').open('w') as output:
        result = unittest.TextTestRunner(stream=output, verbosity=2).run(suite)
    print((EVIDENCE / 'result.txt').read_text())
    after = snapshot()
    write(EVIDENCE / 'candidate-integrity.json', json.dumps({
        'before': before, 'after': after, 'unchanged': before == after,
        'bytecodeDisabled': sys.dont_write_bytecode}, indent=2))
    return 0 if result.wasSuccessful() and before == after else 1


if __name__ == '__main__':
    sys.exit(main())
