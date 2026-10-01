"""Real installer in private roots; no systemd/start/native binaries.

On Windows use Git sh (never WSL); only factory guard kernel inventory paths
map to empty private directories. This is not Linux/device installation proof.
"""
import ast
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import shlex
import subprocess
import sys
import tarfile
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
TRIO = ('offline-runtime-upgrade.py', 'runtime-upgrade-guard.py', 'ota-health-result.py')
SHELL = os.environ.get('FACTORY_TEST_SH') or ('C:/Program Files/Git/usr/bin/sh.exe' if os.name == 'nt' else '/bin/sh')


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data.encode() if isinstance(data, str) else data)


def inventory(root):
    return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in root.rglob('*') if p.is_file()}


def stage_module():
    spec = importlib.util.spec_from_file_location('factory_stage_test', REPO / 'deploy/factory-lifecycle-stage.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class LifecycleInstall(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='factory-lifecycle-')
        self.addCleanup(temp.cleanup); self.root = Path(temp.name)
        self.source = self.root / 'source'; self.external = self.root / 'external'
        for directory, label in ((self.source, 'source'), (self.external, 'external')):
            shutil.copytree(REPO / 'deploy', directory / 'deploy', ignore=shutil.ignore_patterns('__pycache__'))
            # Optional exact historical shell entries reproduce product RED while
            # retaining the same private kernel-path fixture and current tests.
            ref = os.environ.get('FACTORY_TEST_BASELINE_REF')
            if ref:
                for name in ('install-factory-config.sh', 'build-factory-package.sh'):
                    write(directory / 'deploy' / name, subprocess.check_output(
                        ['git', 'show', ref + ':deploy/' + name], cwd=REPO))
            for name in TRIO:
                path = directory / 'deploy' / name
                data = path.read_text() + '\n# fixture origin: ' + label + '\n'
                if name == 'runtime-upgrade-guard.py':
                    (self.root / 'empty-shm').mkdir(exist_ok=True)
                    (self.root / 'empty-proc').mkdir(exist_ok=True)
                    data = data.replace("Path('/dev/shm')", 'Path(' + repr(str(self.root / 'empty-shm')) + ')')
                    data = data.replace("Path('/proc')", 'Path(' + repr(str(self.root / 'empty-proc')) + ')')
                write(path, data)
            write(directory / 'config/factory/runtime/device_identity.json', '{"machineCode":"PRIVATE_FACTORY"}\n')
            for app in ('monitor-service', 'mqtt-service'):
                write(directory / ('config/factory/runtime/apps/' + app + '.json'), '{"enabled":false}\n')
            for binary in ('SystemMonitor', 'MqttDriver', 'MqttForwarder', 'pointctl'):
                write(directory / 'build-aarch64' / binary, b'private synthetic payload: never execute\n')
        self.commands = self.root / 'commands'; self.commands.mkdir()
        shim = self.commands / 'python3'
        write(shim, '#!/bin/sh\nexec ' + shlex.quote(Path(sys.executable).as_posix()) + ' "$@"\n')
        shim.chmod(0o755)
        self.env = os.environ.copy()
        self.env.update(PATH=str(self.commands) + os.pathsep + str(Path(SHELL).parent) + os.pathsep + self.env.get('PATH', ''),
                        GATEWAY_HOME=(self.root / 'home').as_posix(), SOURCE_ROOT=self.source.as_posix(),
                        DEFAULT_SOURCE_ROOT=(self.root / 'missing').as_posix(),
                        INSTALL_SYSTEMD='0', START_SERVICES='0', RESET_SHM='0', INIT_INTERACTIVE='0',
                        INIT_DIRECT_MAINTENANCE_ENABLED='0', INIT_MACHINE_CODE='PRIVATE_FACTORY',
                        TMPDIR=self.root.as_posix(), FACTORY_TMP_DIR=(self.root / 'extract').as_posix(),
                        OTA_PUBKEY_TARGET=(self.root / 'test.pub').as_posix(),
                        WATCHDOG_RUN_DIR=(self.root / 'watchdog').as_posix(), PYTHONDONTWRITEBYTECODE='1')
        if os.name == 'nt':
            # GNU tar otherwise interprets C:/... as a remote archive host.
            self.env['TAR_OPTIONS'] = '--force-local'
        for key in ('FACTORY_PACKAGE', 'DEPLOY_DIR', 'FACTORY_DIR', 'TEMPLATES_DIR', 'EXAMPLES_DIR'):
            self.env.pop(key, None)

    def install(self, script=None, **extra):
        env = dict(self.env, **extra)
        result = subprocess.run([SHELL, (script or self.source / 'deploy/install-factory-config.sh').as_posix(), '--profile', 'base'],
                                env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, timeout=40)
        return result, Path(env['GATEWAY_HOME'])

    def assert_installed(self, home, origin):
        for name in TRIO[1:]:
            self.assertEqual((home / 'bin' / name).read_bytes(), (origin / 'deploy' / name).read_bytes())
        tree = ast.parse((home / 'bin/offline-runtime-upgrade.py').read_text())
        assignments = [n for n in tree.body if isinstance(n, ast.Assign) and
                       any(isinstance(t, ast.Name) and t.id == 'CORE_TEST_APPLY_ALLOWED' for t in n.targets)]
        self.assertIs(ast.literal_eval(assignments[0].value), False)
        for name in ('ota-apply.sh', 'ota-rollback.sh', 'gateway-services.sh'):
            self.assertEqual((home / 'bin' / name).read_bytes(), (origin / 'deploy' / name).read_bytes())

    def package(self):
        output = self.root / 'factory.tar.gz'
        result = subprocess.run([SHELL, (self.source / 'deploy/build-factory-package.sh').as_posix(),
                                 '--profile', 'base', '--out', output.as_posix()],
                                env=self.env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, timeout=40)
        self.assertEqual(result.returncode, 0, result.stdout)
        target = self.root / 'packaged'
        with tarfile.open(output) as archive:
            for member in archive.getmembers():
                self.assertFalse(member.issym() or member.islnk())
                self.assertNotIn('..', Path(member.name).parts)
                self.assertTrue(member.name == 'gateway-factory-defaults' or
                                member.name.startswith('gateway-factory-defaults/'))
            archive.extractall(target)
        return output, target / 'gateway-factory-defaults'

    def test_raw_source_install_disables_core_without_editing_source(self):
        before = (self.source / 'deploy/offline-runtime-upgrade.py').read_bytes()
        result, home = self.install()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assert_installed(home, self.source)
        self.assertEqual(before, (self.source / 'deploy/offline-runtime-upgrade.py').read_bytes())

    def test_external_installer_keeps_source_trio_paired(self):
        result, home = self.install(self.external / 'deploy/install-factory-config.sh', DEPLOY_DIR=(self.external / 'deploy').as_posix())
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assert_installed(home, self.source)

    def test_built_package_wins_over_raw_source_and_external_deploy(self):
        before = inventory(self.source)
        archive, packaged = self.package()
        result, home = self.install(self.external / 'deploy/install-factory-config.sh',
                                    FACTORY_PACKAGE=archive.as_posix(), DEPLOY_DIR=(self.external / 'deploy').as_posix())
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assert_installed(home, packaged)
        self.assertEqual(before, inventory(self.source))
        self.assertEqual((home / 'bin/factory-lifecycle-stage.py').read_bytes(),
                         (self.external / 'deploy/factory-lifecycle-stage.py').read_bytes())
        self.assertEqual((home / 'bin/install-factory-config.sh').read_bytes(),
                         (self.external / 'deploy/install-factory-config.sh').read_bytes())

    def test_production_init_extracted_source_and_cleared_package_are_authoritative(self):
        _, packaged = self.package()
        before = inventory(packaged)
        # production-init exports its extracted SOURCE_ROOT and clears FACTORY_PACKAGE.
        result, home = self.install(self.external / 'deploy/install-factory-config.sh',
                                    SOURCE_ROOT=packaged.as_posix(), FACTORY_PACKAGE='',
                                    DEPLOY_DIR=(self.external / 'deploy').as_posix())
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assert_installed(home, packaged)
        self.assertEqual(before, inventory(packaged))

    def test_incomplete_or_unrecognized_package_cannot_fall_back_to_raw_source(self):
        _, packaged = self.package()
        (packaged / 'deploy/ota-health-result.py').unlink()
        broken = self.root / 'broken.tar.gz'
        with tarfile.open(broken, 'w:gz') as archive:
            archive.add(packaged, arcname='gateway-factory-defaults')
        result, home = self.install(self.external / 'deploy/install-factory-config.sh', FACTORY_PACKAGE=broken.as_posix())
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertFalse(home.exists(), result.stdout)
        with tarfile.open(broken, 'w:gz') as archive:
            item = tarfile.TarInfo('unrecognized'); item.type = tarfile.DIRTYPE
            archive.addfile(item)
        result, home = self.install(self.external / 'deploy/install-factory-config.sh', FACTORY_PACKAGE=broken.as_posix())
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn('factory package source root missing', result.stdout)
        self.assertFalse(home.exists(), result.stdout)

    def test_installed_bin_installer_has_paired_dependency_and_can_initialize_fresh_home(self):
        result, first = self.install(self.external / 'deploy/install-factory-config.sh')
        self.assertEqual(result.returncode, 0, result.stdout)
        result, second = self.install(first / 'bin/install-factory-config.sh',
                                      GATEWAY_HOME=(self.root / 'second-home').as_posix())
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assert_installed(second, self.source)
        self.assertEqual((first / 'bin/factory-lifecycle-stage.py').read_bytes(),
                         (second / 'bin/factory-lifecycle-stage.py').read_bytes())
        before = inventory(second)
        result, _ = self.install(second / 'bin/install-factory-config.sh', GATEWAY_HOME=second.as_posix())
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn('not an in-place runtime upgrade', result.stdout)
        self.assertEqual(before, inventory(second))

    def test_each_missing_authoritative_trio_member_rejects_before_home_creation(self):
        for name in TRIO:
            with self.subTest(name=name):
                path = self.source / 'deploy' / name
                original = path.read_bytes(); path.unlink()
                try:
                    result, home = self.install(self.external / 'deploy/install-factory-config.sh',
                                                DEPLOY_DIR=(self.external / 'deploy').as_posix())
                    self.assertNotEqual(result.returncode, 0, result.stdout)
                    self.assertFalse(home.exists(), result.stdout)
                finally:
                    write(path, original)

    def test_missing_installer_stage_helper_rejects_before_home_creation(self):
        (self.external / 'deploy/factory-lifecycle-stage.py').unlink()
        result, home = self.install(self.external / 'deploy/install-factory-config.sh')
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn('factory-lifecycle-stage.py', result.stdout)
        self.assertFalse(home.exists(), result.stdout)

    def test_malformed_authoritative_policy_rejects_before_home_creation(self):
        path = self.source / 'deploy/offline-runtime-upgrade.py'
        write(path, path.read_text().replace('CORE_TEST_APPLY_ALLOWED = True', 'CORE_TEST_APPLY_ALLOWED = bool(1)'))
        result, home = self.install(self.external / 'deploy/install-factory-config.sh')
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn('factory core policy assignment', result.stdout)
        self.assertFalse(home.exists(), result.stdout)

    def test_empty_roots_do_not_create_absolute_deploy_candidates(self):
        # Trace expanded filesystem-test argv, never plant bait in host root paths.
        script = (self.source / 'deploy/install-factory-config.sh').read_text()
        def section(begin, end):
            start = script.index(begin)
            return script[start:script.index(end, start)] + '\n'
        code = section('pick_existing_dir() {', '\npick_source_root() {')
        code += section('deploy_file() {', '\ninstall_deploy_file_if_exists()')
        code += section('find_first_file() {', '\ninstall_required_binary()')
        code += section('required_binary_file() {', '\nmanifest_profile()')
        code += 'set -x\ndeploy_file example.sh || true\nrequired_binary_file PrivateMissing || true\n'
        code += 'install_optional_binary PrivateMissing\ninstall_ky_ems_payload 0\n'
        config_end = '\nif [ -z "$FACTORY_DIR" ]' if '\nif [ -z "$FACTORY_DIR" ]' in script else '\nif [ ! -d "$FACTORY_DIR/runtime" ]'
        code += section('if [ -z "${FACTORY_DIR:-}" ]; then', config_end)
        code += section('if [ -z "$EDGE_PACKAGE_MANIFEST" ]; then', '\nif [ -z "$PACKAGE_PROFILE" ]; then')
        result = subprocess.run([SHELL, '-c', code], env=dict(self.env, PACKAGE_ROOT='', SOURCE_ROOT='',
                                DEFAULT_SOURCE_ROOT='', ROOT_DIR='', SCRIPT_DIR='', DEPLOY_DIR='',
                                EDGE_PACKAGE_MANIFEST='', LIFECYCLE_DEPLOY_DIR='', LIFECYCLE_STAGE_DIR=''), stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout)
        probes = []
        for line in result.stdout.splitlines():
            if line.startswith('+'):
                body = line.lstrip('+ ')
                if not body.startswith(("'[' -f ", "'[' -d ", '[ -f ', '[ -d ')):
                    continue
                args = shlex.split(body)
                if len(args) >= 3 and args[0] == '[' and args[1] in ('-f', '-d'):
                    probes.append(args[2])
        self.assertTrue(probes, result.stdout)
        self.assertFalse(any(path.startswith('/') for path in probes), probes)
        for root in ('PACKAGE_ROOT', 'SOURCE_ROOT', 'DEFAULT_SOURCE_ROOT', 'ROOT_DIR', 'DEPLOY_DIR'):
            self.assertTrue('"$' + root + '/' not in script, 'unguarded optional root: ' + root)


class StagePolicy(unittest.TestCase):
    def test_literal_value_only_preserves_adjacent_unicode_statements(self):
        raw = 'label="中文"; CORE_TEST_APPLY_ALLOWED=True; kept=3\n'.encode()
        self.assertEqual(stage_module().disabled_entry(raw), raw.replace(b'=True;', b'=False;'))

    def test_nonliteral_duplicate_nested_and_annotated_bindings_reject(self):
        for raw in (b'CORE_TEST_APPLY_ALLOWED=bool(1)\n', b'CORE_TEST_APPLY_ALLOWED=True\nCORE_TEST_APPLY_ALLOWED=False\n',
                    b'CORE_TEST_APPLY_ALLOWED=True\ndef f():\n CORE_TEST_APPLY_ALLOWED=False\n',
                    b'CORE_TEST_APPLY_ALLOWED: bool=True\n', b'other=CORE_TEST_APPLY_ALLOWED=True\n',
                    b'CORE_TEST_APPLY_ALLOWED=True\ndef CORE_TEST_APPLY_ALLOWED(): pass\n',
                    b'CORE_TEST_APPLY_ALLOWED=True\nimport os as CORE_TEST_APPLY_ALLOWED\n',
                    b'CORE_TEST_APPLY_ALLOWED=True\ndef f(CORE_TEST_APPLY_ALLOWED): pass\n'):
            with self.subTest(raw=raw), self.assertRaisesRegex(ValueError, 'policy assignment'):
                stage_module().disabled_entry(raw)

    def test_complete_trio_validated_before_output_mutation(self):
        with tempfile.TemporaryDirectory(prefix='factory-stage-') as directory:
            root = Path(directory); source = root / 'source'; output = root / 'output'
            write(source / TRIO[0], 'CORE_TEST_APPLY_ALLOWED=True\n')
            write(source / TRIO[1], 'invalid python !!!\n')
            write(source / TRIO[2], 'valid=1\n')
            with self.assertRaises(SyntaxError): stage_module().stage(source, output)
            self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main(verbosity=2)
