"""Linux private boot chain: real files/flock, substituted native ABI constructor.

The substitute writes ONLY mapped private files. It does not validate the actual
pointctl ELF, robust mutex initialization, target hardware or OS/systemd reboot.
"""
import os
import copy
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
        migration_boot = self._testMethodName == 'test_v1_to_v2_migration_newboot_binds_candidate_config_roles'
        if migration_boot:
            args, self.migration_previous, self.migration_candidate = f.v1_to_v2_release()
            with f.m.production_lock():
                f.m.production_install(args)
            self.adopted = None
        elif adoption_rollback:
            args = f.args()
            f.fail_health = True
            with self.assertRaisesRegex(ValueError, 'endpoint health'):
                with f.m.production_lock(): f.m.production_adopt(args)
            f.fail_health = False
            self.adopted = (f.runtime.root / 'transactions/ADOPT_01', args)
        else:
            self.adopted = f.adopt()
        if self._testMethodName.startswith('test_existing_boot_proof_'):
            self.config_args, self.config_previous = f.v2_config_release(adopt_first=False)
        if self._testMethodName == 'test_v2_role_authority_newboot_proof_and_post_gate_health':
            f.v2_code_only_release(adopt_first=False)
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
        self.mountinfo = f.path('/proc') / str(os.getpid()) / 'mountinfo'
        shm = f.path('/dev/shm')
        device = shm.stat().st_dev
        self.mount_row = '1 0 %d:%d / %s rw - {kind} shm rw\n' % (os.major(device), os.minor(device), shm)
        f.write(self.mountinfo, self.mount_row.format(kind='tmpfs'))
        f.path('/proc/self').symlink_to(str(os.getpid()), target_is_directory=True)
        original_open = os.open
        def opened(path, flags, *args, **kwargs):
            fd = original_open(path, flags, *args, **kwargs)
            if str(path) == str(shm) and flags & os.O_PATH:
                f.write(f.path('/proc') / str(os.getpid()) / 'fdinfo' / str(fd), 'mnt_id:\t1\n')
            return fd
        f.patch(f.m.os, 'open', opened)
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
        boot = f.path('/proc/sys/kernel/random/boot_id').read_text().strip()
        intent = f.g.production_json(f.runtime.root / 'boots' / (boot + '-intent.json'))
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

    def test_v2_role_authority_newboot_proof_and_post_gate_health(self):
        self.boot()
        f = self.f
        proof = f.g.production_json(f.runtime.root / 'boots/NATIVE_BOOT_2.json')
        self.assertEqual(proof['rolesSha256'], f.g.production_roles_sha(self.baseline))
        self.assertEqual(proof['sourceSha256'], f.m.digest(self.source))
        self.assertFalse(f.live, 'v2 oneshot must not synchronously start roles')
        self.finish()
        work = f.g.production_json(f.m.production_boot_work(f.runtime))
        health = f.m.checked_json(f.m.production_boot_work(f.runtime).parent /
                                  work['healthRecord'], work['healthSha256'])
        self.assertEqual(health['health']['format'], 'runtime-health-v2')
        self.assertEqual(set(health['health']['roles']), set(self.baseline['allowedUnits']))
        self.assertEqual(health['health']['bootId'], 'NATIVE_BOOT_2')

    def test_v1_to_v2_migration_newboot_binds_candidate_config_roles(self):
        self.assertEqual(self.baseline, self.migration_candidate)
        self.assertNotEqual(self.f.g.production_roles_sha(self.migration_previous),
                            self.f.g.production_roles_sha(self.baseline))
        self.boot()
        f = self.f
        proof = f.g.production_json(f.runtime.root / 'boots/NATIVE_BOOT_2.json')
        self.assertEqual(proof['rolesSha256'], f.g.production_roles_sha(self.migration_candidate))
        self.assertEqual(proof['sourceSha256'], f.m.digest(self.source))
        self.assertEqual(proof['sourceRecord'], str(self.source.relative_to(f.runtime.root)))
        self.assertFalse(f.live)
        self.finish()
        work = f.g.production_json(f.m.production_boot_work(f.runtime))
        health = f.m.checked_json(f.m.production_boot_work(f.runtime).parent /
                                  work['healthRecord'], work['healthSha256'])
        self.assertEqual(health['health']['format'], 'runtime-health-v2')
        self.assertEqual(health['health']['configSha256'], self.migration_candidate['configSha256'])

    def test_existing_boot_proof_config_install_promotes(self):
        self.boot()
        f = self.f
        proof_path = f.runtime.root / 'boots/NATIVE_BOOT_2.json'
        proof_bytes = proof_path.read_bytes()
        inode = f.path('/dev/shm/live_v11').stat().st_ino
        with f.m.production_lock():
            f.m.production_install(self.config_args)
        self.assertEqual(proof_path.read_bytes(), proof_bytes)
        self.assertEqual(f.path('/dev/shm/live_v11').stat().st_ino, inode)
        self.assertEqual(f.g.production_context(f.home), f.approval['candidate'])
        self.assertEqual((f.home / 'config/runtime/logic/new.json').read_bytes(), b'{"enabled":true}\n')
        receipt = f.runtime.root / 'boot-lineage' / (f.m.digest(
            f.runtime.root / 'transactions/V2_CONFIG/candidate.json') + '-V2_CONFIG.json')
        self.assertTrue(receipt.is_file())
        row = f.g.production_json(receipt)
        for field in ('previousSha256', 'proofSha256', 'rolesSha256'):
            changed = dict(row)
            changed[field] = '0' * 64
            f.write(receipt, json.dumps(changed, sort_keys=True) + '\n', 0o600)
            with self.assertRaisesRegex(ValueError, 'native boot authority lineage'):
                f.g.production_context(f.home)
        f.write(receipt, json.dumps(row, sort_keys=True) + '\n', 0o600)
        self.assertEqual(f.g.production_context(f.home), f.approval['candidate'])

    def test_existing_boot_proof_config_failed_health_recovers(self):
        self.boot()
        f = self.f
        proof_path = f.runtime.root / 'boots/NATIVE_BOOT_2.json'
        proof_bytes = proof_path.read_bytes()
        inode = f.path('/dev/shm/live_v11').stat().st_ino
        f.fail_health = True
        with self.assertRaisesRegex(ValueError, 'endpoint health'):
            with f.m.production_lock():
                f.m.production_install(self.config_args)
        self.assertTrue((f.home / 'config/runtime/logic/new.json').exists())
        self.assertTrue((f.runtime.root / 'pending.json').exists())
        f.fail_health = False
        transaction = f.runtime.root / 'transactions/V2_CONFIG'
        with f.m.production_lock():
            self.assertEqual(f.runtime.recover(transaction, self.config_args.approval_sha256), 'RECOVERED')
        self.assertEqual(proof_path.read_bytes(), proof_bytes)
        self.assertEqual(f.path('/dev/shm/live_v11').stat().st_ino, inode)
        self.assertEqual(f.g.production_context(f.home), self.config_previous)
        self.assertFalse((f.home / 'config/runtime/logic/new.json').exists())

    def test_existing_boot_proof_legacy_depth_limit_rejects_before_intent(self):
        self.boot()
        f = self.f
        proof = (f.runtime.root / 'boots/NATIVE_BOOT_2.json').read_bytes()
        call_count = len(f.calls)
        with mock.patch.object(f.g, 'production_boot_lineage_depth', return_value=128):
            with self.assertRaisesRegex(ValueError, 'lineage capacity exceeded before intent'):
                with f.m.production_lock():
                    f.m.production_install(self.config_args)
        self.assertFalse((f.runtime.root / 'transactions/V2_CONFIG').exists())
        self.assertFalse((f.runtime.root / 'pending.json').exists())
        self.assertFalse(any(call[0] == 'stop' for call in f.calls[call_count:]))
        self.assertEqual((f.runtime.root / 'boots/NATIVE_BOOT_2.json').read_bytes(), proof)

    def test_existing_boot_proof_receipt_before_promoting_recovers(self):
        self.boot()
        f = self.f
        proof = (f.runtime.root / 'boots/NATIVE_BOOT_2.json').read_bytes()
        original_state = f.m.ProductionRuntime.state
        reached = False
        def fail_promoting(runtime, transaction, value):
            nonlocal reached
            if value['phase'] == 'PROMOTING':
                reached = True
                receipt = f.runtime.root / 'boot-lineage' / (
                    f.m.digest(transaction / 'candidate.json') + '-V2_CONFIG.json')
                self.assertTrue(receipt.is_file())
                raise OSError('injected before PROMOTING state')
            return original_state(runtime, transaction, value)
        with mock.patch.object(f.m.ProductionRuntime, 'state', fail_promoting):
            with self.assertRaisesRegex(OSError, 'before PROMOTING state'):
                with f.m.production_lock():
                    f.m.production_install(self.config_args)
        self.assertTrue(reached)
        self.assertEqual(f.g.production_json(f.runtime.root / 'current.json')['baselineId'], 'V2_START')
        transaction = f.runtime.root / 'transactions/V2_CONFIG'
        with f.m.production_lock():
            self.assertEqual(f.runtime.recover(transaction, self.config_args.approval_sha256), 'RECOVERED')
        self.assertEqual(f.g.production_context(f.home), self.config_previous)
        self.assertEqual((f.runtime.root / 'boots/NATIVE_BOOT_2.json').read_bytes(), proof)
        self.retry_same_candidate_after_orphan()

    def retry_same_candidate_after_orphan(self):
        f = self.f
        candidate_sha = f.m.digest(f.runtime.root / 'transactions/V2_CONFIG/candidate.json')
        old_receipt = f.runtime.root / 'boot-lineage' / (candidate_sha + '-V2_CONFIG.json')
        old_bytes = old_receipt.read_bytes()
        old_record = f.runtime.root / 'baselines' / (f.approval['candidate']['baselineId'] + '.json')
        old_record_bytes = old_record.read_bytes()
        f.approval['transaction'] = 'V2_CONFIG_RETRY'
        retry = f.args()
        with self.assertRaisesRegex(ValueError, 'candidate baseline ID already occupied'):
            with f.m.production_lock():
                f.m.production_install(retry)
        self.assertFalse((f.runtime.root / 'transactions/V2_CONFIG_RETRY').exists())
        self.assertEqual(f.g.production_context(f.home), self.config_previous)
        f.approval['candidate']['baselineId'] = 'V2_CONFIG_RETRY'
        retry = f.args()
        with f.m.production_lock():
            f.m.production_install(retry)
        candidate_sha = f.m.digest(f.runtime.root / 'transactions/V2_CONFIG_RETRY/candidate.json')
        new_receipt = f.runtime.root / 'boot-lineage' / (candidate_sha + '-V2_CONFIG_RETRY.json')
        self.assertTrue(new_receipt.is_file())
        self.assertEqual(old_receipt.read_bytes(), old_bytes)
        self.assertEqual(old_record.read_bytes(), old_record_bytes)
        self.assertEqual(f.g.production_context(f.home), f.approval['candidate'])

    def assert_pointer_window(self, after_replace):
        self.boot()
        f = self.f
        proof = (f.runtime.root / 'boots/NATIVE_BOOT_2.json').read_bytes()
        original_replace = f.m.replace
        reached = False
        def fail_pointer(path, data, mode=0o600):
            nonlocal reached
            if path == f.runtime.root / 'current.json' and b'V2_CONFIG' in data:
                reached = True
                receipt = f.runtime.root / 'boot-lineage' / (
                    f.m.digest(f.runtime.root / 'transactions/V2_CONFIG/candidate.json') + '-V2_CONFIG.json')
                self.assertTrue(receipt.is_file())
                if after_replace:
                    original_replace(path, data, mode)
                raise OSError('injected current pointer window')
            return original_replace(path, data, mode)
        with mock.patch.object(f.m, 'replace', fail_pointer):
            with self.assertRaisesRegex(OSError, 'current pointer window'):
                with f.m.production_lock():
                    f.m.production_install(self.config_args)
        self.assertTrue(reached)
        transaction = f.runtime.root / 'transactions/V2_CONFIG'
        self.assertEqual(f.g.production_json(f.runtime.root / 'current.json')['baselineId'],
                         'V2_CONFIG' if after_replace else 'V2_START')
        with f.m.production_lock():
            result = f.runtime.recover(transaction, self.config_args.approval_sha256)
        self.assertEqual(result, 'COMPLETED' if after_replace else 'RECOVERED')
        self.assertEqual(f.g.production_context(f.home),
                         f.approval['candidate'] if after_replace else self.config_previous)
        self.assertEqual((f.runtime.root / 'boots/NATIVE_BOOT_2.json').read_bytes(), proof)
        if not after_replace:
            self.retry_same_candidate_after_orphan()

    def test_existing_boot_proof_pointer_before_replace_recovers(self):
        self.assert_pointer_window(False)

    def test_existing_boot_proof_pointer_after_replace_completes(self):
        self.assert_pointer_window(True)

    def test_existing_boot_proof_promoted_pending_cross_boot_completes(self):
        self.boot()
        f = self.f
        with mock.patch.object(f.m.ProductionRuntime, 'clear_pending',
                               lambda runtime, state: (_ for _ in ()).throw(OSError('injected pending clear'))):
            with self.assertRaisesRegex(OSError, 'pending clear'):
                with f.m.production_lock():
                    f.m.production_install(self.config_args)
        transaction = f.runtime.root / 'transactions/V2_CONFIG'
        self.assertEqual(f.g.production_json(transaction / 'state.json')['phase'], 'COMPLETED')
        self.assertTrue((f.runtime.root / 'pending.json').is_file())
        candidate_sha = f.m.digest(transaction / 'candidate.json')
        receipt = f.runtime.root / 'boot-lineage' / (candidate_sha + '-V2_CONFIG.json')
        old_receipt = receipt.read_bytes()
        old_proof = (f.runtime.root / 'boots/NATIVE_BOOT_2.json').read_bytes()

        f.write(f.path('/proc/sys/kernel/random/boot_id'), 'NATIVE_BOOT_3\n')
        f.live.clear()
        proc = f.path('/proc') / f.pid
        if proc.exists():
            shutil.rmtree(proc)
        f.path('/dev/shm/live_v11').unlink()
        self.baseline = f.approval['candidate']
        self.boot()
        self.finish()

        self.assertEqual(f.g.production_json(transaction / 'state.json')['phase'], 'COMPLETED')
        self.assertFalse((f.runtime.root / 'pending.json').exists())
        self.assertEqual(receipt.read_bytes(), old_receipt)
        self.assertEqual((f.runtime.root / 'boots/NATIVE_BOOT_2.json').read_bytes(), old_proof)
        self.assertTrue((f.runtime.root / 'boots/NATIVE_BOOT_3.json').is_file())
        self.assertEqual(f.g.production_context(f.home), f.approval['candidate'])
        changed = f.g.production_json(receipt)
        changed['rolesSha256'] = '0' * 64
        f.write(receipt, json.dumps(changed, sort_keys=True) + '\n', 0o600)
        with self.assertRaisesRegex(ValueError, 'production boot lineage receipt changed'):
            with f.m.production_lock():
                f.runtime.recover(transaction, self.config_args.approval_sha256)
        f.write(receipt, old_receipt, 0o600)
        with f.m.production_lock():
            self.assertEqual(f.runtime.recover(transaction, self.config_args.approval_sha256), 'COMPLETED')
        self.assertEqual(receipt.read_bytes(), old_receipt)

    def test_existing_boot_proof_promoted_pending_unlink_sync_retry(self):
        self.boot()
        f = self.f
        pending = f.runtime.root / 'pending.json'
        original_sync = f.m.sync_dir
        failed = False
        def fail_after_unlink(path):
            nonlocal failed
            if path == f.runtime.root and not pending.exists() and not failed:
                failed = True
                raise OSError('injected pending unlink root sync')
            return original_sync(path)
        with mock.patch.object(f.m, 'sync_dir', fail_after_unlink):
            with self.assertRaisesRegex(OSError, 'pending unlink root sync'):
                with f.m.production_lock():
                    f.m.production_install(self.config_args)
        self.assertTrue(failed)
        self.assertFalse(pending.exists())
        transaction = f.runtime.root / 'transactions/V2_CONFIG'
        self.assertEqual(f.g.production_json(transaction / 'state.json')['phase'], 'COMPLETED')
        root_syncs = []
        def record_sync(path):
            if path == f.runtime.root:
                root_syncs.append(path)
            return original_sync(path)
        with mock.patch.object(f.m, 'sync_dir', record_sync):
            with f.m.production_lock():
                self.assertEqual(f.runtime.recover(transaction, self.config_args.approval_sha256), 'COMPLETED')
        self.assertTrue(root_syncs, 'official recovery must retry the root directory sync')
        self.assertEqual(f.g.production_context(f.home), f.approval['candidate'])

    def next_config_change(self, transaction, padding):
        f = self.f
        previous = copy.deepcopy(f.g.production_context(f.home))
        staged = f.payload / 'config/runtime'
        app = staged / 'apps/monitor-service.json'
        value = json.loads(app.read_text())
        f.write(app, json.dumps(value, sort_keys=True, indent=2) + ' ' * padding + '\n')
        candidate = copy.deepcopy(previous)
        candidate.update(baselineId=transaction, generation=previous['generation'] + 1,
                         parentSha256=f.g.production_json(f.runtime.root / 'current.json')['sha256'],
                         release=transaction, configSha256=f.m.tree_hashes(staged))
        parser = Path(__file__).resolve().parents[1] / 'deploy/production_role_policy.py'
        policy = {'__name__': 'private_v2_policy'}
        exec(compile(parser.read_bytes(), str(parser), 'exec'), policy)
        candidate['roleAuthority']['resolved'] = policy['resolve'](
            staged, f.path('/etc/systemd/system'), previous['roleAuthority']['resolved']['protected'],
            f.path('/etc/modbus-gateway/ota.pub'))
        f.approval.update(transaction=transaction, previous=previous, candidate=candidate,
                          parentSha256=candidate['parentSha256'],
                          configWriteSet=[{'path': 'apps/monitor-service.json',
                                           'before': f.g.production_pin(f.home / 'config/runtime/apps/monitor-service.json'),
                                           'after': f.g.production_pin(app)}])
        return f.args(), previous

    def test_existing_boot_proof_second_config_failure_recovers(self):
        self.boot()
        f = self.f
        with f.m.production_lock():
            f.m.production_install(self.config_args)
        original_proof = (f.runtime.root / 'boots/NATIVE_BOOT_2.json').read_bytes()
        second, first = self.next_config_change('V2_CONFIG_2', 1)
        f.fail_health = True
        with self.assertRaisesRegex(ValueError, 'endpoint health'):
            with f.m.production_lock():
                f.m.production_install(second)
        f.fail_health = False
        transaction = f.runtime.root / 'transactions/V2_CONFIG_2'
        with f.m.production_lock():
            self.assertEqual(f.runtime.recover(transaction, second.approval_sha256), 'RECOVERED')
        self.assertEqual(f.g.production_context(f.home), first)
        self.assertEqual((f.runtime.root / 'boots/NATIVE_BOOT_2.json').read_bytes(), original_proof)

    def test_existing_boot_proof_127_128_129_config_promotions(self):
        self.boot()
        f = self.f
        proof_path = f.runtime.root / 'boots/NATIVE_BOOT_2.json'
        proof = proof_path.read_bytes()
        with f.m.production_lock():
            f.m.production_install(self.config_args)
        for edge in range(2, 130):
            transaction = 'V2_STEP_%03d' % edge
            args, previous = self.next_config_change(transaction, edge)
            self.assertNotEqual(f.g.production_roles_sha(previous),
                                f.g.production_roles_sha(f.approval['candidate']))
            with f.m.production_lock():
                f.m.production_install(args)
            if edge in (127, 128, 129):
                self.assertEqual(f.g.production_context(f.home), f.approval['candidate'])
                candidate_sha = f.m.digest(f.runtime.root / 'transactions' / transaction / 'candidate.json')
                receipt = f.runtime.root / 'boot-lineage' / (candidate_sha + '-' + transaction + '.json')
                row = f.g.production_json(receipt)
                self.assertEqual(row['format'], 'runtime-boot-lineage-checkpoint-v1')
                self.assertEqual(row['proofSha256'], f.m.digest(proof_path))
                self.assertEqual(proof_path.read_bytes(), proof)
        failed, previous = self.next_config_change('V2_STEP_130_FAIL', 130)
        f.fail_health = True
        with self.assertRaisesRegex(ValueError, 'endpoint health'):
            with f.m.production_lock():
                f.m.production_install(failed)
        f.fail_health = False
        transaction = f.runtime.root / 'transactions/V2_STEP_130_FAIL'
        with f.m.production_lock():
            self.assertEqual(f.runtime.recover(transaction, failed.approval_sha256), 'RECOVERED')
        self.assertEqual(f.g.production_context(f.home), previous)
        self.assertEqual(proof_path.read_bytes(), proof)

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
        f.write(self.mountinfo, self.mount_row.format(kind='ext4'))
        with self.assertRaisesRegex(ValueError, 'tmpfs'): self.boot()
        self.assert_unadmitted()
        f.write(self.mountinfo, self.mount_row.format(kind='tmpfs'))
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

    def test_retired_source_during_health_records_unconfirmed_stop_without_retry(self):
        self.boot()
        f = self.f
        def failed_health(runtime, baseline):
            f.write(f.path('/dev/shm/retired_v10'), b'unexpected source')
            raise ValueError('injected endpoint health failure')
        with mock.patch.object(f.m.ProductionRuntime, 'health', failed_health):
            with self.assertRaisesRegex(ValueError, 'retired source recreated'):
                self.finish()
        self.assertFalse(f.live)
        path = f.m.production_boot_work(f.runtime)
        failed_bytes = path.read_bytes()
        self.assertEqual(f.g.production_json(path)['phase'], 'FAILED_STOP_UNCONFIRMED')
        start_count = sum(c[0] == 'start' for c in f.calls)
        with self.assertRaisesRegex(ValueError, 'unconfirmed'):
            self.finish()
        self.assertEqual(path.read_bytes(), failed_bytes)
        self.assertEqual(sum(c[0] == 'start' for c in f.calls), start_count)
        self.assertEqual(f.path('/dev/shm/retired_v10').read_bytes(), b'unexpected source')


if __name__ == '__main__':
    unittest.main()
