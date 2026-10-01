"""Linux private boot chain: real files/flock, substituted native ABI constructor.

The substitute writes ONLY mapped private files. It does not validate the actual
pointctl ELF, robust mutex initialization, target hardware or OS/systemd reboot.
"""
import os
import contextlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import types
import unittest
from unittest import mock


@unittest.skipUnless(os.name == 'posix' and hasattr(os, 'geteuid') and os.geteuid() == 0,
                     'requires Linux root private fixture and actual kernel flock')
class NativeBootTest(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.spec_from_file_location('nativeboot_baseline_fixture', Path(__file__).with_name('production_runtime_baseline_test.py'))
        baseline = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(baseline)
        self.f = baseline.ProductionRuntimeTest('test_production_entry_exists_without_changing_legacy_actions')
        self.f.setUp()
        self.addCleanup(self.f.doCleanups)
        f = self.f
        for root in (f.home, f.payload): f.write(root / 'bin/pointctl', f.elf(b'native-creator-substitute'), 0o755)
        f.previous['files'] = f.g.production_files(f.home)
        f.candidate['files'] = f.g.production_files(f.payload)
        adoption_rollback = self._testMethodName == 'test_no_handoff_runtime_newboot_adoption_rollback'
        if adoption_rollback:
            args = f.args()
            f.fail_health = True
            with self.assertRaisesRegex(ValueError, 'endpoint health'):
                with f.m.production_lock(): f.m.production_adopt(args)
            f.fail_health = False
            self.adopted = (f.runtime.root / 'transactions/ADOPT_01', args)
        else:
            self.adopted = f.adopt()
        if self._testMethodName == 'test_no_handoff_runtime_newboot_promoted':
            args = f.next_release()
            def crash(state): raise OSError('injected promoted runtime pending clear')
            with mock.patch.object(f.m.ProductionRuntime, 'clear_pending', lambda runtime, state: crash(state)):
                with self.assertRaisesRegex(OSError, 'runtime pending clear'):
                    with f.m.production_lock(): f.m.production_install(args)
        self.owner = None
        if self._testMethodName.startswith('test_original_job_'):
            self.make_interrupted_job(self._testMethodName.rsplit('_', 1)[1])
        if self._testMethodName == 'test_original_job_newboot_pending':
            record = self.owner.read()
            self.source = f.runtime.root / 'transactions' / record['productionTransaction'] / 'previous.json'
            self.baseline = f.g.production_json(self.source)
            with self.assertRaisesRegex(ValueError, 'unresolved production install'):
                f.g.production_context(f.home)
        elif adoption_rollback:
            self.baseline = f.previous
            self.source = self.adopted[0] / 'previous.json'
        else:
            self.baseline = f.g.production_context(f.home)
            pointer = f.g.production_json(f.runtime.root / 'current.json')
            self.source = f.runtime.root / 'baselines' / (pointer['baselineId'] + '.json')
        self.source_bytes = self.source.read_bytes()
        f.live.clear()
        shutil.rmtree(f.path('/proc/98765'))
        for name in ('retired_v10', 'live_v11'): f.path('/dev/shm', name).unlink()
        shutil.rmtree(f.path('/run')); f.path('/run').mkdir(mode=0o755)
        f.write(f.path('/proc/sys/kernel/random/boot_id'), 'NATIVE_BOOT_2\n')
        f.write(f.path('/proc/self/mountinfo'), '1 0 0:1 / /dev/shm rw - tmpfs shm rw\n')
        self.native_calls = []
        self.prepare_calls = []
        self.failure = None
        self.patch = mock.patch.object(f.m.subprocess, 'run', self.external)
        self.patch.start(); self.addCleanup(self.patch.stop)
        self.original_prepare = f.m.ProductionRuntime.prepare_boot
        def prepare(runtime, *args, **kwargs):
            self.prepare_calls.append(args)
            f.write(f.path('/proc/locks'), Path('/proc/locks').read_bytes())
            return self.original_prepare(runtime, *args, **kwargs)
        self.prepare_patch = mock.patch.object(f.m.ProductionRuntime, 'prepare_boot', prepare)
        self.prepare_patch.start(); self.addCleanup(self.prepare_patch.stop)
        original_spec = f.m.importlib.util.spec_from_file_location
        def local_spec(name, path, *args, **kwargs):
            spec = original_spec(name, path, *args, **kwargs)
            if name == 'production_ota_health':
                delegate = spec.loader
                class Loader:
                    def create_module(self, spec): return None
                    def exec_module(self, module):
                        delegate.exec_module(module)
                        module.RuntimeHandoff.verify_owner = lambda *a, **k: dict(ActiveState='inactive', MainPID='0', Job='')
                        module.RuntimeHandoff.no_unit_tasks = lambda *a: None
                        module.RuntimeHandoff.production_entry = lambda *a: f.m
                spec.loader = Loader()
            return spec
        f.patch(f.m.importlib.util, 'spec_from_file_location', local_spec)

    def make_interrupted_job(self, window):
        f = self.f
        with mock.patch.object(f, 'adopt', lambda: self.adopted): self.owner = f.owner_fixture()
        old_monitor = (f.home / 'bin/SystemMonitor').read_bytes()
        def crash(*args, **kwargs): raise OSError('injected original job window')
        native_external = f.external
        def endpoint(command, **kwargs):
            result = native_external(command, **kwargs)
            if command[0] == '/usr/bin/curl' and (f.home / 'bin/SystemMonitor').read_bytes() != old_monitor:
                return subprocess.CompletedProcess(command, 1, b'', b'candidate-only failure')
            return result
        with contextlib.ExitStack() as stack:
            stack.enter_context(mock.patch.object(f.m, 'production_ota_finish', crash))
            if window == 'preintent': stack.enter_context(mock.patch.object(f.m.ProductionRuntime, 'install', crash))
            if window == 'pending':
                f.fail_health = True
                stack.enter_context(mock.patch.object(f.m.ProductionRuntime, 'recover', crash))
            if window == 'rollback': stack.enter_context(mock.patch.object(f.m.subprocess, 'run', endpoint))
            with self.assertRaisesRegex(OSError, 'original job window'):
                with self.owner.lease(): f.m.production_ota_owner(self.owner, self.owner.read())
        f.fail_health = False
        self.assertEqual(self.owner.read()['phase'], 'OWNER_INSTALLING')

    def external(self, command, **kwargs):
        f = self.f
        if command[1:] != ['stats', '--shm', 'live_v11']: return f.external(command, **kwargs)
        creator = Path(command[0])
        backup = False
        try:
            creator.relative_to(f.runtime.root / 'transactions')
            backup = True
        except ValueError:
            pass
        self.assertTrue(creator == f.home / 'bin/pointctl' or backup)
        self.assertEqual(f.g.production_pin(creator), self.baseline['files']['bin/pointctl'])
        self.assertEqual(kwargs['env'], {'PATH': '/usr/bin:/bin', 'LC_ALL': 'C'})
        self.assertTrue(f.m.production_locked_owner({'ownerProof': f.m.production_owner_proof()}))
        intent = f.g.production_json(f.runtime.root / 'boots/NATIVE_BOOT_2-intent.json')
        self.assertEqual(intent['inProgress'], 'live_v11')
        self.native_calls.append(command)
        path = f.path('/dev/shm/live_v11')
        existed = path.exists()
        inode = path.stat().st_ino if existed else None
        f.write(path, struct.pack('<II', 0x4d505354, 11), 0o600)
        with path.open('r+b') as stream: stream.truncate(f.g.POINT_STORE_SIZES[11])
        if existed: self.assertEqual(path.stat().st_ino, inode)
        if self.failure:
            kind, self.failure = self.failure, None
            if kind == 'creator-crash': raise OSError('injected creator exit after ABI file creation')
            return subprocess.CompletedProcess(command, 0, b'unproved stats', b'')
        output = ('shm=live_v11 latest=0/1 latestCapacity=1 pendingWrites=0/1 pendingCapacity=1 '
                  'persistent=0/1 persistentCapacity=1 pointUpdates=0/1 writeSeq=0 persistentSeq=0 pointUpdateSeq=0\n')
        return subprocess.CompletedProcess(command, 0, output.encode(), b'')

    def boot(self):
        f = self.f
        with mock.patch.object(f.m.sys, 'argv', ['offline-runtime-upgrade.py', 'production-boot', str(f.home)]):
            f.m.main()

    def finish(self):
        f = self.f
        work_path = f.m.production_boot_work(f.runtime)
        if work_path.exists():
            work = f.g.production_json(work_path)
            f.m.__file__ = str(Path(work['entryPath']) / 'offline-runtime-upgrade.py')
        with mock.patch.object(f.m.sys, 'argv', ['offline-runtime-upgrade.py', 'production-boot-finish', str(f.home)]):
            f.m.main()

    def assert_unadmitted(self):
        self.assertEqual(self.native_calls, [])
        self.assertFalse((self.f.runtime.root / 'boots').exists())
        self.assertFalse(self.f.path('/dev/shm/live_v11').exists())
        self.assertFalse(self.f.path('/dev/shm/retired_v10').exists())
        self.assertEqual(self.source.read_bytes(), self.source_bytes)

    def test_cleared_run_real_entry_creates_locked_boot_proof_before_readers(self):
        self.boot()
        f = self.f
        self.assertTrue(f.path('/run/gateway-offline-runtime.lock').exists())
        proof = f.g.production_json(f.runtime.root / 'boots/NATIVE_BOOT_2.json')
        self.assertEqual(proof['sourceSha256'], f.m.digest(self.source))
        self.assertEqual(proof['creatorPin'], self.baseline['files']['bin/pointctl'])
        self.assertEqual(proof['segments'][0]['ino'], f.path('/dev/shm/live_v11').stat().st_ino)
        self.assertNotEqual(proof['bootId'], self.baseline['bootId'])
        self.assertEqual(f.g.production_context(f.home), self.baseline)
        self.assertEqual(f.g.production_unit(f.home, 'gateway-services.service'), self.baseline)
        self.assertFalse(f.path('/dev/shm/retired_v10').exists())
        self.assertFalse(f.live, 'oneshot must not synchronously start its dependent readers')
        self.assertEqual(self.source.read_bytes(), self.source_bytes)
        self.finish()
        self.assertEqual(f.live, set(f.allowed))
        self.boot()
        self.assertEqual(len(self.native_calls), 1, 'same boot proof prevents a second constructor invocation')

    def test_memory_insufficient_refuses_before_intent_or_native_call(self):
        f = self.f
        f.write(f.path('/proc/meminfo'), 'MemAvailable: 0 kB\n')
        with self.assertRaisesRegex(ValueError, '^insufficient MemAvailable for activation$'): self.boot()
        self.assert_unadmitted()
        self.assertFalse(f.live, 'capacity refusal must not start readers')

    def test_tmpfs_insufficient_refuses_before_intent_or_native_call(self):
        f = self.f
        f.write(f.path('/proc/meminfo'), 'MemAvailable: 2097152 kB\n')
        with mock.patch.object(f.m.os, 'statvfs', lambda p: types.SimpleNamespace(f_frsize=4096, f_bavail=0)):
            with self.assertRaisesRegex(ValueError, '^insufficient native boot memory/tmpfs capacity$'): self.boot()
        self.assert_unadmitted()
        self.assertFalse(f.live, 'capacity refusal must not start readers')

    def test_wrong_creator_pin_refuses_before_intent(self):
        self.f.write(self.f.home / 'bin/pointctl', self.f.elf(b'wrong-creator'), 0o755)
        with self.assertRaises(ValueError): self.boot()
        self.assert_unadmitted()

    def test_retired_recreated_or_unowned_target_refuses(self):
        f = self.f
        f.write(f.path('/dev/shm/retired_v10'), struct.pack('<II', 0x4d505354, 10))
        with self.assertRaisesRegex(ValueError, 'retired'): self.boot()
        self.assertFalse((f.runtime.root / 'boots').exists())
        f.path('/dev/shm/retired_v10').unlink()
        f.write(f.path('/dev/shm/live_v11'), b'', 0o600)
        with self.assertRaisesRegex(ValueError, 'unowned'): self.boot()
        self.assertEqual(self.native_calls, [])

    def test_interrupted_creator_reenters_original_intent_and_inode(self):
        self.failure = 'creator-crash'
        with self.assertRaisesRegex(OSError, 'creator exit'): self.boot()
        f = self.f
        inode = f.path('/dev/shm/live_v11').stat().st_ino
        self.assertFalse((f.runtime.root / 'boots/NATIVE_BOOT_2.json').exists())
        self.boot()
        self.assertEqual(f.path('/dev/shm/live_v11').stat().st_ino, inode)
        self.assertEqual(len(self.native_calls), 2)
        self.assertFalse(f.path('/dev/shm/retired_v10').exists())

    def test_wrong_interrupted_header_is_not_truncated_or_replaced(self):
        self.failure = 'creator-crash'
        with self.assertRaises(OSError): self.boot()
        path = self.f.path('/dev/shm/live_v11')
        with path.open('r+b') as stream: stream.write(struct.pack('<II', 0x4d505354, 10))
        inode = path.stat().st_ino
        with self.assertRaisesRegex(ValueError, 'ABI/header'): self.boot()
        self.assertEqual(path.stat().st_ino, inode)
        self.assertEqual(len(self.native_calls), 1)

    def test_invalid_stats_never_publishes_ready_or_admits_reader(self):
        self.failure = 'stats'
        with self.assertRaisesRegex(ValueError, 'queues'): self.boot()
        f = self.f
        self.assertFalse((f.runtime.root / 'boots/NATIVE_BOOT_2.json').exists())
        with self.assertRaises((ValueError, FileNotFoundError)): f.g.production_unit(f.home, 'gateway-services.service')

    def test_not_tmpfs_or_held_lock_refuses_before_native_call(self):
        f = self.f
        f.write(f.path('/proc/self/mountinfo'), '1 0 0:1 / /dev/shm rw - ext4 disk rw\n')
        with self.assertRaisesRegex(ValueError, 'tmpfs'): self.boot()
        self.assert_unadmitted()
        f.write(f.path('/proc/self/mountinfo'), '1 0 0:1 / /dev/shm rw - tmpfs shm rw\n')
        with f.m.production_lock():
            with self.assertRaises(BlockingIOError): self.boot()
        self.assert_unadmitted()

    def original_job_newboot(self, outcome):
        f, owner = self.f, self.owner
        original_intent = owner.intent_path.read_bytes()
        record = owner.read()
        tx = f.runtime.root / 'transactions' / record['productionTransaction']
        if self._testMethodName.endswith('preintent'): self.assertFalse(tx.exists())
        self.boot()
        self.assertFalse(f.live, 'recovery oneshot must never wait for its own After readers')
        self.assertEqual(owner.read()['phase'], 'OWNER_INSTALLING')
        work = f.g.production_json(f.m.production_boot_work(f.runtime))
        self.assertTrue(self.prepare_calls, 'must reach the real native preparation function, not fail in setup')
        self.assertEqual(work['kind'], 'job')
        self.assertEqual(work['jobId'], owner.job)
        self.assertEqual(work['approvalSha256'], record['productionApprovalSha256'])
        if self._testMethodName.endswith('preintent'): self.assertFalse(tx.exists(), 'no synthetic transaction on pre-intent recovery')
        self.finish()
        terminal = owner.read()
        self.assertEqual(terminal['phase'], 'OWNER_FINISHED')
        self.assertEqual(terminal['completionProof']['status'], outcome)
        self.assertEqual(owner.intent_path.read_bytes(), original_intent)
        self.assertEqual(f.live, set(f.allowed))
        self.assertFalse(f.path('/dev/shm/retired_v10').exists())
        self.assertEqual(len(self.native_calls), 1)
        work_path = f.m.production_boot_work(f.runtime)
        finished = f.g.production_json(work_path)
        proof = f.g.production_json(work_path.parent / finished['healthRecord'])
        self.assertEqual(proof['health']['bootId'], 'NATIVE_BOOT_2')
        self.assertTrue(proof['health']['readers'], 'real private health call, not prior-boot health.json')
        self.assertEqual(f.g.production_json(work_path)['phase'], 'FINISHED')

    def test_original_job_newboot_pending(self): self.original_job_newboot('failed')
    def test_original_job_newboot_promoted(self): self.original_job_newboot('healthy')
    def test_original_job_newboot_rollback(self): self.original_job_newboot('failed')
    def test_original_job_newboot_preintent(self): self.original_job_newboot('failed')

    def no_handoff_runtime_newboot(self, expected_phase):
        f = self.f
        pending = f.g.production_json(f.runtime.root / 'pending.json')
        transaction = f.runtime.root / 'transactions' / pending['transaction']
        approval_bytes = (transaction / 'approval.json').read_bytes()
        self.boot()
        self.assertFalse(f.live, 'gate must leave readers stopped until post-gate worker')
        work_path = f.m.production_boot_work(f.runtime)
        work = f.g.production_json(work_path)
        self.assertEqual((work['kind'], work['jobId']), ('runtime', None))
        self.assertEqual(work['transaction'], pending['transaction'])
        entry = Path(work['entryPath'])
        self.assertEqual({p.name for p in entry.iterdir()}, set(work['entryPins']))
        self.assertTrue(all(f.m.digest(entry / name) == pin for name, pin in work['entryPins'].items()))
        if expected_phase == 'RECOVERED':
            self.assertNotEqual(f.m.digest(f.home / 'bin/offline-runtime-upgrade.py'), work['entryPins']['offline-runtime-upgrade.py'])
            self.assertFalse((f.runtime.root / 'current.json').exists())
        self.finish()
        done = f.g.production_json(work_path)
        self.assertEqual((done['phase'], done['startAttempt'], done['healthAttempt']), ('FINISHED', 1, 1))
        proof = f.m.checked_json(work_path.parent / done['healthRecord'], done['healthSha256'])
        self.assertEqual(proof['health']['bootId'], 'NATIVE_BOOT_2')
        self.assertTrue(proof['health']['readers'])
        self.assertEqual(f.live, set(f.allowed))
        self.assertEqual(f.g.production_json(transaction / 'state.json')['phase'], expected_phase)
        self.assertFalse((f.runtime.root / 'pending.json').exists())
        self.assertEqual((transaction / 'approval.json').read_bytes(), approval_bytes)
        self.assertFalse(f.path('/dev/shm/retired_v10').exists())
        self.assertEqual(len(self.native_calls), 1)
        self.finish()  # official completed worker reentry must not need a new pointer/job
        self.assertEqual(f.g.production_json(work_path)['startAttempt'], 1)

    def test_no_handoff_runtime_newboot_promoted(self): self.no_handoff_runtime_newboot('COMPLETED')
    def test_no_handoff_runtime_newboot_adoption_rollback(self): self.no_handoff_runtime_newboot('RECOVERED')

    def test_uncertain_dispatch_original_finish_recovers_without_gate_resend(self):
        for window in ('timeout', 'nonzero', 'accepted-before-save'):
            with self.subTest(window=window):
                child = NativeBootTest('test_no_handoff_runtime_newboot_promoted')
                try:
                    child.setUp()
                    f = child.f
                    original_run, original_save = child.external, f.m.save
                    injected, dispatched = [], []
                    def run(command, **kwargs):
                        result = original_run(command, **kwargs)
                        if command[0] == '/usr/bin/systemd-run':
                            dispatched.append(command)
                            if window == 'timeout':
                                injected.append(window)
                                raise subprocess.TimeoutExpired(command, 15)
                            if window == 'nonzero':
                                injected.append(window)
                                return subprocess.CompletedProcess(command, 1, b'', b'unknown result')
                        return result
                    def save(path, value):
                        if window == 'accepted-before-save' and value.get('phase') == 'DISPATCHED':
                            injected.append(window)
                            raise OSError('injected accepted-before-save')
                        return original_save(path, value)
                    with mock.patch.object(f.m.subprocess, 'run', run), mock.patch.object(f.m, 'save', save):
                        if window == 'timeout':
                            with self.assertRaises(subprocess.TimeoutExpired): child.boot()
                        elif window == 'nonzero':
                            with self.assertRaisesRegex(ValueError, 'boot continuation dispatch uncertain/failed'): child.boot()
                        else:
                            with self.assertRaisesRegex(OSError, 'injected accepted-before-save'): child.boot()
                    self.assertEqual(injected, [window], 'must hit the requested injection, not an earlier guard')
                    self.assertEqual(len(dispatched), 1)
                    work_path = f.m.production_boot_work(f.runtime)
                    work_bytes = work_path.read_bytes()
                    work = f.g.production_json(work_path)
                    self.assertEqual(work['phase'], 'DISPATCH_INTENT')
                    self.assertEqual(work['kind'], 'runtime')
                    child.boot()
                    self.assertEqual(work_path.read_bytes(), work_bytes)
                    self.assertFalse(f.live)
                    start_count = sum(c[0] == 'start' for c in f.calls)
                    for field, bad in (('phase', 'UNAUTHORIZED'), ('approvalSha256', '0' * 64)):
                        invalid = dict(work); invalid[field] = bad
                        f.write(work_path, json.dumps(invalid), 0o600)
                        with self.assertRaises(ValueError): child.finish()
                        self.assertFalse(f.live)
                        self.assertEqual(sum(c[0] == 'start' for c in f.calls), start_count)
                    f.write(work_path, work_bytes, 0o600)
                    child.finish()
                    self.assertEqual(f.live, set(f.allowed))
                    done = f.g.production_json(work_path)
                    self.assertEqual((done['phase'], done['startAttempt'], done['healthAttempt']), ('FINISHED', 1, 1))
                    self.assertEqual(done['approvalSha256'], work['approvalSha256'])
                    proof = f.m.checked_json(work_path.parent / done['healthRecord'], done['healthSha256'])
                    self.assertEqual((proof['transaction'], proof['startAttempt'], proof['health']['bootId']),
                                     (work['transaction'], 1, work['bootId']))
                    finished_bytes = work_path.read_bytes()
                    child.finish()
                    self.assertEqual(work_path.read_bytes(), finished_bytes, 'healthy official reentry must not add an attempt')
                    child.boot()
                    self.assertEqual(len([c for c in f.calls if c[0] == '/usr/bin/systemd-run']), 1)
                finally:
                    child.doCleanups()

    def test_postgate_worker_refuses_while_recovery_oneshot_activating(self):
        self.boot()
        f = self.f
        original = f.property
        with mock.patch.object(f.g, 'systemctl_value', lambda u, p: 'activating' if u == 'gateway-ota-recover.service' and p == 'ActiveState' else original(u, p)):
            with self.assertRaisesRegex(ValueError, 'ordering'): self.finish()
        self.assertFalse(f.live)

    def test_reader_start_can_require_gate_without_deadlock_or_worker_replay(self):
        self.boot()
        f = self.f
        original = f.systemctl
        def systemctl(*args):
            if args[0] == 'start':
                f.write(f.path('/proc/locks'), Path('/proc/locks').read_bytes())
                self.boot()  # exact positive lock proof returns before owner/lease recovery
            return original(*args)
        with mock.patch.object(f.m, 'systemctl', systemctl): self.finish()
        dispatches = [c for c in f.calls if c[0] == '/usr/bin/systemd-run']
        self.assertEqual(len(dispatches), 1)
        self.assertEqual(f.live, set(f.allowed))

    def test_failed_newboot_health_refences_and_official_worker_can_recover(self):
        self.boot()
        f = self.f
        f.fail_health = True
        with self.assertRaisesRegex(ValueError, 'endpoint health'): self.finish()
        self.assertFalse(f.live)
        path = f.m.production_boot_work(f.runtime)
        self.assertEqual(f.g.production_json(path)['phase'], 'FAILED_STOPPED')
        with self.assertRaises(ValueError): f.g.production_unit(f.home, 'gateway-services.service')
        f.fail_health = False
        self.finish()
        self.assertEqual(f.g.production_json(path)['phase'], 'FINISHED')
        self.assertEqual(f.live, set(f.allowed))

    def test_health_published_then_directory_sync_failure_retry_binds_new_pid_attempt(self):
        self.boot()
        f = self.f
        native_publish = f.m.publish_new
        original_pid = f.pid
        def interrupted(path, data, mode=0o600):
            native_publish(path, data, mode)
            if '-health-' in path.name: raise OSError('injected after health publish directory sync')
        with mock.patch.object(f.m, 'publish_new', interrupted):
            with self.assertRaisesRegex(OSError, 'after health publish'): self.finish()
        path = f.m.production_boot_work(f.runtime)
        failed = f.g.production_json(path)
        self.assertEqual(failed['phase'], 'FAILED_STOPPED')
        self.assertEqual(failed['startAttempt'], 1)
        self.assertFalse(f.live)
        old_files = list(path.parent.glob('*-attempt-1-health-*.json'))
        self.assertEqual(len(old_files), 1)
        old_bytes = old_files[0].read_bytes()
        self.assertEqual(f.g.production_json(old_files[0])['health']['readers'][f.g.MONITOR_UNIT]['pid'], original_pid)
        f.pid = '98766'
        self.finish()
        done = f.g.production_json(path)
        self.assertEqual((done['phase'], done['startAttempt'], done['healthAttempt']), ('FINISHED', 2, 2))
        latest = f.g.production_json(path.parent / done['healthRecord'])
        self.assertEqual(latest['health']['readers'][f.g.MONITOR_UNIT]['pid'], '98766')
        self.assertEqual(old_files[0].read_bytes(), old_bytes, 'retain the previous attempt proof byte-for-byte')
        self.assertEqual(f.live, set(f.allowed))


if __name__ == '__main__':
    unittest.main()
