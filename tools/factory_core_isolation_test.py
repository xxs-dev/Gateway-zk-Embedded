"""D1 factory artifacts and simulated offline transactions; no real services or SHM."""
import ast
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock
import shlex

import core_readonly_activation_test as core
from factory_lifecycle_install_test import SHELL

REPO = Path(__file__).resolve().parent.parent
MESSAGE = 'core test apply is disabled in factory packages'
FILES = ('build-factory-package.sh', 'factory-lifecycle-stage.py', 'offline-runtime-upgrade.py', 'runtime-upgrade-guard.py',
         'production_role_policy.py',
         'gateway-services.sh', 'ota-apply.sh', 'ota-rollback.sh', 'ota-health-result.py',
         'production-smoke-test.sh')
BINS = ('SystemMonitor MqttDriver MqttForwarder pointctl ModbusRtu Dlt645Driver DioDriver '
        'CanDriver IecDriver EventEngine ComputeEngine EmsParityCheck EmsClusterCoordinator '
        'memory_point_store_migrate').split()


def load(path):
    spec = importlib.util.spec_from_file_location('factory_offline_d1', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def inventory(root):
    return {str(p.relative_to(root)): (p.stat().st_mode, hashlib.sha256(p.read_bytes()).hexdigest())
            for p in root.rglob('*') if p.is_file()}


class FactoryCoreIsolationTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='factory-core-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / 'source'
        for name in FILES:
            core.write(self.source / 'deploy' / name, (REPO / 'deploy' / name).read_bytes())
        core.write(self.source / 'config/factory/runtime/apps/monitor-service.json', '{}\n')
        for name in BINS:
            core.write(self.source / 'build-aarch64' / name, 'not executable: synthetic package payload\n')
        self.env = os.environ.copy()
        if os.name == 'nt':
            commands = self.root / 'commands'; commands.mkdir()
            core.write(commands / 'python3', '#!/bin/sh\nexec ' + shlex.quote(Path(sys.executable).as_posix()) + ' "$@"\n')
            (commands / 'python3').chmod(0o755)
            self.env.update(PATH=str(commands) + os.pathsep + str(Path(SHELL).parent) + os.pathsep + self.env.get('PATH', ''),
                            TAR_OPTIONS='--force-local', PYTHONDONTWRITEBYTECODE='1')

    def build(self, profile='full'):
        output = self.root / (profile + '.tar.gz')
        args = [SHELL, (self.source / 'deploy/build-factory-package.sh').as_posix(), '--profile', profile, '--out', output.as_posix()]
        if profile == 'project':
            manifest = self.root / 'project.json'
            core.write(manifest, json.dumps({'requiredDrivers': [{'binary': 'ModbusRtu'}]}))
            args += ['--manifest', manifest.as_posix()]
        env = self.env.copy()
        env.update(TMPDIR=self.root.as_posix(), EDGE_PACKAGE_BUILD_DIR=(self.source / 'build-aarch64').as_posix(),
                   GIT_OPTIONAL_LOCKS='0')
        return subprocess.run(args, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True, timeout=45), output

    def unpack(self, output, name='unpacked'):
        target = self.root / name
        with tarfile.open(output) as archive:
            for row in archive.getmembers():
                self.assertFalse(row.issym() or row.islnk())
                self.assertTrue(row.name == 'gateway-factory-defaults' or
                                row.name.startswith('gateway-factory-defaults/'))
                self.assertNotIn('..', Path(row.name).parts)
            archive.extractall(target)
        return target / 'gateway-factory-defaults'

    def test_all_factory_profiles_disable_new_core_apply_and_keep_guard(self):
        original = inventory(self.source)
        for profile in ('base', 'project', 'full'):
            with self.subTest(profile=profile):
                result, output = self.build(profile)
                self.assertEqual(0, result.returncode, result.stdout)
                package = self.unpack(output, 'unpacked-' + profile)
                tree = ast.parse((package / 'deploy/offline-runtime-upgrade.py').read_bytes())
                assignment = next(n for n in tree.body if isinstance(n, ast.Assign) and
                                  any(isinstance(t, ast.Name) and t.id == 'CORE_TEST_APPLY_ALLOWED' for t in n.targets))
                self.assertIs(False, ast.literal_eval(assignment.value))
                self.assertEqual((REPO / 'deploy/runtime-upgrade-guard.py').read_bytes(),
                                 (package / 'deploy/runtime-upgrade-guard.py').read_bytes())
                self.assertEqual((REPO / 'deploy/production_role_policy.py').read_bytes(),
                                 (package / 'deploy/production_role_policy.py').read_bytes())
                self.assertEqual((REPO / 'deploy/ota-health-result.py').read_bytes(),
                                 (package / 'deploy/ota-health-result.py').read_bytes())
                if os.name == 'posix':
                    module = load(package / 'deploy/offline-runtime-upgrade.py')
                    self.assertTrue(callable(module.recover))
                    self.assertTrue(callable(module.guard.activated_unit))
                    self.assertTrue(callable(module.guard.ota))
        self.assertEqual(original, inventory(self.source))

    def test_unknown_policy_assignment_refuses_package_publication(self):
        path = self.source / 'deploy/offline-runtime-upgrade.py'
        core.write(path, path.read_text().replace('CORE_TEST_APPLY_ALLOWED = True',
                                                 'CORE_TEST_APPLY_ALLOWED = os.getenv("TEST_CORE")'))
        result, output = self.build()
        self.assertNotEqual(0, result.returncode, result.stdout)
        self.assertIn('factory core policy assignment', result.stdout)
        self.assertFalse(output.exists())

    @unittest.skipUnless(os.name == 'posix', 'real offline main requires Linux fcntl; do not substitute')
    def test_packaged_real_main_rejects_both_test_nodes_before_lock_or_mutation(self):
        result, output = self.build()
        self.assertEqual(0, result.returncode, result.stdout)
        package = self.unpack(output)
        module = load(package / 'deploy/offline-runtime-upgrade.py')
        for node, profile in (('COMM202600999', 'RTU_MQTT_MONITOR'), ('COMM202600998', 'MONITOR_ONLY')):
            with self.subTest(node=node):
                approval = self.root / 'approval.json'
                core.write(approval, json.dumps({'nodeId': node, 'coreReadOnlyTest': {'profile': profile}}))
                argv = ['offline-runtime-upgrade.py', 'apply', '--approval', str(approval),
                        '--approval-sha256', core.sha(approval), '--manifest', str(self.root / 'missing-manifest'),
                        '--payload', str(self.root / 'missing-payload'), '--state', str(self.root / 'transaction')]
                before = inventory(self.root)
                with mock.patch.object(sys, 'argv', argv), mock.patch.object(module.os, 'geteuid', return_value=0), \
                        mock.patch.object(module.os, 'open', side_effect=AssertionError('unexpected lock/mutation')) as opened, \
                        mock.patch.object(module, 'systemctl', side_effect=AssertionError('unexpected service action')) as systemctl:
                    with self.assertRaisesRegex(ValueError, MESSAGE):
                        module.main()
                opened.assert_not_called()
                systemctl.assert_not_called()
                self.assertEqual(before, inventory(self.root))
                self.assertFalse((self.root / 'transaction').exists())

    def test_factory_policy_does_not_disable_existing_noncore_profile(self):
        f = core.fixture.AcquisitionTest('test_apply_activate_recover_restores_exact_bytes')
        f.setUp()
        self.addCleanup(f.doCleanups)
        f.m.CORE_TEST_APPLY_ALLOWED = False
        f.apply()
        f.activate()
        f.m.recover(f.args)
        self.assertEqual('RECOVERED_STOPPED', json.loads((f.state_dir / 'state.json').read_text())['phase'])
        self.assertFalse(f.live)


class CorePolicyCompatibilityTest(unittest.TestCase):
    def setUp(self):
        case = self.case = core.CoreReadOnlyTest('test_monitor_only_absent_qt_preserves_original_unit_scope')
        case.setUp()
        self.addCleanup(case.doCleanups)
        self.f = case.f

    def test_direct_apply_refuses_before_service_state_shm_or_file_mutation(self):
        f = self.f
        f.m.CORE_TEST_APPLY_ALLOWED = False
        core.write(f.root / 'approval.json', json.dumps(f.approval))
        before = inventory(f.root)
        with self.assertRaisesRegex(ValueError, MESSAGE):
            f.apply()
        self.assertEqual(before, inventory(f.root))
        self.assertEqual([], f.calls)
        self.assertFalse(f.state_dir.exists())
        self.assertFalse(f.live)

    def test_existing_core_transactions_validate_and_recover_with_apply_disabled(self):
        f = self.f
        f.apply()
        f.activate(self.case.readers())
        f.m.CORE_TEST_APPLY_ALLOWED = False
        f.g.activated_unit(f.home, f.g.MONITOR_UNIT)
        f.m.recover(f.args)
        self.assertEqual('RECOVERED_STOPPED', json.loads((f.state_dir / 'state.json').read_text())['phase'])
        self.assertFalse(f.live)
        for name, old in f.originals.items():
            self.assertEqual(old, (f.config / name).read_bytes())

    def test_monitor_only_saved_activation_and_recovery_are_not_disabled(self):
        self.case.without_qt()
        f = self.f
        f.apply()
        f.m.CORE_TEST_APPLY_ALLOWED = False
        f.activate([f.g.MONITOR_UNIT])
        f.g.activated_unit(f.home, f.g.MONITOR_UNIT)
        f.m.recover(f.args)
        self.assertEqual('RECOVERED_STOPPED', json.loads((f.state_dir / 'state.json').read_text())['phase'])
        self.assertFalse(f.live)


if __name__ == '__main__':
    unittest.main()
