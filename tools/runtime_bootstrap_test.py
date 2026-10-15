"""Root-owned private preparation fixtures; native constructor/systemd substituted."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import time
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
MARKER = b'create-empty requires exactly --shm <new-name>'


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@unittest.skipUnless(getattr(os, 'geteuid', lambda: -1)() == 0, 'Linux root private filesystem fixture')
class BootstrapTest(unittest.TestCase):
    def setUp(self):
        fixture = load('bootstrap_reprovision_fixture', ROOT / 'tools/gateway_reprovision_test.py')
        self.f = fixture.ReprovisionTest('test_install_expands_units_and_keeps_original_image')
        self.f.setUp()
        self.addCleanup(self.f.doCleanups)
        f = self.f
        self.external_run = subprocess.run
        self.m = load('bootstrap_fixture', ROOT / 'deploy/runtime-bootstrap.py')
        m = self.m
        self.patch(m, 'reprovision', f.m)
        self.patch(m, 'lifecycle', f.m.lifecycle)
        self.patch(m, 'guard', f.m.guard)
        self.patch(m, 'HOME', f.home)
        self.patch(m, 'ROOT', f.root / 'bootstrap')
        self.patch(m, 'SHM', f.root / 'shm')
        self.patch(m, 'TRUST', f.root / 'ota.pub')
        self.patch(m, 'BOOT', f.root / 'boot-id')
        m.SHM.mkdir(mode=0o755)
        f.write(m.SHM / 'retained_old', b'old store never touched', 0o600)
        f.write(m.TRUST, b'private fixture public key')
        f.write(m.BOOT, b'FIXTURE_BOOT\n')
        self.patch(m.guard, 'systemctl_value', lambda u, p: f.systemctl('show', '--property=' + p, '--value', u))
        self.patch(m.lifecycle, 'production_owner_proof', lambda: {})
        self.patch(m.lifecycle, 'production_locked_owner', lambda value: True)
        self.patch(m.lifecycle, 'no_mappings', lambda *a, **k: None)
        self.real_capacity = m.capacity
        self.patch(m, 'capacity', lambda *a: None)
        self.patch(m, 'helper_pins', lambda: {'fixture': 'pinned'})
        for name in ('system-monitor@.service', 'gateway-services.service'):
            f.write(f.new_units / name, (ROOT / 'deploy' / name).read_bytes())
        header = bytearray(64)
        header[:6] = b'\x7fELF\x02\x01'
        struct.pack_into('<H', header, 18, 183)
        f.write(f.image / 'bin/pointctl', bytes(header) + MARKER, 0o755)
        if self._testMethodName == 'test_prepare_with_real_native_constructor_in_private_tmpfs':
            f.write(f.image / 'bin/pointctl', Path(os.environ['GATEWAY_TEST_BOOTSTRAP_NATIVE']).read_bytes(), 0o755)
        f.write(f.image / 'config/runtime/edge-package-manifest.json', b'{"components":[]}\n')
        for name in ('mqtt-service', 'monitor-service'):
            value = {'identityConfigFile': '/opt/modbus-gateway/config/runtime/device_identity.json',
                     'mqtt': {'enabled': False},
                     'mqttDriver': {'enabled': False, 'sharedMemoryName': 'fresh_data'}}
            if name == 'monitor-service':
                value['systemMonitor'] = {'enabled': True}
                value['localDisplay'] = {'enabled': False}
            f.write(f.image / ('config/runtime/apps/' + name + '.json'), json.dumps(value).encode())
        self.prepare_image()
        self.reprovision_transaction, self.reprovision_pin = f.install()
        self.source = f.root / 'config-payload'
        shutil.copytree(f.home / 'config/runtime', self.source)
        self.authority = {'machineCode': 'FIXTURE',
            'identitySha256': m.guard.production_pin(self.source / 'device_identity.json')['sha256'],
            'networkSha256': {}, 'tlsSha256': {},
            'otaPublicKeySha256': m.guard.production_pin(m.TRUST)['sha256'], 'monitorShmName': 'fresh_monitor'}
        resolved = m.policy.resolve(self.source, f.units, self.authority, m.TRUST)
        self.profile = {'format': 'board-simulator-profile-v1', 'machineCode': 'FIXTURE', 'ramMiB': 2048,
                        'capabilities': [], 'simulatorBindings': {},
                        'allowedUnits': [r['unit'] for r in resolved['roles']],
                        'segmentNames': sorted({n for r in resolved['roles'] for n in r['shm']['names']})}
        self.trust = {'requireSignature': True, 'checksumRequired': True, 'allowInsecureHttp': False,
                      'publicKeySha256': self.authority['otaPublicKeySha256'], 'httpsOrigins': ['https://fixture.invalid']}
        self.native_calls = []
        self.patch(m.subprocess, 'run', self.native)
        self.runtime = m.Bootstrap()
        self.initial_home = f.m.tree(f.home)
        f.calls.clear()

    def patch(self, *args, **kwargs):
        patch = mock.patch.object(*args, **kwargs)
        result = patch.start()
        self.addCleanup(patch.stop)
        return result

    def prepare_image(self):
        pass

    def native(self, args, **kwargs):
        m = self.m
        self.native_calls.append(args)
        self.assertEqual(args[:3], [str(m.HOME / 'bin/pointctl'), 'create-empty', '--shm'])
        self.assertEqual(kwargs['env'], {'PATH': '/usr/bin:/bin', 'LC_ALL': 'C'})
        name = args[3]
        path = m.SHM / name
        with path.open('xb') as stream:
            stream.write(struct.pack('<II', 0x4d505354, 11))
            stream.truncate(m.guard.POINT_STORE_SIZES[11])
        path.chmod(0o600)
        output = ('shm=/' + name + ' latest=0/1 latestCapacity=1 pendingWrites=0/1 pendingCapacity=1 '
                  'persistent=0/1 persistentCapacity=1 pointUpdates=0/1 writeSeq=0 persistentSeq=0 pointUpdateSeq=0\n')
        return subprocess.CompletedProcess(args, 0, output.encode(), b'')

    def approval(self, name='BOOTSTRAP_ONE'):
        m = self.m
        value = m.plan(self.source, m.HOME / 'bin/pointctl', name, int(time.time()) + 600,
                       self.authority, self.profile, self.trust)
        path = self.f.root / (name + '.json')
        self.f.m.record(path, value, new=True)
        return path, self.f.m.digest(path)

    def prepare(self):
        path, pin = self.approval()
        return self.runtime.prepare(path, pin), pin

    def disable_ota(self):
        self.m.TRUST.unlink()
        self.authority['otaPublicKeySha256'] = None
        self.trust.update(otaEnabled=False, publicKeySha256=None, httpsOrigins=[])
        for path in (self.source / 'apps').glob('*.json'):
            value = json.loads(path.read_text())
            value['ota'] = {'enabled': False}
            value.setdefault('mqtt', {})['otaRequestTopic'] = ''
            self.f.write(path, json.dumps(value).encode())

    def test_explicit_offline_policy_prepares_without_key_or_origins(self):
        self.disable_ota()
        transaction, _ = self.prepare()
        self.assertFalse(self.m.TRUST.exists())
        self.assertIs(self.f.m.read_record(transaction / 'ready.json')['trust']['otaEnabled'], False)
        self.assert_untouched()

    def test_offline_policy_rejects_implicit_or_enabled_ota(self):
        self.disable_ota()
        path = self.source / 'apps/monitor-service.json'
        original = json.loads(path.read_text())
        for key, value in (('ota', {}), ('ota', {'enabled': True}),
                           ('mqtt', {'enabled': False}), ('mqtt', {'otaRequestTopic': 'edge/ota/request'})):
            with self.subTest(key=key, value=value):
                changed = dict(original, **{key: value})
                self.f.write(path, json.dumps(changed).encode())
                with self.assertRaisesRegex(ValueError, 'disabled OTA requires'):
                    self.approval()
        self.assertFalse(self.m.ROOT.exists())
        self.assert_untouched()

    def assert_untouched(self):
        f, m = self.f, self.m
        self.assertEqual(f.m.tree(f.home), self.initial_home)
        self.assertEqual((m.SHM / 'retained_old').read_bytes(), b'old store never touched')
        self.assertEqual((f.journal / 'STOP').read_bytes(), f.m.STOP_BODY)
        self.assertFalse(any(c[0] != 'show' for c in f.calls), f.calls)
        self.assertFalse((m.HOME / 'data/runtime-production').exists())

    def test_prepare_binds_fresh_empty_receipt_without_activation(self):
        transaction, pin = self.prepare()
        state = self.f.m.read_record(transaction / 'state.json')
        ready = self.f.m.read_record(transaction / 'ready.json')
        self.assertEqual(state['phase'], 'PREPARED_STOPPED')
        self.assertEqual(ready['activation'], 'not-authorized')
        self.assertEqual(ready['approvalSha256'], pin)
        self.assertEqual([r['name'] for r in ready['segments']], self.profile['segmentNames'])
        self.assertEqual(len(self.native_calls), 2)
        self.assertEqual(self.f.m.tree(transaction / 'config'), self.f.m.tree(self.source))
        self.assertFalse((self.m.ROOT / 'pending.json').exists())
        self.assert_untouched()

    def test_recovery_retains_native_objects_and_is_idempotent(self):
        transaction, pin = self.prepare()
        before = self.m.inventory()
        self.runtime.recover('BOOTSTRAP_ONE', pin)
        self.runtime.recover('BOOTSTRAP_ONE', pin)
        self.assertEqual(self.m.inventory(), before)
        self.assertEqual(self.f.m.read_record(transaction / 'state.json')['phase'], 'ABORTED_STOPPED')
        self.assertFalse((self.m.ROOT / 'current.json').exists())
        self.assert_untouched()

    def test_plan_requires_unoccupied_fresh_names(self):
        self.f.write(self.m.SHM / self.profile['segmentNames'][0], b'foreign existing data', 0o600)
        with self.assertRaisesRegex(ValueError, 'already occupied'):
            self.approval()
        self.assertFalse(self.m.ROOT.exists())
        self.assert_untouched()

    def test_old_creator_is_rejected_without_invoking_unknown_command(self):
        with mock.patch.object(self.m.guard, 'production_pin', wraps=self.m.guard.production_pin):
            original = self.m.origin
            before = original()
            self.f.write(self.m.HOME / 'bin/pointctl', b'\x7fELF' + b'\x02\x01' + b'\0' * 12 + b'\xb7\0', 0o755)
            with mock.patch.object(self.m, 'origin', return_value=before):
                with self.assertRaisesRegex(ValueError, 'lacks exclusive'):
                    self.approval()
        self.assertFalse(self.native_calls)
        self.assertFalse(self.m.ROOT.exists())

    def test_approval_config_and_creator_drift_refuse_before_journal(self):
        path, pin = self.approval()
        self.f.write(self.source / 'logic/new.json', b'{}')
        with self.assertRaises(ValueError):
            self.runtime.prepare(path, pin)
        self.assertFalse(self.m.ROOT.exists())
        self.assertFalse(self.native_calls)
        self.assert_untouched()

    def test_missing_trust_and_insecure_origin_refuse_before_journal(self):
        self.m.TRUST.unlink()
        with self.assertRaises((ValueError, FileNotFoundError)):
            self.approval()
        self.f.write(self.m.TRUST, b'private fixture public key')
        self.trust['httpsOrigins'] = ['http://fixture.invalid']
        with self.assertRaises(ValueError):
            self.approval()
        self.assertFalse(self.m.ROOT.exists())
        self.assert_untouched()

    def test_profile_and_identity_mismatch_refuse(self):
        self.profile['segmentNames'].append('unreviewed_store')
        with self.assertRaisesRegex(ValueError, 'profile differs'):
            self.approval()
        self.profile['segmentNames'].pop()
        self.f.write(self.source / 'device_identity.json', b'{"machineCode":"OTHER"}')
        with self.assertRaisesRegex(ValueError, 'identity/package'):
            self.approval()
        self.assertFalse(self.m.ROOT.exists())

    def test_missing_loaded_fence_refuses(self):
        original = self.m.guard.systemctl_value
        with mock.patch.object(self.m.guard, 'systemctl_value',
                               side_effect=lambda u, p: '' if p == 'DropInPaths' else original(u, p)):
            with self.assertRaisesRegex(ValueError, 'not loaded'):
                self.approval()
        self.assertFalse(self.native_calls)

    def test_missing_offline_lock_and_capacity_refuse_before_journal(self):
        path, pin = self.approval()
        with mock.patch.object(self.m.lifecycle, 'production_locked_owner', return_value=False):
            with self.assertRaisesRegex(ValueError, 'lock ownership'):
                self.runtime.prepare(path, pin)
        with mock.patch.object(self.m, 'capacity', side_effect=ValueError('insufficient bootstrap memory/tmpfs')):
            with self.assertRaisesRegex(ValueError, 'insufficient'):
                self.runtime.prepare(path, pin)
        self.assertFalse(self.m.ROOT.exists())
        self.assert_untouched()

    def test_capacity_uses_shared_kernel_mount_check(self):
        approval = {'roleAuthority': {'roles': []}}
        with mock.patch.object(self.m.lifecycle, 'tmpfs_mount') as mount, \
             mock.patch.object(self.m.lifecycle, 'mem_available_bytes', return_value=1024 ** 3), \
             mock.patch.object(self.m.os, 'statvfs', return_value=mock.Mock(f_frsize=4096, f_bavail=262144)):
            self.real_capacity(approval, 2)
            mount.assert_called_once_with(self.m.SHM)
            mount.side_effect = ValueError('exact tmpfs mount required')
            with self.assertRaisesRegex(ValueError, 'exact tmpfs mount'):
                self.real_capacity(approval, 2)
        self.assertFalse(self.m.ROOT.exists())
        self.assert_untouched()

    def test_native_failure_retains_partial_store_for_stopped_recovery(self):
        path, pin = self.approval()
        def failed(args, **kwargs):
            self.f.write(self.m.SHM / args[3], b'\0' * 8, 0o600)
            raise OSError('injected native crash')
        with mock.patch.object(self.m.subprocess, 'run', failed):
            with self.assertRaisesRegex(OSError, 'native crash'):
                self.runtime.prepare(path, pin)
        before = self.m.inventory()
        self.runtime.recover('BOOTSTRAP_ONE', pin)
        self.assertEqual(self.m.inventory(), before)
        self.assert_untouched()

    def test_checkpoint_crashes_have_stopped_recovery_without_shm_replay(self):
        path, pin = self.approval()
        original = self.runtime.checkpoint
        def crash(transaction, state, phase):
            original(transaction, state, phase)
            if state['segments']:
                raise OSError('injected receipt crash')
        with mock.patch.object(self.runtime, 'checkpoint', crash):
            with self.assertRaisesRegex(OSError, 'receipt crash'):
                self.runtime.prepare(path, pin)
        before = self.m.inventory()
        self.runtime.recover('BOOTSTRAP_ONE', pin)
        self.assertEqual(self.m.inventory(), before)
        self.assertEqual(len(self.native_calls), 1)
        self.assert_untouched()

    def interrupted_publication(self, target, after=False):
        path, pin = self.approval()
        original = self.m.lifecycle.write_new
        publish = self.m.lifecycle.publish_new
        def interrupted_write(destination, data, mode=0o600):
            if destination.name.startswith('.' + target + '.stage-'):
                original(destination, data[:9], mode)
                raise OSError('injected publication interruption')
            return original(destination, data, mode)
        def interrupted_publish(destination, data, mode=0o600):
            publish(destination, data, mode)
            if destination.name == target:
                raise OSError('injected publication interruption')
        attribute = 'publish_new' if after else 'write_new'
        with mock.patch.object(self.m.lifecycle, attribute,
                               interrupted_publish if after else interrupted_write):
            with self.assertRaisesRegex(OSError, 'publication interruption'):
                self.runtime.prepare(path, pin)
        transaction = self.m.ROOT / 'transactions/BOOTSTRAP_ONE'
        destination = self.m.ROOT / target if target in ('pending.json', 'current.json') else transaction / target
        self.assertEqual(destination.exists(), after)
        if target == 'approval.json' and not after:
            with self.assertRaises(FileNotFoundError):
                self.runtime.recover('BOOTSTRAP_ONE', pin)
            self.assertFalse(self.native_calls)
            self.assertFalse((self.m.ROOT / 'pending.json').exists())
        else:
            before = self.m.inventory()
            self.runtime.recover('BOOTSTRAP_ONE', pin)
            self.assertEqual(self.m.inventory(), before)
            self.assertEqual(self.f.m.read_record(transaction / 'state.json')['phase'], 'ABORTED_STOPPED')
            self.assertFalse((self.m.ROOT / 'pending.json').exists())
            self.assertFalse((self.m.ROOT / 'current.json').exists())
        self.assert_untouched()

    def test_partial_approval_never_becomes_authority(self):
        self.interrupted_publication('approval.json')

    def test_durable_approval_before_initial_state_can_be_aborted(self):
        self.interrupted_publication('approval.json', after=True)

    def test_partial_pending_pointer_can_be_recovered(self):
        self.interrupted_publication('pending.json')

    def test_partial_ready_receipt_keeps_created_stores(self):
        self.interrupted_publication('ready.json')

    def test_partial_current_pointer_keeps_created_stores(self):
        self.interrupted_publication('current.json')

    def test_current_published_before_pending_removal_can_be_recovered(self):
        self.interrupted_publication('current.json', after=True)

    def test_config_staging_interruption_does_not_create_stores(self):
        path, pin = self.approval()
        def partial(source, destination, expected):
            destination.mkdir()
            self.f.write(destination / 'partial.json', b'{')
            raise OSError('injected staging interruption')
        with mock.patch.object(self.m.reprovision, 'copy_tree', partial):
            with self.assertRaisesRegex(OSError, 'staging interruption'):
                self.runtime.prepare(path, pin)
        self.runtime.recover('BOOTSTRAP_ONE', pin)
        self.assertFalse(self.native_calls)
        self.assert_untouched()

    def test_missing_initial_state_does_not_override_an_existing_pointer(self):
        path, pin = self.approval()
        with mock.patch.object(self.runtime, 'checkpoint', side_effect=OSError('before initial state')):
            with self.assertRaisesRegex(OSError, 'before initial state'):
                self.runtime.prepare(path, pin)
        pointer = {'transaction': 'OTHER', 'approvalSha256': '0' * 64}
        self.f.m.record(self.m.ROOT / 'pending.json', pointer, new=True)
        with self.assertRaisesRegex(ValueError, 'initialization is not untouched'):
            self.runtime.recover('BOOTSTRAP_ONE', pin)
        self.assertEqual(self.f.m.read_record(self.m.ROOT / 'pending.json'), pointer)
        self.assertFalse(self.native_calls)
        self.assert_untouched()

    def test_native_nonzero_stats_or_changed_completed_store_prevent_receipt(self):
        path, pin = self.approval()
        def corrupt(args, **kwargs):
            result = self.native(args, **kwargs)
            first = self.profile['segmentNames'][0]
            if args[3] != first:
                with (self.m.SHM / first).open('r+b') as stream:
                    stream.seek(100)
                    stream.write(b'changed')
            return result
        with mock.patch.object(self.m.subprocess, 'run', corrupt):
            with self.assertRaisesRegex(ValueError, 'created bootstrap store changed'):
                self.runtime.prepare(path, pin)
        self.assertFalse((self.m.ROOT / 'transactions/BOOTSTRAP_ONE/ready.json').exists())
        self.runtime.recover('BOOTSTRAP_ONE', pin)
        self.assert_untouched()

    def test_wrong_pin_reuse_and_other_journal_owner_refuse(self):
        path, pin = self.approval()
        with self.assertRaisesRegex(ValueError, 'digest mismatch'):
            self.runtime.prepare(path, '0' * 64)
        transaction = self.runtime.prepare(path, pin)
        with self.assertRaisesRegex(ValueError, 'already reserved'):
            self.runtime.prepare(path, pin)
        self.f.m.record(self.m.ROOT / 'pending.json', {'transaction': 'OTHER', 'approvalSha256': '0' * 64}, new=True)
        with self.assertRaisesRegex(ValueError, 'another bootstrap'):
            self.runtime.recover('BOOTSTRAP_ONE', pin)
        self.assertEqual(self.f.m.read_record(transaction / 'state.json')['phase'], 'PREPARED_STOPPED')
        self.assert_untouched()

    def test_nonempty_native_stats_refuse_a_ready_receipt(self):
        path, pin = self.approval()
        def nonempty(args, **kwargs):
            result = self.native(args, **kwargs)
            result.stdout = result.stdout.replace(b'pendingWrites=0/', b'pendingWrites=1/')
            return result
        with mock.patch.object(self.m.subprocess, 'run', nonempty):
            with self.assertRaisesRegex(ValueError, 'empty-store creation failed'):
                self.runtime.prepare(path, pin)
        self.assertFalse((self.m.ROOT / 'transactions/BOOTSTRAP_ONE/ready.json').exists())
        self.runtime.recover('BOOTSTRAP_ONE', pin)
        self.assert_untouched()

    def test_trust_drift_before_receipt_is_rejected_and_stopped_recovery_is_possible(self):
        path, pin = self.approval()
        def rotate(args, **kwargs):
            result = self.native(args, **kwargs)
            self.f.write(self.m.TRUST, b'changed external trust')
            return result
        with mock.patch.object(self.m.subprocess, 'run', rotate):
            with self.assertRaisesRegex(ValueError, 'fingerprint drift|state changed before receipt'):
                self.runtime.prepare(path, pin)
        self.runtime.recover('BOOTSTRAP_ONE', pin)
        self.assert_untouched()

    def test_recovery_refuses_unknown_progress_and_mismatched_helpers(self):
        transaction, pin = self.prepare()
        with mock.patch.object(self.m, 'helper_pins', return_value={'changed': True}):
            with self.assertRaisesRegex(ValueError, 'paired helpers'):
                self.runtime.recover('BOOTSTRAP_ONE', pin)
        state = self.f.m.read_record(transaction / 'state.json')
        state['foreign'] = True
        self.f.m.record(transaction / 'state.json', state)
        with self.assertRaisesRegex(ValueError, 'recovery progress'):
            self.runtime.recover('BOOTSTRAP_ONE', pin)
        self.assertTrue((self.m.ROOT / 'current.json').exists())
        self.assert_untouched()

    @unittest.skipUnless(os.environ.get('GATEWAY_TEST_BOOTSTRAP_NATIVE'), 'opt-in native creator integration')
    def test_prepare_with_real_native_constructor_in_private_tmpfs(self):
        rows = [line.split() for line in Path('/proc/self/mountinfo').read_text().splitlines()]
        self.assertTrue(any(r[4] == '/dev/shm' and r[r.index('-') + 1] == 'tmpfs' and
                            r[r.index('-') + 2].startswith('gateway-pointctl-empty-test-') for r in rows))
        self.assertNotEqual(os.readlink('/proc/self/ns/mnt'), os.readlink('/proc/1/ns/mnt'))
        with mock.patch.object(self.m, 'SHM', Path('/dev/shm')):
            self.f.write(self.m.SHM / 'retained_old', b'old store never touched', 0o600)
            with mock.patch.object(self.m.subprocess, 'run', self.external_run), \
                 mock.patch.object(self.m, 'capacity', self.real_capacity):
                transaction, pin = self.prepare()
                ready = self.f.m.read_record(transaction / 'ready.json')
                self.assertEqual(len(ready['segments']), 2)
                for row in ready['segments']:
                    self.assertEqual((self.m.SHM / row['name']).stat().st_size, self.m.guard.POINT_STORE_SIZES[11])
                before = self.m.inventory()
                self.runtime.recover('BOOTSTRAP_ONE', pin)
                self.assertEqual(self.m.inventory(), before)
                self.assert_untouched()


if __name__ == '__main__':
    unittest.main()
