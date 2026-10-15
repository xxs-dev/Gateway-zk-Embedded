"""First activation in private files with real guards/flock and a fake manager.

No real service, native constructor, endpoint health, or physical output runs.
"""
try:
    import fcntl
except ImportError:
    fcntl = None
import copy
import importlib.util
import os
import json
from pathlib import Path
import shutil
import signal
import struct
import subprocess
import time
import types
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@unittest.skipUnless(getattr(os, 'geteuid', lambda: -1)() == 0, 'private Linux root fixture')
class ActivationTest(unittest.TestCase):
    def setUp(self):
        fixture = load('activation_prepare_fixture', REPO / 'tools/runtime_bootstrap_test.py')
        self.p = fixture.BootstrapTest('test_prepare_binds_fresh_empty_receipt_without_activation')
        def image():
            f = self.p.f
            for name in f.m.guard.PRODUCTION_HELPERS:
                f.write(f.image / 'bin' / name, (REPO / 'deploy' / name).read_bytes(), 0o755)
            f.write(f.image / 'bin/SystemMonitor', (f.image / 'bin/pointctl').read_bytes(), 0o755)
            f.write(f.new_units / 'gateway-ota-recover.service', (REPO / 'deploy/gateway-ota-recover.service').read_bytes())
        self.p.prepare_image = image
        self.p.setUp()
        self.addCleanup(self.p.doCleanups)
        self.f, self.prepare = self.p.f, self.p.m
        self.m = load('activation_fixture', REPO / 'deploy/runtime-bootstrap-activate.py')
        m, f, p = self.m, self.f, self.p
        for name, value in (('prepare', self.prepare), ('reprovision', f.m),
                            ('lifecycle', f.m.lifecycle), ('guard', f.m.guard)):
            self.patch(m, name, value)
        self.live = set()
        self.calls = []
        self.fail_health = False
        self.lock_path = f.root / 'activation.lock'
        f.write(self.lock_path, b'', 0o600)
        self.fd = os.open(str(self.lock_path), os.O_RDWR)
        self.addCleanup(os.close, self.fd)
        fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        def owner_proof():
            meta = self.lock_path.stat()
            ticks = Path('/proc').joinpath(str(os.getpid()), 'stat').read_text().rsplit(')', 1)[1].split()[19]
            return dict(pid=str(os.getpid()), startTicks=ticks, bootId=self.prepare.boot_id(),
                        lockDev=meta.st_dev, lockInode=meta.st_ino)
        self.patch(m.lifecycle, 'production_owner_proof', owner_proof)
        def mapped(*parts):
            path = Path(*parts)
            for source, target in ((Path('/opt/modbus-gateway'), f.home),
                (Path('/etc/systemd/system'), f.units), (Path('/etc/modbus-gateway/ota.pub'), self.prepare.TRUST),
                (Path('/dev/shm'), self.prepare.SHM),
                (Path('/proc/sys/kernel/random/boot_id'), self.prepare.BOOT),
                (Path('/run/gateway-offline-runtime.lock'), self.lock_path)):
                if path == source or source in path.parents:
                    return target / path.relative_to(source)
            return path
        self.patch(m.guard, 'Path', mapped)
        self.patch(m.lifecycle, 'Path', mapped)
        owner = load('activation_real_owner', REPO / 'deploy/offline-runtime-upgrade.py')
        owner.Path = mapped
        self.patch(m.lifecycle, 'production_locked_owner', owner.production_locked_owner)
        self.patch(m.guard, 'production_units_present', lambda units: None)
        self.patch(m.guard, 'no_old_mappings', lambda *a, **k: None)
        self.patch(m.lifecycle, 'systemctl', self.systemctl)
        self.patch(m.guard, 'systemctl_value', self.property)
        test = self
        def health(runtime, baseline):
            m.guard.production_verify(f.home, baseline)
            test.assertEqual(test.live, set(baseline['allowedUnits']))
            if test.fail_health:
                raise ValueError('injected first-runtime health failure')
            return dict(format='runtime-health-v2', roles={u: {} for u in test.live},
                        bootId=self.prepare.boot_id(), configSha256=baseline['configSha256'])
        self.patch(m.lifecycle.ProductionRuntime, 'health', health)
        if self._testMethodName.startswith('test_offline_'):
            p.disable_ota()
        self.prepared_tx, self.prepared_pin = p.prepare()
        self.ready_sha = f.m.digest(self.prepared_tx / 'ready.json')
        self.origin = self.prepare.origin()
        self.runtime = m.Activation()
        self.calls.clear()

    def patch(self, *args, **kwargs):
        patch = mock.patch.object(*args, **kwargs)
        result = patch.start()
        self.addCleanup(patch.stop)
        return result

    def property(self, unit, prop):
        f = self.f
        template = unit.split('@')[0] + '@.service' if '@' in unit else unit
        fragment = f.units / template
        if prop == 'LoadState': return 'loaded' if fragment.exists() else 'not-found'
        if prop == 'ActiveState': return 'active' if unit in self.live else 'inactive'
        if prop == 'MainPID': return '43210' if unit in self.live else '0'
        if prop == 'NeedDaemonReload': return 'no'
        if prop == 'FragmentPath': return str(fragment) if fragment.exists() else ''
        if prop == 'DropInPaths':
            effective = {}
            for directory in ('service.d', template + '.d', unit + '.d'):
                effective.update({p.name: p for p in (f.units / directory).glob('*.conf')})
            return ' '.join(str(effective[name]) for name in sorted(effective))
        if prop == 'Environment':
            paths = list((f.units / (unit + '.d')).glob('91-bootstrap-monitor-*.conf'))
            return paths[0].read_text().split('Environment=', 1)[1].strip() if paths else ''
        if prop in ('EnvironmentFiles', 'PassEnvironment', 'UnsetEnvironment', 'PAMName'): return ''
        raise AssertionError((unit, prop))

    def systemctl(self, *args):
        self.calls.append(args)
        if args[0] == 'daemon-reload': return ''
        if args[:2] == ('show', '--property=Id,ActiveState,MainPID'):
            return '\n\n'.join('Id=%s\nActiveState=%s\nMainPID=%s' %
                (unit, self.property(unit, 'ActiveState'), self.property(unit, 'MainPID')) for unit in args[2:])
        if args[0] == 'show': return self.property(args[3], args[1].split('=', 1)[1])
        if args[0] == 'start':
            self.m.guard.production_unit(self.f.home, args[1])
            self.live.add(args[1])
            return ''
        if args[0] == 'is-active':
            self.assertIn(args[-1], self.live)
            return ''
        if args[0] == 'stop':
            self.live.difference_update(args[1:])
            return ''
        raise AssertionError(args)

    def approval(self):
        value = self.m.plan('BOOTSTRAP_ONE', self.ready_sha, 'ACTIVATE_ONE', int(time.time()) + 120,
                            'fixture-first', dict(stableSeconds=2, timeoutSeconds=3, url='http://127.0.0.1:8080/health'))
        path = self.f.root / 'activation-approval.json'
        self.f.m.record(path, value, new=True)
        return path, self.f.m.digest(path)

    def apply(self):
        path, pin = self.approval()
        return self.runtime.apply(path, pin), pin

    def assert_stopped_origin(self):
        self.assertFalse(self.live)
        self.assertEqual(self.prepare.origin(), self.origin)
        self.assertFalse((self.f.home / 'data/runtime-production').exists())
        self.assertFalse((self.m.root() / 'pending.json').exists())
        self.assertEqual((self.prepare.SHM / 'retained_old').read_bytes(), b'old store never touched')

    def test_first_baseline_is_real_generation_one_without_legacy_history(self):
        transaction, pin = self.apply()
        baseline = self.m.guard.production_context(self.f.home)
        self.assertEqual(baseline['generation'], 1)
        self.assertIsNone(baseline['parentSha256'])
        self.assertEqual(set(self.live), set(baseline['allowedUnits']))
        approval = self.f.m.read_record(transaction / 'approval.json', pin)
        self.assertIsNone(approval['previous'])
        self.assertNotIn('legacy', approval)
        self.assertTrue((self.f.journal / 'STOP').exists())
        self.assertFalse((self.f.home / 'data/runtime-upgrade-active.json').exists())
        self.assertEqual(self.f.m.read_record(transaction / 'state.json')['phase'], 'COMPLETED')
        self.assertFalse((self.m.root() / 'pending.json').exists())
        self.runtime.recover('ACTIVATE_ONE', pin)
        self.assertEqual(self.m.guard.production_context(self.f.home), baseline)

    def test_offline_activation_without_key_still_checks_files_and_rejects_ota(self):
        self.test_first_baseline_is_real_generation_one_without_legacy_history()
        baseline = self.m.guard.production_context(self.f.home)
        self.assertFalse(self.prepare.TRUST.exists())
        with self.assertRaisesRegex(ValueError, 'OTA is disabled'):
            self.m.guard.production_require_ota(baseline)
        self.f.write(self.f.home / 'bin/SystemMonitor', b'changed executable', 0o755)
        with self.assertRaises(ValueError):
            self.m.guard.production_context(self.f.home)

    def test_offline_health_failure_preserves_stopped_recovery(self):
        self.test_health_failure_restores_stopped_origin_and_preserves_new_objects()

    def test_offline_newboot_recreates_stores_and_checks_health(self):
        self.test_first_baseline_newboot_creates_proof_then_post_gate_health()

    def test_offline_config_upgrade_and_failure_recovery(self):
        self.test_existing_upgrade_failure_recovers_to_first_baseline()

    def test_health_failure_restores_stopped_origin_and_preserves_new_objects(self):
        before = self.prepare.inventory()
        self.fail_health = True
        with self.assertRaisesRegex(ValueError, 'first-runtime health failure'):
            self.apply()
        self.assert_stopped_origin()
        self.assertEqual(self.prepare.inventory(), before)
        transaction = self.m.root() / 'ACTIVATE_ONE'
        self.assertTrue((transaction / 'retired/data/runtime-production').is_dir())
        pin = self.f.m.digest(transaction / 'approval.json')
        self.runtime.recover('ACTIVATE_ONE', pin)
        self.assert_stopped_origin()

    def test_interrupted_directory_switch_restores_original_inodes(self):
        original = self.runtime.move
        interrupted = []
        def move(source, target):
            original(source, target)
            if target.name == 'config' and target.parent.name == 'old' and not interrupted:
                interrupted.append(True)
                raise OSError('injected config rename interruption')
        with mock.patch.object(self.runtime, 'move', move):
            with self.assertRaisesRegex(OSError, 'rename interruption'):
                self.apply()
        self.assert_stopped_origin()

    def test_gate_publication_interruption_recovers_before_any_service_start(self):
        original = self.m.lifecycle.publish_new
        def publish(path, raw, mode=0o600):
            original(path, raw, mode)
            if path.name.startswith('90-offline-bootstrap-'):
                raise OSError('injected gate publication failure')
        with mock.patch.object(self.m.lifecycle, 'publish_new', publish):
            with self.assertRaisesRegex(OSError, 'gate publication'):
                self.apply()
        self.assertFalse(any(c[0] == 'start' for c in self.calls))
        self.assert_stopped_origin()

    def test_prepared_store_drift_refuses_before_activation_journal(self):
        self.f.write(self.prepare.SHM / 'fresh_data', b'drift', 0o600)
        with self.assertRaises(ValueError):
            self.approval()
        self.assertFalse(self.m.root().exists())
        self.assertFalse(self.live)

    def test_wrong_activation_hash_and_expiry_refuse_without_mutation(self):
        path, pin = self.approval()
        with self.assertRaisesRegex(ValueError, 'digest mismatch'):
            self.runtime.apply(path, '0' * 64)
        with mock.patch.object(self.m.time, 'time', return_value=time.time() + 1000):
            with self.assertRaises(ValueError):
                self.runtime.apply(path, pin)
        self.assertFalse(self.m.root().exists())
        self.assertFalse(self.live)

    def test_healthy_pointer_wins_over_interrupted_pending_cleanup(self):
        def interrupted(runtime, state):
            raise OSError('injected committed cleanup failure')
        path, pin = self.approval()
        with mock.patch.object(self.m.lifecycle.ProductionRuntime, 'clear_pending', interrupted):
            with self.assertRaisesRegex(OSError, 'committed cleanup'):
                self.runtime.apply(path, pin)
        baseline = self.m.guard.production_context(self.f.home)
        self.runtime.recover('ACTIVATE_ONE', pin)
        self.assertEqual(self.m.guard.production_context(self.f.home), baseline)
        self.assertTrue(self.live)
        self.assertFalse((self.f.home / 'data/runtime-production/pending.json').exists())

    def test_lost_lock_refuses_provisional_service_and_recovers_stopped(self):
        original = self.systemctl
        lost = []
        def manager(*args):
            if args[0] == 'start' and not lost:
                fcntl.flock(self.fd, fcntl.LOCK_UN)
                lost.append(True)
            return original(*args)
        with mock.patch.object(self.m.lifecycle, 'systemctl', manager):
            with self.assertRaisesRegex(ValueError, 'offline lock'):
                self.apply()
        self.assertFalse(self.live)
        fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        transaction = self.m.root() / 'ACTIVATE_ONE'
        self.runtime.recover('ACTIVATE_ONE', self.f.m.digest(transaction / 'approval.json'))
        self.assert_stopped_origin()

    def kill_activation(self, boundary):
        path, pin = self.approval()
        fcntl.flock(self.fd, fcntl.LOCK_UN)
        pid = os.fork()
        if pid == 0:
            try:
                os.close(self.fd)
                child_fd = os.open(str(self.lock_path), os.O_RDWR)
                fcntl.flock(child_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                die = lambda: os.kill(os.getpid(), signal.SIGKILL)
                original_move = self.runtime.move
                original_unlink = Path.unlink
                def move(source, destination):
                    original_move(source, destination)
                    if boundary == 'old-config' and destination.name == 'config' and destination.parent.name == 'old':
                        die()
                    if boundary == 'recover-config' and destination.name == 'config' and destination.parent.name == 'retired':
                        die()
                def unlink(path, *args, **kwargs):
                    original_unlink(path, *args, **kwargs)
                    if boundary == 'first-fence' and path.name == self.f.m.FENCE_NAME:
                        die()
                def health(runtime, baseline):
                    self.f.write(self.f.home / 'data/retained-first-start.json', b'{"retain":true}\n')
                    with (self.prepare.SHM / 'fresh_data').open('r+b') as stream:
                        stream.seek(100)
                        stream.write(b'keep live bytes')
                    die()
                self.runtime.move = move
                Path.unlink = unlink
                if boundary == 'started':
                    self.m.lifecycle.ProductionRuntime.health = health
                if boundary == 'promoted':
                    self.m.lifecycle.ProductionRuntime.clear_pending = lambda *a: die()
                if boundary == 'recover-config':
                    self.fail_health = True
                self.runtime.apply(path, pin)
                os._exit(97)
            except BaseException:
                import traceback
                traceback.print_exc()
                os._exit(98)
        deadline = time.monotonic() + 45
        while True:
            done, status = os.waitpid(pid, os.WNOHANG)
            if done:
                break
            if time.monotonic() >= deadline:
                os.kill(pid, signal.SIGKILL)
                os.waitpid(pid, 0)
                self.fail('activation child timed out before crash boundary')
            time.sleep(0.02)
        fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        self.assertTrue(os.WIFSIGNALED(status) and os.WTERMSIG(status) == signal.SIGKILL, status)
        return pin

    def test_sigkill_after_original_config_rename_recovers(self):
        pin = self.kill_activation('old-config')
        self.runtime.recover('ACTIVATE_ONE', pin)
        self.assert_stopped_origin()

    def test_sigkill_during_stop_fence_removal_recovers(self):
        pin = self.kill_activation('first-fence')
        self.runtime.recover('ACTIVATE_ONE', pin)
        self.assert_stopped_origin()

    def test_sigkill_during_recovery_can_resume_original_directory_restore(self):
        pin = self.kill_activation('recover-config')
        self.runtime.recover('ACTIVATE_ONE', pin)
        self.assert_stopped_origin()
        self.runtime.recover('ACTIVATE_ONE', pin)
        self.assert_stopped_origin()

    def test_staging_failure_before_switch_requires_explicit_stopped_recovery(self):
        path, pin = self.approval()
        with mock.patch.object(self.m.reprovision, 'copy_tree', side_effect=OSError('injected staging failure')):
            with self.assertRaisesRegex(OSError, 'staging failure'):
                self.runtime.apply(path, pin)
        self.assertTrue((self.m.root() / 'pending.json').exists())
        self.assertEqual(self.prepare.origin(), self.origin)
        self.runtime.recover('ACTIVATE_ONE', pin)
        self.assert_stopped_origin()

    def test_sigkill_after_start_requires_paired_recovery_and_preserves_runtime_data(self):
        pin = self.kill_activation('started')
        with self.assertRaises((ValueError, FileNotFoundError)):
            self.m.guard.production_unit(self.f.home, 'gateway-services.service')
        fcntl.flock(self.fd, fcntl.LOCK_UN)
        try:
            with self.assertRaisesRegex(ValueError, 'run paired stopped recovery'):
                self.m.lifecycle.production_boot(types.SimpleNamespace(home=self.f.home))
        finally:
            fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        self.runtime.recover('ACTIVATE_ONE', pin)
        self.assert_stopped_origin()
        retired = self.m.root() / 'ACTIVATE_ONE/retired/data/retained-first-start.json'
        self.assertEqual(retired.read_bytes(), b'{"retain":true}\n')
        with (self.prepare.SHM / 'fresh_data').open('rb') as stream:
            stream.seek(100)
            self.assertEqual(stream.read(15), b'keep live bytes')

    def test_sigkill_after_healthy_pointer_finishes_without_rollback(self):
        pin = self.kill_activation('promoted')
        baseline = self.m.guard.production_context(self.f.home)
        self.runtime.recover('ACTIVATE_ONE', pin)
        self.assertEqual(self.m.guard.production_context(self.f.home), baseline)
        self.assertFalse((self.f.home / 'data/runtime-production/pending.json').exists())

    def next_release(self, name):
        runtime = self.m.lifecycle.ProductionRuntime(self.f.home)
        previous = self.m.guard.production_context(self.f.home)
        pointer = self.m.guard.production_json(runtime.root / 'current.json')
        payload = self.f.root / name
        payload.mkdir(mode=0o700)
        shutil.copytree(self.f.home / 'bin', payload / 'bin')
        shutil.copytree(self.f.home / 'config/runtime', payload / 'config/runtime')
        relative = 'apps/monitor-service.json'
        changed = payload / 'config/runtime' / relative
        value = json.loads(changed.read_text())
        value['systemMonitor']['reportIntervalMs'] = 5000
        self.f.write(changed, json.dumps(value).encode())
        candidate = copy.deepcopy(previous)
        resolved = self.prepare.policy.resolve(payload / 'config/runtime', self.f.units,
            previous['roleAuthority']['resolved']['protected'], self.prepare.TRUST,
            ota_enabled=self.m.guard.production_trust_policy(previous['trust'], allow_disabled=True))
        candidate.update(baselineId=name, generation=2, parentSha256=pointer['sha256'], release=name,
                         configSha256=resolved['configSha256'])
        candidate['roleAuthority']['resolved'] = resolved
        approval = dict(format='runtime-provisioning-v2', transaction=name, operation='install',
            expiresAtUnix=int(time.time()) + 120, gatewayHome=str(self.f.home), machineCode=previous['machineCode'],
            parentSha256=pointer['sha256'], previous=previous, candidate=candidate, legacy=None,
            configWriteSet=[dict(path=relative, before=self.m.guard.production_pin(self.f.home / 'config/runtime' / relative),
                                 after=self.m.guard.production_pin(changed))])
        path = self.f.root / (name + '.json')
        self.f.m.record(path, approval, new=True)
        return runtime, types.SimpleNamespace(approval=path, approval_sha256=self.f.m.digest(path), payload=payload), previous

    def test_first_baseline_supports_existing_config_upgrade(self):
        self.apply()
        runtime, args, previous = self.next_release('CONFIG_NEXT')
        runtime.install(args)
        current = self.m.guard.production_context(self.f.home)
        self.assertEqual(current['generation'], 2)
        self.assertNotEqual(current['configSha256'], previous['configSha256'])
        self.assertEqual(current['historical'], previous['historical'])

    def test_existing_upgrade_failure_recovers_to_first_baseline(self):
        self.apply()
        runtime, args, previous = self.next_release('CONFIG_FAIL')
        self.fail_health = True
        with self.assertRaisesRegex(ValueError, 'first-runtime health failure'):
            runtime.install(args)
        self.fail_health = False
        runtime.recover(runtime.root / 'transactions/CONFIG_FAIL', args.approval_sha256)
        self.assertEqual(self.m.guard.production_context(self.f.home), previous)
        self.assertEqual(self.live, set(previous['allowedUnits']))

    def test_first_baseline_stages_paired_reboot_entry_without_previous_baseline(self):
        transaction, pin = self.apply()
        runtime = self.m.lifecycle.ProductionRuntime(self.f.home)
        entry, pins = self.m.lifecycle.production_boot_stage_entry(runtime,
            dict(kind='current', transaction=transaction.name, approvalSha256=pin))
        self.assertEqual(len(pins), 3)
        self.assertTrue(all(self.f.m.digest(entry / name) == sha for name, sha in pins.items()))

    def new_boot(self):
        m, f = self.m, self.f
        self.live.clear()
        for name in self.p.profile['segmentNames']:
            (self.prepare.SHM / name).unlink()
        f.write(self.prepare.BOOT, b'ACTIVATION_BOOT_2\n')
        mapped = m.lifecycle.Path
        proc = f.root / 'boot-proc'
        local_pid = proc / str(os.getpid())
        mountinfo = local_pid / 'mountinfo'
        shm = self.prepare.SHM
        device = shm.stat().st_dev
        f.write(mountinfo, ('1 0 %d:%d / %s rw - tmpfs shm rw\n' %
            (os.major(device), os.minor(device), shm)).encode())
        f.write(local_pid / 'maps', b'')
        (local_pid / 'fd').mkdir(mode=0o700)
        def paths(*parts):
            path = Path(*parts)
            pid = Path('/proc') / str(os.getpid())
            if path == Path('/proc'): return proc
            if path == pid or pid in path.parents: return local_pid / path.relative_to(pid)
            return mapped(*parts)
        self.patch(m.lifecycle, 'Path', paths)
        original_open = os.open
        def opened(path, flags, *args, **kwargs):
            fd = original_open(path, flags, *args, **kwargs)
            if str(path) == str(shm) and flags & os.O_PATH:
                f.write(local_pid / 'fdinfo' / str(fd), b'mnt_id:\t1\n')
            return fd
        self.patch(m.lifecycle.os, 'open', opened)
        self.boot_native_calls = []
        self.dispatches = []
        def external(command, **kwargs):
            if command[0] == '/usr/bin/systemd-run':
                self.assertIn('--no-block', command)
                self.assertIn('--property=After=gateway-ota-recover.service', command)
                self.dispatches.append(command)
                return subprocess.CompletedProcess(command, 0, b'', b'')
            self.assertEqual(command[:3], [str(f.home / 'bin/pointctl'), 'stats', '--shm'])
            self.assertIn(command[3], self.p.profile['segmentNames'])
            self.assertEqual(kwargs['env'], {'PATH': '/usr/bin:/bin', 'LC_ALL': 'C'})
            self.assertTrue(m.lifecycle.production_locked_owner({'ownerProof': m.lifecycle.production_owner_proof()}))
            self.boot_native_calls.append(command)
            with (shm / command[3]).open('xb') as stream:
                stream.write(struct.pack('<II', 0x4d505354, 11))
                stream.truncate(m.guard.POINT_STORE_SIZES[11])
            (shm / command[3]).chmod(0o600)
            output = ('shm=' + command[3] + ' latest=0/1 latestCapacity=1 pendingWrites=0/1 pendingCapacity=1 '
                'persistent=0/1 persistentCapacity=1 pointUpdates=0/1 writeSeq=0 persistentSeq=0 pointUpdateSeq=0\n')
            return subprocess.CompletedProcess(command, 0, output.encode(), b'')
        self.patch(m.lifecycle.subprocess, 'run', external)

    def boot(self):
        fcntl.flock(self.fd, fcntl.LOCK_UN)
        try:
            self.m.lifecycle.production_boot(types.SimpleNamespace(home=self.f.home))
        finally:
            fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def boot_finish(self):
        runtime = self.m.lifecycle.ProductionRuntime(self.f.home)
        work = self.m.guard.production_json(self.m.lifecycle.production_boot_work(runtime))
        fcntl.flock(self.fd, fcntl.LOCK_UN)
        try:
            with mock.patch.object(self.m.lifecycle, '__file__', str(Path(work['entryPath']) / 'offline-runtime-upgrade.py')):
                self.m.lifecycle.production_boot_finish(types.SimpleNamespace(home=self.f.home))
        finally:
            fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def assert_boot_completed(self):
        runtime = self.m.lifecycle.ProductionRuntime(self.f.home)
        work_path = self.m.lifecycle.production_boot_work(runtime)
        work = self.m.guard.production_json(work_path)
        self.assertEqual(work['phase'], 'FINISHED')
        self.assertEqual(work['kind'], 'current')
        self.assertEqual(work['transaction'], 'ACTIVATE_ONE')
        health = self.m.lifecycle.checked_json(work_path.parent / work['healthRecord'], work['healthSha256'])
        self.assertEqual(health['bootId'], 'ACTIVATION_BOOT_2')
        self.assertEqual(health['health']['bootId'], 'ACTIVATION_BOOT_2')
        self.assertEqual(self.live, set(self.p.profile['allowedUnits']))
        self.assertEqual(len(self.boot_native_calls), len(self.p.profile['segmentNames']))

    def test_first_baseline_newboot_creates_proof_then_post_gate_health(self):
        self.apply()
        self.new_boot()
        self.boot()
        self.assertFalse(self.live)
        self.assertEqual(len(self.dispatches), 1)
        self.boot_finish()
        self.assert_boot_completed()
        self.boot()
        self.assert_boot_completed()

    def test_promoted_sigkill_newboot_reconciles_without_legacy_rollback(self):
        self.kill_activation('promoted')
        self.new_boot()
        self.boot()
        self.assertFalse(self.live)
        self.assertFalse((self.f.home / 'data/runtime-production/pending.json').exists())
        self.boot_finish()
        self.assert_boot_completed()

    def test_first_baseline_newboot_failed_health_can_retry_without_recreation(self):
        self.apply()
        self.new_boot()
        self.boot()
        self.fail_health = True
        with self.assertRaisesRegex(ValueError, 'first-runtime health failure'):
            self.boot_finish()
        self.assertFalse(self.live)
        self.fail_health = False
        self.boot_finish()
        self.assert_boot_completed()

    def test_old_activation_recovery_cannot_replace_a_newer_production_baseline(self):
        transaction, pin = self.apply()
        runtime, args, previous = self.next_release('CONFIG_LATER')
        runtime.install(args)
        current = self.m.guard.production_context(self.f.home)
        with self.assertRaisesRegex(ValueError, 'another production baseline'):
            self.runtime.recover(transaction.name, pin)
        self.assertEqual(self.m.guard.production_context(self.f.home), current)


if __name__ == '__main__':
    unittest.main()
