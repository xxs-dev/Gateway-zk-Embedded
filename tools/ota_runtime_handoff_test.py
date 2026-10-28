#!/usr/bin/env python3
"""Private-directory handoff tests. Never apply, stop or restart business units."""
import fcntl
import contextlib
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('ota_health', REPO / 'deploy/ota-health-result.py')
health = importlib.util.module_from_spec(spec)
spec.loader.exec_module(health)


class HandoffTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='ota-handoff-')
        self.home = Path(self.temp.name) / 'home'
        self.root = self.home / 'ota/staging'
        self.root.mkdir(parents=True, mode=0o700)
        self.job = 'JOB_' + Path(self.temp.name).name.replace('-', '_')
        self.baseline(self.home)
        self.token = '12345678-1234-4234-8234-123456789abc'
        self.entry = self.root / 'entry'
        self.entry.mkdir()
        for name in ('offline-runtime-upgrade.py', 'runtime-upgrade-guard.py', 'ota-health-result.py'):
            shutil.copyfile(REPO / 'deploy' / name, self.entry / name)
        artifact = self.root / 'artifact.tar.gz'
        artifact.write_bytes(b'private fixture artifact, never unpacked')
        self.intent = dict(format='ota-job-v1', jobId=self.job, machineCode='LOCAL_ONLY',
                           version='1.0', artifactPath=str(artifact), previousVersion='0.0',
                           token=self.token, lifecycle='systemd-handoff-v1', artifactSize=str(artifact.stat().st_size),
                           artifactSha256=hashlib.sha256(artifact.read_bytes()).hexdigest())
        self.intent_path = self.root / ('ota_job_' + self.job + '.json')
        self.intent_path.write_text(json.dumps(self.intent) + '\n')
        (self.root / 'ota_active_job.txt').write_text('jobId=' + self.job + '\ntoken=' + self.token + '\n')
        self.lease = os.open(str(self.root / ('ota_job_' + self.job + '.lock')), os.O_CREAT | os.O_RDWR, 0o600)
        fcntl.flock(self.lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
        info = os.fstat(self.lease)
        self.intent.update(leaseDev=str(info.st_dev), leaseInode=str(info.st_ino))
        self.intent_path.write_text(json.dumps(self.intent) + '\n')

    def baseline(self, home, version=1):
        # The staged helper set comes from a pinned baseline, even when admission
        # will later reject this intentionally incomplete private fixture.
        root = home / 'data/runtime-production'
        (root / 'baselines').mkdir(parents=True, exist_ok=True)
        raw = (json.dumps(dict(format='runtime-baseline-v' + str(version))) + '\n').encode()
        (root / 'baselines/LOCAL_HANDOFF.json').write_bytes(raw)
        (root / 'current.json').write_text(json.dumps(dict(baselineId='LOCAL_HANDOFF',
            sha256=hashlib.sha256(raw).hexdigest())) + '\n')

    def test_changed_baseline_refuses_dispatch_before_unit_launch(self):
        protocol = health.RuntimeHandoff(self.root, self.job, self.token)
        (self.home / 'data/runtime-production/baselines/LOCAL_HANDOFF.json').write_text('{}\n')
        with self.assertRaisesRegex(ValueError, 'baseline changed'):
            protocol.launch(self.entry / 'offline-runtime-upgrade.py', self.lease)
        self.assertFalse(protocol.path.exists())
        self.assertEqual(protocol.properties()['LoadState'], 'not-found')

    def test_config_handoff_stages_v2_role_policy_before_admission(self):
        self.baseline(self.home, version=2)
        shutil.copyfile(REPO / 'deploy/production_role_policy.py', self.entry / 'production_role_policy.py')
        self.intent['packageType'] = 'config'
        self.intent_path.write_text(json.dumps(self.intent) + '\n')
        protocol = self.launch()
        ready = protocol.read()
        self.assertEqual(ready['phase'], 'OWNER_READY')
        self.assertEqual(ready['entrySchema'], 'paired-v2')
        self.assertEqual(set(ready['pins']), {'offline-runtime-upgrade.py',
            'runtime-upgrade-guard.py', 'ota-health-result.py', 'production_role_policy.py'})
        self.release()
        self.wait_phase(protocol, 'OWNER_BLOCKED')
        self.assertFalse(protocol.read()['runtimeMayChange'])
        protocol.cancel()
        self.assertEqual(protocol.read()['phase'], 'CANCELLED')

    def tearDown(self):
        if self.lease >= 0:
            os.close(self.lease)
        # Only this private transaction's transient unit; never business services.
        protocol = health.RuntimeHandoff(self.root, self.job, self.token)
        subprocess.run(['/usr/bin/systemctl', 'stop', protocol.unit], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)
        props = protocol.properties()
        self.assertEqual(props['MainPID'], '0')
        self.assertIn(props['ActiveState'], ('inactive', 'failed'))
        if props['LoadState'] != 'not-found':
            subprocess.run(['/usr/bin/systemctl', 'reset-failed', protocol.unit], check=True,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)
        self.temp.cleanup()

    def launch(self):
        protocol = health.RuntimeHandoff(self.root, self.job, self.token)
        protocol.launch(self.entry / 'offline-runtime-upgrade.py', self.lease)
        return protocol

    def release(self):
        fcntl.flock(self.lease, fcntl.LOCK_UN)

    def wait_phase(self, protocol, phase):
        deadline = time.monotonic() + 10
        while protocol.read()['phase'] != phase and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertEqual(protocol.read()['phase'], phase)

    def test_cancel_tombstone_prevents_waiting_or_future_owner_and_no_false_health(self):
        protocol = self.launch()
        with self.assertRaises(BlockingIOError):
            protocol.cancel() # donor still owns lease: no unchanged proof yet
        self.assertEqual(protocol.read()['phase'], 'CANCEL_REQUESTED')
        self.assertFalse((self.root / ('health_' + self.job + '.txt')).exists())
        self.release()
        protocol.cancel()
        with self.assertRaisesRegex(ValueError, 'cancelled or stale'):
            protocol.own()
        self.assertEqual(protocol.read()['phase'], 'CANCELLED')

    def test_ready_artifact_drift_refuses_owned(self):
        protocol = self.launch()
        Path(self.intent['artifactPath']).write_bytes(b'x' * int(self.intent['artifactSize']))
        self.release()
        deadline = time.monotonic() + 5
        while protocol.properties()['MainPID'] != '0' and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertEqual(protocol.read()['phase'], 'OWNER_READY')
        protocol.cancel()

    def test_ready_active_job_drift_refuses_owned_and_cancelled_final(self):
        protocol = self.launch()
        (self.root / 'ota_active_job.txt').write_text('jobId=OTHER\ntoken=' + self.token + '\n')
        self.release()
        time.sleep(0.15)
        with self.assertRaisesRegex(ValueError, 'active job changed'):
            protocol.cancel()
        self.assertFalse((self.root / ('health_' + self.job + '.txt')).exists())

    def test_ready_staged_pin_drift_refuses_owned(self):
        protocol = self.launch()
        path = Path(protocol.read()['entry']) / 'runtime-upgrade-guard.py'
        path.chmod(0o600)
        path.write_text('# drift\n')
        self.release()
        time.sleep(0.15)
        self.assertEqual(protocol.read()['phase'], 'OWNER_READY')
        protocol.cancel()

    def test_wrong_token_unit_pid_cgroup_or_boot_refuses_cancel(self):
        protocol = self.launch()
        ready = protocol.read()
        cases = [('token', '00000000-0000-4000-8000-000000000000'), ('unit', 'not-our-unit.service'),
                 ('bootId', 'different-boot'), ('invocationId', '0' * 32)]
        for field, value in cases:
            damaged = dict(ready)
            damaged[field] = value
            protocol.save(damaged)
            with self.assertRaises(ValueError):
                protocol.cancel()
            protocol.save(ready)
        for field, value in [('startTicks', '0'), ('cgroup', '/wrong')]:
            damaged = dict(ready)
            damaged['owner'] = dict(ready['owner'], **{field: value})
            protocol.save(damaged)
            with self.assertRaisesRegex(ValueError, 'PID reused or cgroup changed'):
                protocol.cancel()
            protocol.save(ready)
        self.release()
        self.wait_phase(protocol, 'OWNER_BLOCKED')
        protocol.cancel()

    def test_health_atomic_publication_faults_recover_without_truncation(self):
        protocol = self.launch()
        self.release()
        self.wait_phase(protocol, 'OWNER_BLOCKED')
        target = self.root / ('health_' + self.job + '.txt')
        rename = os.rename
        with patch.object(health.os, 'rename', side_effect=OSError('injected rename failure')):
            with self.assertRaisesRegex(OSError, 'injected'):
                protocol.cancel()
        self.assertFalse(target.exists())
        sync = health.sync_dir
        def failed_sync(path):
            if target.exists():
                raise OSError('injected directory fsync failure')
            return sync(path)
        with patch.object(health, 'sync_dir', side_effect=failed_sync):
            with self.assertRaisesRegex(OSError, 'injected'):
                protocol.cancel()
        self.assertTrue(target.read_bytes().endswith(b'\n'))
        protocol.cancel()
        self.assertEqual(protocol.read()['phase'], 'CANCELLED')

    def test_health_write_and_file_fsync_faults_leave_no_partial_decision(self):
        protocol = self.launch()
        self.release()
        self.wait_phase(protocol, 'OWNER_BLOCKED')
        target = self.root / ('health_' + self.job + '.txt')
        fdopen, fsync = os.fdopen, os.fsync
        class FailedWrite:
            def __init__(self, output):
                self.output = output
            def __enter__(self):
                return self
            def __exit__(self, *args):
                self.output.close()
            def write(self, data):
                self.output.write(data[:7])
                raise OSError('injected health write failure')
        def failed_write(fd, mode):
            output = fdopen(fd, mode)
            if os.readlink('/proc/self/fd/' + str(fd)).startswith(str(target) + '.tmp.'):
                return FailedWrite(output)
            return output
        def failed_fsync(fd):
            if os.readlink('/proc/self/fd/' + str(fd)).startswith(str(target) + '.tmp.'):
                raise OSError('injected health file fsync failure')
            return fsync(fd)
        for attribute, fault in [('fdopen', failed_write), ('fsync', failed_fsync)]:
            with patch.object(health.os, attribute, side_effect=fault):
                with self.assertRaisesRegex(OSError, 'injected health'):
                    protocol.cancel()
            self.assertFalse(target.exists())
            self.assertFalse(list(self.root.glob(target.name + '.tmp.*')))
        protocol.cancel()
        self.assertEqual(protocol.json_file(target)['rollback'], 'unchanged')

    def test_conflicting_completed_final_refuses_cancelled_health(self):
        protocol = self.launch()
        self.release()
        self.wait_phase(protocol, 'OWNER_BLOCKED')
        final = self.root / ('ota_final_' + self.job + '.txt')
        final.write_text('jobId=' + self.job + '\ntoken=' + self.token + '\nstatus=completed\n')
        with self.assertRaisesRegex(ValueError, 'final conflicts'):
            protocol.cancel()
        self.assertFalse((self.root / ('health_' + self.job + '.txt')).exists())

    def test_unbound_cgroup_layouts_wrong_roots_and_membership_refuse(self):
        read_text = health.RuntimeHandoff.kernel_text
        valid = '1 0 0:1 / /sys/fs/cgroup rw - cgroup2 cgroup rw\n'
        legacy = '2 0 0:2 / /sys/fs/cgroup/systemd rw - cgroup cgroup rw\n'
        for mount, membership in [(legacy, '1:name=systemd:/\n'),
                                  (valid + legacy, '0::/\n'),
                                  (valid.replace(' / /sys/', ' /nested /sys/'), '0::/\n'),
                                  (valid.replace('/sys/fs/cgroup', '/wrong'), '0::/\n'),
                                  (valid, '1:name=systemd:/\n')]:
            def contents(path, *args, **kwargs):
                if str(path) == '/proc/self/mountinfo':
                    return mount
                if str(path) == '/proc/self/cgroup':
                    return membership
                return read_text(path, *args, **kwargs)
            with patch.object(health.RuntimeHandoff, 'kernel_text', side_effect=contents):
                with self.assertRaisesRegex(ValueError, 'cgroup'):
                    health.RuntimeHandoff.cgroup_mount()

    @contextlib.contextmanager
    def board_hybrid_layout(self, board):
        # Exact hierarchy/controller order from coordinator's read-only board
        # snapshots. Files below are private substitutes, not mounted cgroups.
        controllers = {23: ['cpu,cpuacct', 'devices', 'cpuset', 'pids', 'freezer', 'memory', 'blkio'],
                       24: ['cpu,cpuacct', 'blkio', 'cpuset', 'pids', 'memory', 'freezer', 'devices']}[board]
        mounts = ['1 0 0:1 / /sys/fs/cgroup/unified rw - cgroup2 cgroup rw,nsdelegate',
                  '2 0 0:2 / /sys/fs/cgroup/systemd rw - cgroup cgroup rw,xattr,name=systemd']
        mounts += [str(i + 2) + ' 0 0:' + str(i + 2) + ' / /sys/fs/cgroup/' + controller +
                   ' rw - cgroup cgroup rw,' + controller for i, controller in enumerate(controllers, 2)]
        self_group = '/user.slice/user-0.slice/session-c' + ('119' if board == 23 else '54') + '.scope'
        rows = ['0::' + self_group, '1:name=systemd:' + self_group]
        rows += [str(i) + ':' + controller + ':' + (self_group if controller in ('memory', 'pids') else
                 '/user.slice' if controller == 'devices' else '/') for i, controller in enumerate(controllers, 2)]
        pid1 = ['0::/init.scope', '1:name=systemd:/init.scope']
        pid1 += [str(i) + ':' + controller + ':' + ('/init.scope' if controller in ('memory', 'pids', 'devices') else '/')
                 for i, controller in enumerate(controllers, 2)]
        layout = dict(mountinfo='\n'.join(mounts) + '\n', selfCgroup='\n'.join(reversed(rows)) + '\n',
                      pid1Cgroup='\n'.join(reversed(pid1)) + '\n')
        root = self.root / ('hybrid-' + str(board))
        for mount in ('systemd', 'unified'):
            for group, pid in ((self_group, os.getpid()), ('/init.scope', 1)):
                path = root / mount / group.lstrip('/')
                path.mkdir(parents=True)
                (path / 'cgroup.procs').write_text(str(pid) + '\n')
        original_text = health.RuntimeHandoff.kernel_text
        def text(path, *args, **kwargs):
            key = {'/proc/self/mountinfo': 'mountinfo', '/proc/self/cgroup': 'selfCgroup',
                   '/proc/1/cgroup': 'pid1Cgroup'}.get(str(path))
            return layout[key] if key else original_text(path, *args, **kwargs)
        def path(value, *parts):
            actual = Path(value, *parts)
            prefix = '/sys/fs/cgroup/'
            return root / str(actual)[len(prefix):] if str(actual).startswith(prefix) else actual
        with patch.object(health, 'Path', side_effect=path), patch.object(health.RuntimeHandoff, 'kernel_text', side_effect=text):
            yield layout, root

    def test_both_board_hybrid_layouts_select_named_systemd_not_unified(self):
        for board in (23, 24):
            with self.subTest(board=board), self.board_hybrid_layout(board):
                mounted = health.RuntimeHandoff.cgroup_mount()
                self.assertEqual(mounted['path'], '/sys/fs/cgroup/systemd')
                self.assertEqual(mounted['controller'], 'name=systemd')
                self.assertEqual(health.RuntimeHandoff.process(1)['cgroup'], '/init.scope')

    def test_hybrid_wrong_missing_mount_membership_or_descendant_refuses(self):
        with self.board_hybrid_layout(23) as (layout, root):
            original = layout['mountinfo']
            for invalid in (original.replace(' / /sys/fs/cgroup/systemd ', ' /nested /sys/fs/cgroup/systemd '),
                            '\n'.join(row for row in original.splitlines() if 'name=systemd' not in row) + '\n',
                            original + next(row for row in original.splitlines() if 'name=systemd' in row) + '\n'):
                layout['mountinfo'] = invalid
                with self.assertRaises(ValueError):
                    health.RuntimeHandoff.cgroup_mount()
            layout['mountinfo'] = original
            original_pid1 = layout['pid1Cgroup']
            layout['pid1Cgroup'] = original_pid1.replace('1:name=systemd:', '9:name=systemd:')
            with self.assertRaises(ValueError):
                health.RuntimeHandoff.cgroup_mount()
            layout['pid1Cgroup'] = original_pid1
            mount = health.RuntimeHandoff.cgroup_mount()
            protocol = health.RuntimeHandoff(self.root, self.job, self.token)
            group = root / 'systemd/system.slice' / protocol.unit
            group.mkdir(parents=True)
            (group / 'cgroup.procs').write_text('')
            child = group / 'child'
            child.mkdir()
            (child / 'cgroup.procs').write_text('54321\n')
            record = dict(bootId=protocol.boot(), cgroupMount=mount,
                          unitControlGroup='/system.slice/' + protocol.unit)
            with self.assertRaisesRegex(ValueError, 'still has tasks'):
                protocol.no_unit_tasks(record)
            (child / 'cgroup.procs').write_text('')
            protocol.no_unit_tasks(record)
            # A valid v2 tree does not justify a missing v1 systemd mount.
            shutil.rmtree(root / 'systemd')
            with self.assertRaises(ValueError):
                protocol.no_unit_tasks(record)

    def test_hybrid_reboot_mount_change_requires_absent_unit_and_empty_current_tree(self):
        protocol = health.RuntimeHandoff(self.root, self.job, self.token)
        self.release()
        with self.board_hybrid_layout(24) as (layout, root):
            mounted = protocol.cgroup_mount()
            old_mount = dict(mounted, dev=mounted['dev'] + 1)
            info = os.fstat(self.lease)
            protocol.save(dict(format='ota-handoff-v1', jobId=self.job, token=self.token,
                               version=self.intent['version'], unit=protocol.unit, bootId='previous-boot',
                               phase='HANDOFF_PENDING', runtimeMayChange=False, cgroupMount=old_mount,
                               intentSha256=hashlib.sha256(protocol.file_bytes(self.intent_path)).hexdigest(),
                               leaseIdentity=dict(dev=info.st_dev, inode=info.st_ino),
                               unitControlGroup='/system.slice/' + protocol.unit))
            props = protocol.properties()
            with patch.object(protocol, 'properties', return_value=dict(props, LoadState='loaded')):
                with self.assertRaisesRegex(ValueError, 'unit absence is unproved'):
                    protocol.recover()
            group = root / 'systemd/system.slice' / protocol.unit
            group.mkdir(parents=True)
            (group / 'cgroup.procs').write_text('54321\n')
            with self.assertRaisesRegex(ValueError, 'still has tasks'):
                protocol.recover()
            self.assertEqual(protocol.read()['phase'], 'CANCEL_REQUESTED')
            self.assertFalse((self.root / ('health_' + self.job + '.txt')).exists())
            (group / 'cgroup.procs').write_text('')
            protocol.recover()
            self.assertEqual(protocol.read()['phase'], 'CANCELLED')
            self.assertEqual(protocol.read()['cancelProof']['bootId'], protocol.boot())

    def test_cgroup_scan_or_read_errors_cannot_publish_cancelled_health(self):
        protocol = self.launch()
        self.release()
        self.wait_phase(protocol, 'OWNER_BLOCKED')
        mount = self.root / 'error-cgroup'
        group = mount / 'system.slice' / protocol.unit
        group.mkdir(parents=True)
        tasks = group / 'cgroup.procs'
        tasks.write_text('')
        identity = dict(path=str(mount), dev=mount.stat().st_dev, inode=mount.stat().st_ino)
        record = protocol.read()
        record['cgroupMount'] = identity
        protocol.save(record)
        target = self.root / ('health_' + self.job + '.txt')
        original_open = Path.open
        with patch.object(protocol, 'cgroup_mount', return_value=identity):
            for error in (PermissionError('injected scandir permission'), OSError(5, 'injected scandir EIO')):
                def failed_walk(path, followlinks=False, onerror=None):
                    if onerror is not None:
                        onerror(error)
                    return iter(())
                with patch.object(health.os, 'walk', side_effect=failed_walk):
                    with self.assertRaises(OSError):
                        protocol.cancel()
                self.assertEqual(protocol.read()['phase'], 'CANCEL_REQUESTED')
                self.assertFalse(target.exists())
            for error in (PermissionError('injected cgroup.procs permission'), FileNotFoundError('injected cgroup.procs absent')):
                def failed_open(path, *args, **kwargs):
                    if path == tasks:
                        raise error
                    return original_open(path, *args, **kwargs)
                with patch.object(Path, 'open', failed_open):
                    with self.assertRaises(OSError):
                        protocol.cancel()
                self.assertEqual(protocol.read()['phase'], 'CANCEL_REQUESTED')
                self.assertFalse(target.exists())
            protocol.cancel()
            self.assertEqual(protocol.read()['phase'], 'CANCELLED')

    def test_cgroup_descendant_tasks_and_dangling_root_refuse_no_writer_proof(self):
        protocol = health.RuntimeHandoff(self.root, self.job, self.token)
        mount = self.root / 'private-cgroup'
        mount.mkdir()
        identity = dict(path=str(mount), dev=mount.stat().st_dev, inode=mount.stat().st_ino)
        record = dict(bootId=protocol.boot(), cgroupMount=identity,
                      unitControlGroup='/system.slice/' + protocol.unit)
        group = mount / 'system.slice' / protocol.unit
        group.mkdir(parents=True)
        (group / 'cgroup.procs').write_text('')
        child = group / 'child'
        child.mkdir()
        (child / 'cgroup.procs').write_text('999999\n')
        with patch.object(protocol, 'cgroup_mount', return_value=identity):
            with self.assertRaisesRegex(ValueError, 'still has tasks'):
                protocol.no_unit_tasks(record)
            (child / 'cgroup.procs').write_text('')
            protocol.no_unit_tasks(record)
            shutil.rmtree(group)
            group.symlink_to(mount / 'absent')
            with self.assertRaisesRegex(ValueError, 'symlink'):
                protocol.no_unit_tasks(record)

    def test_reboot_new_mount_requires_absent_unit_original_lease_and_tombstone(self):
        protocol = self.launch()
        self.release()
        self.wait_phase(protocol, 'OWNER_BLOCKED')
        stopped = subprocess.run(['/usr/bin/systemctl', 'stop', protocol.unit],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)
        self.assertIn(stopped.returncode, (0, 5))
        self.assertEqual(protocol.properties()['LoadState'], 'not-found')
        record = protocol.read()
        record['cgroupMount']['dev'] += 1
        protocol.save(record)
        with self.assertRaisesRegex(ValueError, 'cgroup root changed'):
            protocol.recover()
        record['bootId'] = 'fixture-previous-boot'
        protocol.save(record)
        fcntl.flock(self.lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
        with self.assertRaises(BlockingIOError):
            protocol.recover()
        self.assertEqual(protocol.read()['phase'], 'CANCEL_REQUESTED')
        self.assertFalse((self.root / ('health_' + self.job + '.txt')).exists())
        self.release()
        protocol.recover()
        self.assertEqual(protocol.read()['phase'], 'CANCELLED')
        self.assertEqual(protocol.read()['cancelProof']['bootId'], protocol.boot())
        with self.assertRaisesRegex(ValueError, 'cancelled or stale'):
            protocol.own()

    def test_intent_drift_after_ready_refuses_ownership_and_recovery(self):
        protocol = self.launch()
        self.intent['version'] = 'different-version'
        self.intent_path.write_text(json.dumps(self.intent) + '\n')
        self.release()
        deadline = time.monotonic() + 5
        while protocol.properties()['MainPID'] != '0' and time.monotonic() < deadline:
            time.sleep(0.02)
        with self.assertRaisesRegex(ValueError, 'record mismatch'):
            protocol.recover()
        self.assertFalse((self.root / ('health_' + self.job + '.txt')).exists())

    def test_uncertain_manager_dispatch_is_cancelled_not_inferred_from_missing_pid(self):
        protocol = health.RuntimeHandoff(self.root, self.job, self.token)
        command = protocol.command
        def uncertain(*args):
            result = command(*args)
            if args[0] == '/usr/bin/systemd-run':
                raise OSError('injected lost launch response')
            return result
        with patch.object(protocol, 'command', side_effect=uncertain):
            with self.assertRaisesRegex(OSError, 'lost launch response'):
                protocol.launch(self.entry / 'offline-runtime-upgrade.py', self.lease)
        self.release()
        protocol.recover()
        self.assertEqual(protocol.read()['phase'], 'CANCELLED')
        with self.assertRaisesRegex(ValueError, 'cancelled or stale'):
            protocol.own()

    def test_missing_record_with_manager_owner_cannot_create_unchanged_result(self):
        protocol = health.RuntimeHandoff(self.root, self.job, self.token)
        self.release()
        props = protocol.properties()
        props.update(LoadState='loaded', MainPID='1', ActiveState='active')
        with patch.object(protocol, 'properties', return_value=props):
            with self.assertRaisesRegex(ValueError, 'manager owner exists'):
                protocol.recover()
        self.assertFalse(protocol.path.exists())
        self.assertFalse((self.root / ('health_' + self.job + '.txt')).exists())

    def test_partial_health_is_not_reused_as_successful_cancellation(self):
        protocol = self.launch()
        self.release()
        self.wait_phase(protocol, 'OWNER_BLOCKED')
        (self.root / ('health_' + self.job + '.txt')).write_bytes(b'{broken')
        with self.assertRaisesRegex(ValueError, 'incomplete'):
            protocol.cancel()

    def test_stream_hash_uses_bounded_reads_and_exact_size(self):
        protocol = health.RuntimeHandoff(self.root, self.job, self.token)
        path = Path(self.intent['artifactPath'])
        path.write_bytes(b'x' * (3 * 65536 + 17))
        original = os.read
        sizes = []
        def bounded(fd, count):
            sizes.append(count)
            return original(fd, count)
        with patch.object(health.os, 'read', side_effect=bounded):
            self.assertEqual(protocol.artifact_hash(path, path.stat().st_size), hashlib.sha256(path.read_bytes()).hexdigest())
        self.assertLessEqual(max(sizes), 65536)
        with self.assertRaisesRegex(ValueError, 'size mismatch'):
            protocol.artifact_hash(path, path.stat().st_size - 1)

    def test_missing_record_requires_original_lease_then_creates_dispatch_tombstone(self):
        protocol = health.RuntimeHandoff(self.root, self.job, self.token)
        with self.assertRaises(BlockingIOError):
            protocol.recover()
        self.assertFalse(protocol.path.exists())
        self.release()
        protocol.recover()
        self.assertEqual(protocol.read()['phase'], 'CANCELLED')
        fcntl.flock(self.lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
        with self.assertRaisesRegex(ValueError, 'already consumed'):
            protocol.launch(self.entry / 'offline-runtime-upgrade.py', self.lease)

    def test_missing_record_with_replaced_lease_cannot_prove_no_donor(self):
        protocol = health.RuntimeHandoff(self.root, self.job, self.token)
        path = self.root / ('ota_job_' + self.job + '.lock')
        path.rename(path.with_suffix('.old-lock'))
        path.touch(mode=0o600)
        with self.assertRaisesRegex(ValueError, 'lease inode changed'):
            protocol.recover()
        self.assertFalse(protocol.path.exists())

    def test_stage_copy_failure_before_record_is_recoverable_not_absence_proof(self):
        protocol = health.RuntimeHandoff(self.root, self.job, self.token)
        (self.entry / 'runtime-upgrade-guard.py').unlink()
        with self.assertRaises(FileNotFoundError):
            protocol.launch(self.entry / 'offline-runtime-upgrade.py', self.lease)
        with self.assertRaises(BlockingIOError):
            protocol.recover()
        self.release()
        protocol.recover()
        self.assertEqual(protocol.read()['phase'], 'CANCELLED')

    def test_donor_process_death_after_ready_does_not_finalize_before_owner(self):
        os.close(self.lease)
        self.lease = -1
        code = """import importlib.util,os,fcntl,sys,time
from pathlib import Path
s=importlib.util.spec_from_file_location('health',sys.argv[1]);m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
root=Path(sys.argv[2]);fd=os.open(str(root/('ota_job_'+sys.argv[3]+'.lock')),os.O_RDWR)
fcntl.flock(fd,fcntl.LOCK_EX);m.RuntimeHandoff(root,sys.argv[3],sys.argv[4]).launch(Path(sys.argv[5]),fd)
time.sleep(60)
"""
        donor = subprocess.Popen(['/usr/bin/python3', '-B', '-c', code, str(self.entry / 'ota-health-result.py'),
                                  str(self.root), self.job, self.token, str(self.entry / 'offline-runtime-upgrade.py')])
        protocol = health.RuntimeHandoff(self.root, self.job, self.token)
        try:
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if protocol.path.exists() and protocol.read()['phase'] == 'OWNER_READY':
                    break
                time.sleep(0.02)
            self.assertEqual(protocol.read()['phase'], 'OWNER_READY')
            self.assertFalse((self.root / ('health_' + self.job + '.txt')).exists())
            donor.kill()
            donor.wait(timeout=5)
            self.wait_phase(protocol, 'OWNER_BLOCKED')
            protocol.recover()
        finally:
            if donor.poll() is None:
                donor.kill()
                donor.wait(timeout=5)

    def test_ready_is_durable_before_donor_release_and_no_runtime_write(self):
        self.assertEqual(os.geteuid(), 0, 'run in local isolated root WSL')
        protocol = health.RuntimeHandoff(self.root, self.job, self.token)
        protocol.launch(self.entry / 'offline-runtime-upgrade.py', self.lease)
        ready = protocol.read()
        self.assertEqual(ready['phase'], 'OWNER_READY')
        self.assertGreater(ready['owner']['pid'], 0)
        self.assertEqual(ready['owner']['cgroup'], ready['unitControlGroup'])
        self.assertFalse((self.root / ('health_' + self.job + '.txt')).exists())
        fcntl.flock(self.lease, fcntl.LOCK_UN)
        deadline = time.monotonic() + 10
        while protocol.read()['phase'] != 'OWNER_BLOCKED' and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertEqual(protocol.read()['phase'], 'OWNER_BLOCKED')
        self.assertIn('production runtime baseline', protocol.read()['reason'])
        protocol.cancel()
        self.assertEqual(protocol.read()['phase'], 'CANCELLED')
        self.assertEqual(json.loads((self.root / ('health_' + self.job + '.txt')).read_text())['rollback'], 'unchanged')
        self.assertEqual(sorted(p.name for p in self.entry.iterdir()),
                         ['offline-runtime-upgrade.py', 'ota-health-result.py', 'runtime-upgrade-guard.py'])

    @unittest.skipUnless(os.environ.get('OTA_HANDOFF_BINARY'), 'build the narrow native test to exercise C++')
    def test_cpp_execute_signed_handoff_and_durable_cancelled_final(self):
        root = self.home / 'native/ota'
        root.mkdir(parents=True, mode=0o700)
        self.baseline(root.parent)
        private, public = self.root / 'private.pem', self.root / 'public.pem'
        subprocess.run(['openssl', 'genpkey', '-algorithm', 'RSA', '-pkeyopt', 'rsa_keygen_bits:2048',
                        '-out', str(private)], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        subprocess.run(['openssl', 'pkey', '-in', str(private), '-pubout', '-out', str(public)],
                       check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        descriptor = self.root / 'descriptor'
        descriptor.write_text('edge-ota-v1\njobId=HANDOFF_NATIVE\nmachineCode=LOCAL_ONLY\npackageType=full\n'
                              'version=1.0\nsha256=' + self.intent['artifactSha256'] + '\nsize=' + self.intent['artifactSize'] + '\n')
        signature = self.root / 'signature'
        subprocess.run(['openssl', 'dgst', '-sha256', '-sign', str(private), '-out', str(signature), str(descriptor)], check=True)
        import base64
        result = subprocess.run([os.environ['OTA_HANDOFF_BINARY'], str(root), str(self.entry / 'offline-runtime-upgrade.py'),
                                 self.intent['artifactPath'], self.intent['artifactSha256'],
                                 base64.b64encode(signature.read_bytes()).decode(), str(public), self.intent['artifactSize']],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
