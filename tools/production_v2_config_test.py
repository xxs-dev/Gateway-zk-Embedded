"""Private-file v2 config write-set and real guard-stage regressions."""

import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]


def load():
    spec = importlib.util.spec_from_file_location('production_v2_offline',
                                                  ROOT / 'deploy/offline-runtime-upgrade.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@unittest.skipUnless(hasattr(os, 'geteuid') and os.geteuid() == 0,
                     'root-owned private config fixture required')
class ProductionV2ConfigTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='production-v2-config-')
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name)
        self.m = load()
        self.g = self.m.guard
        self.runtime = self.m.ProductionRuntime(self.home)
        self.tx = self.runtime.root / 'transactions/TEST_01'
        self.tx.mkdir(parents=True)
        self.config = self.home / 'config/runtime'
        self.write('device_identity.json', b'{"machineCode":"SIM"}\n')
        self.write('apps/current.json', b'{"runtimeMode":"gateway"}\n')
        self.old = self.g.production_pin(self.config / 'apps/current.json')
        self.new_raw = b'{"runtimeMode":"ems"}\n'
        self.new = dict(sha256=hashlib.sha256(self.new_raw).hexdigest(),
                        bytes=len(self.new_raw), mode=0o644)
        identity = self.g.production_pin(self.config / 'device_identity.json')['sha256']
        self.previous = {'files': {}, 'configSha256': {'device_identity.json': identity,
                                                       'apps/current.json': self.old['sha256']}}
        self.candidate = {'files': {}, 'configSha256': {'device_identity.json': identity,
                                                        'apps/current.json': self.new['sha256']}}
        self.approval = {'format': 'runtime-provisioning-v2', 'previous': self.previous,
                         'candidate': self.candidate, 'configWriteSet': [
                             {'path': 'apps/current.json', 'before': self.old, 'after': self.new}]}
        self.runtime.copy_ownership(self.tx, self.approval)

    def write(self, relative, data, mode=0o644):
        path = self.config / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        path.chmod(mode)
        return path

    def test_registered_partial_temp_is_recoverable_but_foreign_or_owner_drift_is_not(self):
        temporary = self.runtime.copy_name(self.tx, 'config/runtime/apps/current.json', 'install')
        temporary.write_bytes(self.new_raw[:3])
        temporary.chmod(0o644)
        self.runtime.config_state(self.tx, self.approval)
        foreign = temporary.with_name(temporary.name + '-foreign')
        foreign.write_bytes(b'foreign')
        foreign.chmod(0o644)
        with self.assertRaisesRegex(ValueError, 'unowned production config inventory'):
            self.runtime.config_state(self.tx, self.approval)
        foreign.unlink()
        owners = self.g.production_json(self.tx / 'copy-owners.json')
        owners.pop(str(temporary))
        self.m.save(self.tx / 'copy-owners.json', owners)
        with self.assertRaisesRegex(ValueError, 'staging ownership journal drift'):
            self.runtime.config_state(self.tx, self.approval)

    def test_empty_foreign_directory_is_not_approved_mixed_state(self):
        foreign = self.config / 'unknown-empty'
        foreign.mkdir()
        with self.assertRaisesRegex(ValueError, 'unowned production config directory'):
            self.runtime.config_state(self.tx, self.approval)

    def test_known_empty_directory_with_writable_permissions_is_not_approved(self):
        known = self.config / 'logic'
        known.mkdir()
        known.chmod(0o777)
        with self.assertRaisesRegex(ValueError, 'unowned production config directory'):
            self.runtime.config_state(self.tx, self.approval)

    def test_registered_temp_with_extra_permission_is_not_archived(self):
        temporary = self.runtime.copy_name(self.tx, 'config/runtime/apps/current.json', 'install')
        temporary.write_bytes(self.new_raw[:3])
        temporary.chmod(0o744)
        with self.assertRaisesRegex(ValueError, 'unowned production config staging bytes'):
            self.runtime.config_state(self.tx, self.approval)
        with self.assertRaisesRegex(ValueError, 'interrupted copy type/size/mode'):
            self.runtime.archive_copies(self.tx, self.approval)
        self.assertTrue(temporary.is_file())
        self.assertEqual(temporary.stat().st_mode & 0o777, 0o744)

    def test_real_guard_preserves_identity_but_defers_full_tree_until_restore(self):
        self.write('apps/current.json', self.new_raw)
        temporary = self.runtime.copy_name(self.tx, 'config/runtime/apps/current.json', 'install')
        temporary.write_bytes(self.new_raw[:3])
        temporary.chmod(0o644)
        self.runtime.config_state(self.tx, self.approval)
        baseline = {'format': 'runtime-baseline-v1', 'machineCode': 'SIM',
                    'trust': {'publicKeySha256': 'a' * 64, 'requireSignature': True,
                              'checksumRequired': True, 'allowInsecureHttp': False,
                              'httpsOrigins': ['https://fixture.invalid']},
                    'configSha256': self.previous['configSha256'],
                    'segments': [], 'retiredSources': {}, 'historical': {}}
        original_pin = self.g.production_pin
        def trust_pin(path):
            if str(path) == '/etc/modbus-gateway/ota.pub':
                return {'sha256': 'a' * 64}
            return original_pin(path)
        with mock.patch.object(self.g, 'production_schema'), \
                mock.patch.object(self.g, 'production_pin', side_effect=trust_pin), \
                mock.patch.object(self.g, 'no_old_mappings'):
            self.g.production_verify(self.home, baseline, files=False, units=False,
                                     segments=False, config=False, roles=False)
            with self.assertRaisesRegex(ValueError, 'configuration/identity drift'):
                self.g.production_verify(self.home, baseline, files=False, units=False,
                                         segments=False, config=True)
            self.write('device_identity.json', b'{"machineCode":"OTHER"}\n')
            with self.assertRaisesRegex(ValueError, 'configuration/identity drift'):
                self.g.production_verify(self.home, baseline, files=False, units=False,
                                         segments=False, config=False, roles=False)

    def test_empty_v2_code_only_write_set_is_exact_and_v1_is_unchanged(self):
        approval = dict(self.approval, configWriteSet=[])
        with self.assertRaisesRegex(ValueError, 'write set incomplete'):
            self.runtime.config_changes(approval)
        approval['candidate'] = self.previous
        self.assertEqual(self.runtime.config_changes(approval), [])
        approval['format'] = 'runtime-provisioning-v1'
        self.assertEqual(self.runtime.config_changes(approval), [])

    def test_config_write_set_rejects_path_order_presence_mode_and_size_drift(self):
        def rejects(change, message):
            approval = copy.deepcopy(self.approval)
            change(approval)
            with self.assertRaisesRegex(ValueError, message):
                self.runtime.config_changes(approval)
        for path in ('device_identity.json', 'logic/../escape.json',
                     'logic/nested/escape.json'):
            with self.subTest(path=path):
                rejects(lambda a, value=path: a['configWriteSet'][0].update(path=value),
                        'outside reviewed write set')
        rejects(lambda a: a['configWriteSet'].append(copy.deepcopy(a['configWriteSet'][0])),
                'duplicate production config path')
        rejects(lambda a: a['configWriteSet'][0].update(before=None),
                'production config presence mismatch')
        rejects(lambda a: a['configWriteSet'][0]['after'].update(mode=0o666),
                'production config pin invalid')
        rejects(lambda a: a['configWriteSet'][0]['after'].update(bytes=5 * 1024 * 1024 + 1),
                'production config pin invalid')

        def add_second(approval):
            approval['previous']['configSha256']['apps/second.json'] = 'a' * 64
            approval['candidate']['configSha256']['apps/second.json'] = 'b' * 64
            approval['configWriteSet'].insert(0, {
                'path': 'apps/second.json',
                'before': {'sha256': 'a' * 64, 'mode': 0o644, 'bytes': 1},
                'after': {'sha256': 'b' * 64, 'mode': 0o644, 'bytes': 1}})
        rejects(add_second, 'write set incomplete or unordered')

        def over_total(approval):
            approval['previous']['configSha256'] = {'device_identity.json':
                approval['previous']['configSha256']['device_identity.json']}
            approval['candidate']['configSha256'] = dict(approval['previous']['configSha256'])
            approval['configWriteSet'] = []
            for index in range(10):
                path = 'logic/item_' + str(index) + '.json'
                approval['previous']['configSha256'][path] = 'a' * 64
                approval['candidate']['configSha256'][path] = 'b' * 64
                approval['configWriteSet'].append({
                    'path': path,
                    'before': {'sha256': 'a' * 64, 'mode': 0o600, 'bytes': 5 * 1024 * 1024},
                    'after': {'sha256': 'b' * 64, 'mode': 0o600, 'bytes': 5 * 1024 * 1024}})
        rejects(over_total, 'write set exceeds bound')


if __name__ == '__main__':
    unittest.main()
