"""Private index files; Windows substitutes POSIX metadata, not publication logic.

Linux root additionally tests kernel flock/fork and the real mapped boot entry.
Neither environment executes native pointctl, systemd, networking or business SHM.
"""
import ast
import contextlib
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import tempfile
import time
import types
import unittest
import uuid
from unittest import mock

REPO = Path(__file__).resolve().parents[1]
SOURCE = REPO / 'deploy/ota-health-result.py'


def isolated_handoff():
    tree = ast.parse(SOURCE.read_bytes())
    body = [node for node in tree.body if isinstance(node, ast.ClassDef) and node.name == 'RuntimeHandoff']
    assert len(body) == 1
    functions = [node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == 'sync_dir']
    ns = dict(contextlib=contextlib, hashlib=hashlib, json=json, os=os, Path=Path, re=re,
              stat=stat, subprocess=subprocess, time=time, importlib=__import__('importlib'), uuid=uuid)
    if os.name == 'posix':
        import fcntl
        ns['fcntl'] = fcntl
    exec(compile(ast.Module(body=functions + body, type_ignores=[]), str(SOURCE), 'exec'), ns)
    metadata = {}
    if os.name != 'posix':
        # Explicit platform boundary: Windows cannot prove root modes, nofollow,
        # directory fsync or flock. Actual writer/rename/file bytes remain real.
        native_open = os.open
        def opened(path, flags, mode=0o777):
            fd = native_open(path, flags, mode)
            metadata[fd] = Path(path)
            return fd
        def chmod(fd, mode):
            os.chmod(metadata[fd], mode)
        def info(fd):
            value = os.fstat(fd)
            mode = 0o400 if stat.S_IMODE(value.st_mode) == 0o444 else 0o600
            return types.SimpleNamespace(st_mode=stat.S_IFREG | mode, st_uid=0, st_nlink=value.st_nlink,
                                         st_size=value.st_size, st_dev=value.st_dev, st_ino=value.st_ino)
        api = types.SimpleNamespace(**{name: getattr(os, name) for name in dir(os)})
        api.open, api.fstat, api.fchmod = opened, info, chmod
        api.O_NOFOLLOW = api.O_CLOEXEC = 0
        api.geteuid = lambda: 0
        ns['os'] = api
        ns['stat'] = types.SimpleNamespace(**{name: getattr(stat, name) for name in dir(stat)})
        ns['stat'].S_IMODE = lambda mode: {0o777: 0o700, 0o666: 0o600, 0o444: 0o400}.get(stat.S_IMODE(mode), stat.S_IMODE(mode))
        ns['sync_dir'] = lambda path: None
        def private_init(self, staging, job, token):
            self.root, self.job, self.token = Path(staging), job, token
            if not re.fullmatch(r'[A-Za-z0-9._-]{1,128}', job) or '..' in job or not re.fullmatch(r'[0-9a-f-]{36}', token):
                raise ValueError('invalid private identity')
            self.path = self.root / ('ota_handoff_' + job + '.json')
            self.intent_path = self.root / ('ota_job_' + job + '.json')
            self.unit = 'ota-runtime-' + job + '-' + token + '.service'
        ns['RuntimeHandoff'].__init__ = private_init
    return ns


class IndexPublicationTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='production-index-')
        self.root = Path(self.temp.name)
        self.root.chmod(0o700)
        self.ns = isolated_handoff()
        self.cls = self.ns['RuntimeHandoff']
        self.owner = self.cls(self.root, 'PRIVATE_JOB', '12345678-1234-4234-8234-123456789abc')
        self.addCleanup(self.cleanup)
        self.record = dict(phase='OWNER_OWNED', intentSha256='a' * 64)

    def cleanup(self):
        for path in self.root.rglob('*'):
            if path.is_file() and not path.is_symlink():
                path.chmod(0o600)
        self.temp.cleanup()

    def prepare(self, operation):
        if operation == 'index_unfinished':
            return self.root / 'runtime-unfinished' / (self.owner.job + '.json')
        self.owner.index_unfinished(self.record)
        self.record['phase'] = 'OWNER_FINISHED'
        self.owner.path.write_bytes((json.dumps(self.record) + '\n').encode())
        self.owner.path.chmod(0o600)
        health = self.root / ('health_' + self.owner.job + '.txt')
        health.write_bytes((json.dumps(dict(jobId=self.owner.job, token=self.owner.token)) + '\n').encode())
        health.chmod(0o600)
        return self.root / 'runtime-retired' / (self.owner.job + '-' + self.owner.token + '.json')

    def temporary(self, target):
        return target.with_name('.' + target.name + '.' + self.owner.token + '.publish')

    def interrupted(self, operation, event):
        target = self.prepare(operation)
        before = self.owner.path.read_bytes() if self.owner.path.exists() else None
        native_fdopen = self.ns['os'].fdopen
        native_rename = self.ns['os'].rename
        fired = []
        file_fsync_identity = []
        directory_fsyncs = []
        class Writer:
            def __init__(self, fd, mode): self.stream = native_fdopen(fd, mode)
            def __enter__(self): return self
            def __exit__(self, *args): self.stream.close()
            def write(self, data):
                if event == 'short-write' and not fired:
                    fired.append(event)
                    self.stream.write(data[:7]); self.stream.flush()
                    raise OSError('injected short write')
                return self.stream.write(data)
            def flush(self): return self.stream.flush()
            def fileno(self): return self.stream.fileno()
        def fsync(fd):
            info = os.fstat(fd)
            if stat.S_ISDIR(info.st_mode): directory_fsyncs.append((info.st_dev, info.st_ino))
            temporary = self.temporary(target)
            if event == 'file-fsync' and not fired and stat.S_ISREG(info.st_mode) and temporary.exists() and \
                    (info.st_dev, info.st_ino) == (temporary.stat().st_dev, temporary.stat().st_ino):
                file_fsync_identity.append((info.st_dev, info.st_ino))
                fired.append(event)
                raise OSError('injected file fsync')
            return os.fsync(fd)
        def rename(source, destination):
            if Path(destination) == target and event == 'rename' and not fired:
                fired.append(event)
                raise OSError('injected rename')
            return native_rename(source, destination)
        native_sync = self.ns['sync_dir']
        def sync(path):
            if Path(path) == target.parent and target.exists() and event == 'directory-fsync' and not fired:
                fired.append(event)
                raise OSError('injected directory fsync')
            return native_sync(path)
        # Namespace-local os proxy avoids patching the test's own os primitives.
        api = types.SimpleNamespace(**{name: getattr(self.ns['os'], name) for name in dir(self.ns['os']) if not name.startswith('__')})
        api.fdopen, api.fsync, api.rename = Writer, fsync, rename
        with mock.patch.dict(self.ns, os=api, sync_dir=sync):
            with self.assertRaises(OSError): getattr(self.owner, operation)(self.record)
        self.assertEqual(fired, [event])
        if event == 'file-fsync':
            temporary = self.temporary(target)
            self.assertEqual(file_fsync_identity, [(temporary.stat().st_dev, temporary.stat().st_ino)])
            if os.name == 'posix':
                self.assertTrue(directory_fsyncs, 'real parent-directory fsync must pass before the target-file fault')
        marker = self.root / 'runtime-unfinished' / (self.owner.job + '.json')
        if operation == 'retire_index': self.assertTrue(marker.exists(), 'pending must survive failed receipt publication')
        if event != 'directory-fsync': self.assertFalse(target.exists(), 'no partial final JSON')
        synced = []
        def repaired(path):
            synced.append(Path(path))
            return native_sync(path)
        with mock.patch.dict(self.ns, sync_dir=repaired):
            getattr(self.owner, operation)(self.record)
        self.assertIn(target.parent, synced, 'correct existing final must repair its directory durability')
        self.assertIsInstance(json.loads(target.read_bytes()), dict)
        self.assertFalse(self.temporary(target).exists())
        if operation == 'retire_index':
            self.assertFalse(marker.exists())
            self.assertEqual(self.owner.path.read_bytes(), before, 'terminal journal retained byte-for-byte')
        archives = list((self.root / 'runtime-interrupted-index').glob('*')) if (self.root / 'runtime-interrupted-index').exists() else []
        self.assertEqual(len(archives), 0 if event == 'directory-fsync' else 1)
        if event == 'short-write': self.assertEqual(archives[0].stat().st_size, 7)

    def test_unfinished_short_write(self): self.interrupted('index_unfinished', 'short-write')
    def test_unfinished_file_fsync(self): self.interrupted('index_unfinished', 'file-fsync')
    def test_unfinished_rename(self): self.interrupted('index_unfinished', 'rename')
    def test_unfinished_directory_fsync(self): self.interrupted('index_unfinished', 'directory-fsync')
    def test_retired_short_write(self): self.interrupted('retire_index', 'short-write')
    def test_retired_file_fsync(self): self.interrupted('retire_index', 'file-fsync')
    def test_retired_rename(self): self.interrupted('retire_index', 'rename')
    def test_retired_directory_fsync(self): self.interrupted('retire_index', 'directory-fsync')

    def test_mkdir_root_fsync_retry_repairs_parent(self):
        native = self.ns['sync_dir']
        def fail(path):
            if Path(path) == self.root: raise OSError('injected mkdir parent fsync')
            return native(path)
        with mock.patch.dict(self.ns, sync_dir=fail):
            with self.assertRaises(OSError): self.owner.index_unfinished(self.record)
        self.assertTrue((self.root / 'runtime-unfinished').is_dir())
        synced = []
        with mock.patch.dict(self.ns, sync_dir=lambda path: synced.append(Path(path))):
            self.owner.index_unfinished(self.record)
        self.assertIn(self.root, synced)

    def test_unknown_partial_is_not_deleted_or_accepted(self):
        target = self.prepare('index_unfinished')
        self.owner.index_directory('runtime-unfinished', create=True)
        partial = self.temporary(target)
        partial.write_bytes(b'not-original-job'); partial.chmod(0o600)
        with self.assertRaisesRegex(ValueError, 'bytes differ'):
            self.owner.index_unfinished(self.record)
        self.assertEqual(partial.read_bytes(), b'not-original-job')
        self.assertFalse(target.exists())

    def test_existing_final_drift_is_not_overwritten(self):
        target = self.prepare('index_unfinished')
        self.owner.index_unfinished(self.record)
        target.write_bytes(b'{"different":true}\n')
        with self.assertRaises(ValueError): self.owner.index_unfinished(self.record)
        self.assertEqual(target.read_bytes(), b'{"different":true}\n')

    def indexed_owner(self, job):
        owner = self.cls(self.root, job, self.owner.token)
        value = dict(format='ota-job-v1', lifecycle='systemd-handoff-v1', jobId=job, token=owner.token,
                     version='1.1', machineCode='PRIVATE_NODE', artifactSha256='b' * 64, artifactSize='1',
                     leaseDev='1', leaseInode='2')
        owner.intent_path.write_bytes((json.dumps(value) + '\n').encode()); owner.intent_path.chmod(0o600)
        owner.index_unfinished(dict(intentSha256=hashlib.sha256(owner.intent_path.read_bytes()).hexdigest()))
        return owner

    def test_completed_history_is_not_scanned_or_deleted(self):
        owner = self.indexed_owner('LIVE')
        for i in range(129):
            (self.root / ('ota_handoff_OLD_%d.json' % i)).write_bytes(b'not read as an unfinished pointer')
        self.assertEqual([p.job for p in self.cls.unfinished(self.root)], [owner.job])
        self.assertEqual(len(list(self.root.glob('ota_handoff_OLD_*.json'))), 129)

    def test_128_unfinished_allowed_129_rejected(self):
        for i in range(128): self.indexed_owner('JOB_%d' % i)
        self.assertEqual(len(self.cls.unfinished(self.root)), 128)
        owner = self.cls(self.root, 'OVER_LIMIT', self.owner.token)
        with self.assertRaisesRegex(ValueError, 'count exceeds bound'):
            owner.index_unfinished(self.record)

    def test_interrupted_pointer_is_bound_to_original_intent(self):
        owner = self.indexed_owner('LIVE')
        target = self.root / 'runtime-unfinished/LIVE.json'
        raw = target.read_bytes(); target.unlink()
        partial = target.with_name('.' + target.name + '.' + owner.token + '.publish')
        partial.write_bytes(raw[:7]); partial.chmod(0o600)
        self.assertEqual([p.job for p in self.cls.unfinished(self.root)], ['LIVE'])
        owner.index_unfinished(dict(intentSha256=hashlib.sha256(owner.intent_path.read_bytes()).hexdigest()))
        self.assertEqual(target.read_bytes(), raw)

    def test_partial_128th_pointer_can_repair_without_counting_it_twice(self):
        for i in range(128): self.indexed_owner('JOB_%d' % i)
        owner = self.cls(self.root, 'JOB_127', self.owner.token)
        target = self.root / 'runtime-unfinished/JOB_127.json'
        raw = target.read_bytes(); target.unlink()
        partial = target.with_name('.' + target.name + '.' + owner.token + '.publish')
        partial.write_bytes(raw[:7]); partial.chmod(0o600)
        owner.index_unfinished(dict(intentSha256=hashlib.sha256(owner.intent_path.read_bytes()).hexdigest()))
        self.assertEqual(target.read_bytes(), raw)
        self.assertEqual(len(self.cls.unfinished(self.root)), 128)

    def test_retirement_receipt_sync_succeeds_before_pending_unlink(self):
        target = self.prepare('retire_index')
        marker = self.root / 'runtime-unfinished' / (self.owner.job + '.json')
        native_sync = self.ns['sync_dir']
        seen = []
        def sync(path):
            if Path(path) == target.parent:
                self.assertTrue(marker.exists())
                self.assertTrue(target.exists())
                seen.append('receipt durable before pending deletion')
            return native_sync(path)
        with mock.patch.dict(self.ns, sync_dir=sync): self.owner.retire_index(self.record)
        self.assertEqual(seen, ['receipt durable before pending deletion'])
        self.assertFalse(marker.exists())

    def test_pending_unlink_sync_retry_repairs_directory(self):
        self.prepare('retire_index')
        directory = self.root / 'runtime-unfinished'
        marker = directory / (self.owner.job + '.json')
        native_sync = self.ns['sync_dir']
        def fail(path):
            if Path(path) == directory and not marker.exists(): raise OSError('injected unlink directory fsync')
            return native_sync(path)
        with mock.patch.dict(self.ns, sync_dir=fail):
            with self.assertRaises(OSError): self.owner.retire_index(self.record)
        self.assertFalse(marker.exists())
        synced = []
        with mock.patch.dict(self.ns, sync_dir=lambda path: synced.append(Path(path))):
            self.owner.retire_index(self.record)
        self.assertIn(directory, synced)

    def test_unknown_index_entry_or_wrong_token_refuses_without_deleting(self):
        owner = self.indexed_owner('LIVE')
        directory = self.root / 'runtime-unfinished'
        extra = directory / 'unknown-entry'
        extra.write_bytes(b'unknown')
        with self.assertRaisesRegex(ValueError, 'unknown'): self.cls.unfinished(self.root)
        self.assertTrue(extra.exists()); extra.unlink()
        partial = directory / ('.LIVE.json.' + 'b' * 36 + '.publish')
        partial.write_bytes(b'unknown')
        with self.assertRaisesRegex(ValueError, 'token drift'): self.cls.unfinished(self.root)
        self.assertTrue(partial.exists())

    def test_archive_rename_then_directory_fsync_failure_repairs_existing_archive(self):
        owner = self.indexed_owner('LIVE')
        target = self.root / 'runtime-unfinished/LIVE.json'
        raw = target.read_bytes(); target.unlink()
        partial = target.with_name('.' + target.name + '.' + owner.token + '.publish')
        partial.write_bytes(raw[:7]); partial.chmod(0o600)
        retained = self.root / 'runtime-interrupted-index'
        native_sync = self.ns['sync_dir']
        record = dict(intentSha256=hashlib.sha256(owner.intent_path.read_bytes()).hexdigest())
        def fail(path):
            if Path(path) == retained: raise OSError('injected archive directory fsync')
            return native_sync(path)
        with mock.patch.dict(self.ns, sync_dir=fail):
            with self.assertRaises(OSError): owner.index_unfinished(record)
        archives = list(retained.iterdir())
        self.assertEqual(len(archives), 1); self.assertEqual(archives[0].read_bytes(), raw[:7])
        self.assertFalse(partial.exists()); self.assertFalse(target.exists())
        synced = []
        with mock.patch.dict(self.ns, sync_dir=lambda path: synced.append(Path(path))): owner.index_unfinished(record)
        self.assertIn(retained, synced)
        self.assertEqual(target.read_bytes(), raw)
        self.assertEqual(list(retained.iterdir()), archives)

    @unittest.skipUnless(os.name == 'posix' and hasattr(os, 'fork'), 'requires real Linux process exit')
    def test_process_exit_mid_publication_is_archived_and_repaired(self):
        for operation in ('index_unfinished', 'retire_index'):
            with self.subTest(operation=operation):
                target = self.prepare(operation)
                pid = os.fork()
                if pid == 0:
                    native = self.ns['os'].fdopen
                    class ExitWriter:
                        def __init__(self, fd, mode): self.stream = native(fd, mode)
                        def __enter__(self): return self
                        def __exit__(self, *args): self.stream.close()
                        def write(self, data):
                            self.stream.write(data[:7]); self.stream.flush(); os.fsync(self.stream.fileno())
                            os._exit(73)
                    api = types.SimpleNamespace(**{name: getattr(os, name) for name in dir(os)})
                    api.fdopen = ExitWriter
                    self.ns['os'] = api
                    getattr(self.owner, operation)(self.record)
                    os._exit(74)
                _, status = os.waitpid(pid, 0)
                self.assertTrue(os.WIFEXITED(status)); self.assertEqual(os.WEXITSTATUS(status), 73)
                self.assertFalse(target.exists())
                getattr(self.owner, operation)(self.record)
                self.assertIsInstance(json.loads(target.read_bytes()), dict)
                if operation == 'index_unfinished': target.unlink()


class BootQuiescenceTest(unittest.TestCase):
    """Real scanner/private paths. Approval/boot lock/proof and Windows mode are substitutes."""
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='production-quiescence-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        def mapped(*parts):
            text = '/'.join(str(p).replace('\\', '/') for p in parts)
            if text == '/proc' or text.startswith('/proc/') or text == '/dev/shm' or text.startswith('/dev/shm/'):
                return self.root / 'system' / text.lstrip('/')
            return Path(*parts)
        self.path = mapped
        self.shm = mapped('/dev/shm'); self.shm.mkdir(parents=True)
        self.proc = mapped('/proc/101'); self.proc.mkdir(parents=True)
        (self.proc / 'maps').write_text(''); (self.proc / 'fd').mkdir()
        boot = mapped('/proc/sys/kernel/random/boot_id'); boot.parent.mkdir(parents=True); boot.write_text('NEW_BOOT')
        self.baseline = dict(bootId='OLD_BOOT', segments=[dict(name='live_v11', dev=1, ino=123)],
                             retiredSources={'retired_v10': dict(dev=1, ino=124)})
        self.approved = json.loads(json.dumps(self.baseline))
        self.home = self.root / 'home'
        self.intent = self.home / 'data/runtime-production/boots/NEW_BOOT-intent.json'
        self.rows = []
        self.locked = True
        self.readlinks = {}
        def require(condition, message):
            if not condition: raise ValueError(message)
        def schema(document, home): require(document == self.approved and home == self.home, 'unapproved role inventory')
        def abi(path, version):
            require(version == 11 and path.stat().st_size == 16 and path.read_bytes()[:8] == struct.pack('<II', 0x4d505354, 11), 'wrong ABI11')
        api = types.SimpleNamespace(**{name: getattr(os, name) for name in dir(os)})
        api.readlink = lambda path: self.readlinks[str(path)] if str(path) in self.readlinks else os.readlink(path)
        modes = stat
        if os.name != 'posix':
            api.major, api.minor = lambda device: device, lambda device: 0
            modes = types.SimpleNamespace(**{name: getattr(stat, name) for name in dir(stat)})
            modes.S_IMODE = lambda value: 0o600 if stat.S_IMODE(value) == 0o666 else stat.S_IMODE(value)
        tree = ast.parse((REPO / 'deploy/offline-runtime-upgrade.py').read_bytes())
        functions = [n for n in tree.body if isinstance(n, ast.FunctionDef) and
                     n.name in ('production_no_mappings', 'no_mappings', 'regular')]
        self.ns = dict(Path=mapped, os=api, stat=modes, struct=struct, re=re, require=require,
                       production_owner_proof=lambda: {}, production_locked_owner=lambda value: self.locked,
                       guard=types.SimpleNamespace(PRODUCTION_DIR='runtime-production', production_schema=schema,
                           production_segment_identities=lambda *args: self.rows,
                           production_boot_record=lambda home, baseline, path, intent: json.loads(path.read_bytes()),
                           point_store_abi=abi, POINT_STORE_SIZES={11:16}))
        exec(compile(ast.Module(body=functions, type_ignores=[]), str(REPO / 'deploy/offline-runtime-upgrade.py'), 'exec'), self.ns)

    def check(self, creating=True): self.ns['production_no_mappings'](self.home, self.baseline, creating=creating)

    def target(self, completed=True):
        path = self.shm / 'live_v11'; path.write_bytes(struct.pack('<II', 0x4d505354, 11) + b'\0' * 8); path.chmod(0o600)
        info = path.stat(); row = dict(name='live_v11', dev=info.st_dev, ino=info.st_ino)
        self.rows = [row]
        self.intent.parent.mkdir(parents=True, exist_ok=True)
        self.intent.write_text(json.dumps(dict(segments=[row] if completed else [], inProgress=None if completed else 'live_v11')))
        return path

    def snapshot(self):
        return {str(p.relative_to(self.root)): (p.stat().st_ino, p.read_bytes()) for p in self.root.rglob('*') if p.is_file()}

    def test_approved_missing_new_boot_roles_scan_without_mutation(self):
        before = self.snapshot(); self.check(); self.assertEqual(self.snapshot(), before)
        self.assertFalse(self.shm.joinpath('live_v11').exists()); self.assertFalse(self.intent.exists())

    def test_same_boot_and_legacy_missing_roles_remain_strict(self):
        self.path('/proc/sys/kernel/random/boot_id').write_text('OLD_BOOT')
        with self.assertRaisesRegex(ValueError, 'single-link regular'): self.check()
        with self.assertRaisesRegex(ValueError, 'single-link regular'):
            self.ns['no_mappings'](['live_v11'], include_fds=True)

    def test_unowned_target_and_recreated_retired_source_refuse(self):
        p = self.shm / 'live_v11'; p.write_bytes(b''); p.chmod(0o600)
        with self.assertRaisesRegex(ValueError, 'unowned'): self.check()
        p.unlink(); (self.shm / 'retired_v10').write_bytes(b'old')
        with self.assertRaisesRegex(ValueError, 'retired source recreated'): self.check()

    def test_owned_progress_and_complete_roles_validate_identity_and_abi(self):
        p = self.target(completed=False); before = self.snapshot(); self.check(); self.assertEqual(self.snapshot(), before)
        self.target(); self.check(creating=False)
        self.rows[0]['ino'] += 1
        with self.assertRaisesRegex(ValueError, 'identity drift'): self.check(creating=False)
        self.rows[0]['ino'] -= 1; p.write_bytes(b'x' * 16)
        with self.assertRaisesRegex(ValueError, 'ABI11'): self.check(creating=False)

    def test_existing_symlink_hardlink_and_wrong_mode_fail_closed(self):
        p = self.target()
        original = Path.is_symlink
        with mock.patch.object(Path, 'is_symlink', lambda path: path == p or original(path)):
            with self.assertRaisesRegex(ValueError, 'symlink'): self.check()
        alias = self.shm / 'alias'; os.link(p, alias)
        with self.assertRaisesRegex(ValueError, 'single-link'): self.check()
        alias.unlink(); p.chmod(0o400)
        try:
            with self.assertRaisesRegex(ValueError, 'ownership/type'): self.check()
        finally: p.chmod(0o600)

    def test_deleted_target_and_retired_name_mappings_refuse_when_paths_absent(self):
        for name in ('live_v11', 'retired_v10'):
            (self.proc / 'maps').write_text('1000-2000 rw-s 00000000 00:01 999 /dev/shm/' + name + ' (deleted)\n')
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'still mapped'): self.check()

    def test_deleted_fd_name_refuses_even_if_fd_stat_would_fail(self):
        fd = self.proc / 'fd/3'; fd.write_bytes(b'not a real proc fd')
        self.readlinks[str(fd)] = '/dev/shm/live_v11 (deleted)'
        original_stat, stat_attempts = Path.stat, []
        def missing(path, *args, **kwargs):
            if path == fd:
                stat_attempts.append(path)
                raise FileNotFoundError('private fd stat would fail')
            return original_stat(path, *args, **kwargs)
        with mock.patch.object(Path, 'stat', missing):
            with self.assertRaisesRegex(ValueError, 'still open'): self.check()
        self.assertEqual(stat_attempts, [], 'protected deleted name must reject before a disappearing fd stat')

    def test_alias_mapping_is_rejected_by_actual_identity(self):
        p = self.target(); s = p.stat()
        (self.proc / 'maps').write_text('1000-2000 rw-s 00000000 %x:%x %d /alias/not-the-shm-name\n' %
                                      (self.ns['os'].major(s.st_dev), self.ns['os'].minor(s.st_dev), s.st_ino))
        with self.assertRaisesRegex(ValueError, 'still mapped'): self.check()
        (self.proc / 'maps').write_text('')
        fd = self.proc / 'fd/3'; fd.write_bytes(b'private descriptor substitute')
        self.readlinks[str(fd)] = '/alias/not-the-shm-name'
        original_stat = Path.stat
        with mock.patch.object(Path, 'stat', lambda path, *a, **kw: s if path == fd else original_stat(path, *a, **kw)):
            with self.assertRaisesRegex(ValueError, 'still open'): self.check()

    def test_unrelated_new_object_reusing_historical_tuple_is_not_a_holder(self):
        old = self.baseline['segments'][0]
        (self.proc / 'maps').write_text('1000-2000 rw-s 00000000 %x:%x %d /unrelated/new-boot-object\n' %
                                      (self.ns['os'].major(old['dev']), self.ns['os'].minor(old['dev']), old['ino']))
        before = self.snapshot(); self.check(); self.assertEqual(self.snapshot(), before)

    def test_permission_unknown_visibility_and_malformed_maps_fail_closed(self):
        original_lstat = Path.lstat
        def denied_name(path, *args, **kwargs):
            if path == self.shm / 'live_v11': raise PermissionError('unknown role presence')
            return original_lstat(path, *args, **kwargs)
        with mock.patch.object(Path, 'lstat', denied_name):
            with self.assertRaises(PermissionError): self.check()
        original_open = Path.open
        def denied(path, *args, **kwargs):
            if path == self.proc / 'maps': raise PermissionError('unknown maps permission')
            return original_open(path, *args, **kwargs)
        with mock.patch.object(Path, 'open', denied):
            with self.assertRaises(PermissionError): self.check()
        (self.proc / 'maps').unlink()
        with self.assertRaisesRegex(ValueError, 'visibility unknown'): self.check()
        (self.proc / 'maps').write_text('unparseable\n')
        with self.assertRaisesRegex(ValueError, 'unparseable'): self.check()
        (self.proc / 'maps').write_text('')
        fd = self.proc / 'fd/3'; fd.write_bytes(b'private descriptor substitute')
        with mock.patch.object(self.ns['os'], 'readlink', side_effect=PermissionError('unknown fd permission')):
            with self.assertRaises(PermissionError): self.check()
        fd.unlink(); (self.proc / 'fd').rmdir()
        with self.assertRaisesRegex(ValueError, 'visibility unknown'): self.check()

    def test_lock_and_approval_required_and_completed_role_cannot_vanish(self):
        self.locked = False
        with self.assertRaisesRegex(ValueError, 'lock ownership'): self.check()
        self.locked = True; self.baseline['segments'][0]['name'] = 'unapproved'
        with self.assertRaisesRegex(ValueError, 'unapproved role'): self.check()
        self.baseline = json.loads(json.dumps(self.approved)); p = self.target(); p.unlink()
        with self.assertRaisesRegex(ValueError, 'completed native boot target vanished'): self.check(creating=False)


class FixtureShowContractTest(unittest.TestCase):
    """Execute the real fixture adapter without importing Linux-only lifecycle code."""
    def setUp(self):
        source = REPO / 'tools/production_runtime_baseline_test.py'
        tree = ast.parse(source.read_bytes())
        cls = next(n for n in tree.body if isinstance(n, ast.ClassDef) and n.name == 'ProductionRuntimeTest')
        methods = [n for n in cls.body if isinstance(n, ast.FunctionDef) and n.name in ('systemctl', 'property')]
        ns = dict(shutil=shutil)
        exec(compile(ast.Module(body=methods, type_ignores=[]), str(source), 'exec'), ns)
        monitor = 'system-monitor@monitor-service.service'
        self.f = types.SimpleNamespace(calls=[], units=['gateway-services.service', monitor], live=set(),
                                       pid='PRIVATE_PID', g=types.SimpleNamespace(MONITOR_UNIT=monitor))
        self.f.property = types.MethodType(ns['property'], self.f)
        self.f.systemctl = types.MethodType(ns['systemctl'], self.f)

    def test_scalar_show_value_tracks_actual_fixture_unit_state(self):
        f = self.f
        self.assertEqual(f.systemctl('show', '--property=ActiveState', '--value', f.units[0]), 'inactive')
        f.live.add(f.units[0])
        self.assertEqual(f.systemctl('show', '--property=ActiveState', '--value', f.units[0]), 'active')

    def test_batch_show_preserves_fields_and_unknown_grammar_refuses(self):
        f = self.f
        expected = '\n\n'.join('Id=%s\nActiveState=inactive\nMainPID=0' % unit for unit in f.units)
        self.assertEqual(f.systemctl('show', '--property=Id,ActiveState,MainPID', *f.units), expected)
        for args in (('show', '--property=ActiveState', '--value', 'foreign.service'),
                     ('show', '--property=ActiveState', '--value', *f.units),
                     ('show', '--property=Environment', '--value', f.units[0])):
            with self.subTest(args=args), self.assertRaises(AssertionError): f.systemctl(*args)


@unittest.skipUnless(os.name == 'posix' and hasattr(os, 'geteuid') and os.geteuid() == 0,
                     'requires Linux root private boot fixture and kernel flock')
class BootLockTest(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.spec_from_file_location('nativeboot_lock_baseline_fixture', REPO / 'tools/production_runtime_baseline_test.py')
        baseline = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(baseline)
        self.fixture = baseline.ProductionRuntimeTest('test_production_entry_exists_without_changing_legacy_actions')
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.f = self.fixture

    def boot_entry(self):
        with mock.patch.object(self.f.m.sys, 'argv', ['offline-runtime-upgrade.py', 'production-boot', str(self.f.home)]):
            self.f.m.main()

    def test_cleared_run_boot_entry_creates_lock_before_native_missing_refusal(self):
        self.f.adopt()
        run = self.f.path('/run')
        shutil.rmtree(run); run.mkdir(mode=0o755)
        self.f.write(self.f.path('/proc/sys/kernel/random/boot_id'), 'NEW_BOOT\n')
        # Native pointctl/proof is deliberately absent here. Lock recreation must
        # succeed, while real baseline validation still refuses startup safely.
        with self.assertRaises((ValueError, FileNotFoundError)): self.boot_entry()
        lock = run / 'gateway-offline-runtime.lock'
        self.assertTrue(lock.is_file()); self.assertEqual(lock.stat().st_size, 0)
        self.assertEqual(stat.S_IMODE(lock.stat().st_mode), 0o600)
        import fcntl
        with lock.open('r+b') as stream: fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)

    def test_same_boot_entry_missing_lock_succeeds_without_service_or_shm_mutation(self):
        self.f.adopt()
        lock = self.f.path('/run/gateway-offline-runtime.lock')
        lock.unlink()
        self.f.calls.clear()
        segments = [self.f.path('/dev/shm') / name for name in ('live_v11', 'retired_v10')]
        def identity(path):
            info = path.stat()
            with path.open('rb') as stream: header = stream.read(8)
            return info.st_dev, info.st_ino, info.st_size, header
        before = [identity(p) for p in segments]
        live = set(self.f.live)
        pointer = self.f.runtime.root / 'current.json'
        pointer_bytes = pointer.read_bytes()
        configs = self.f.m.tree_hashes(self.f.home / 'config/runtime')
        self.boot_entry()
        self.assertTrue(lock.exists())
        inventory = [('systemctl', query, '--all', '--no-legend', '--no-pager', '--plain') + self.f.g.PRODUCTION_UNIT_PATTERNS
                     for query in ('list-units', 'list-unit-files')]
        self.assertEqual(self.f.calls, inventory * 2, 'only exact read-only inventory queries are allowed')
        self.assertEqual(self.f.live, live)
        self.assertEqual([identity(p) for p in segments], before)
        self.assertEqual(pointer.read_bytes(), pointer_bytes)
        self.assertEqual(self.f.m.tree_hashes(self.f.home / 'config/runtime'), configs)

    def test_creator_and_contender_do_not_truncate_or_share_flock(self):
        lock = self.f.path('/run/gateway-offline-runtime.lock')
        lock.unlink()
        with self.f.m.production_lock():
            ino = lock.stat().st_ino
            with self.assertRaises(BlockingIOError):
                with self.f.m.production_lock(): self.fail('contender acquired lock')
            self.assertEqual(lock.stat().st_ino, ino)

    def test_wrong_mode_nonempty_symlink_and_inode_replacement_rejected(self):
        lock = self.f.path('/run/gateway-offline-runtime.lock')
        lock.chmod(0o644)
        with self.assertRaises(ValueError):
            with self.f.m.production_lock(): pass
        lock.chmod(0o600); lock.write_bytes(b'unknown')
        with self.assertRaises(ValueError):
            with self.f.m.production_lock(): pass
        lock.unlink(); lock.symlink_to(self.f.root / 'missing-lock')
        with self.assertRaises(OSError):
            with self.f.m.production_lock(): pass
        lock.unlink(); self.f.write(lock, b'', 0o600)
        original = self.f.m.fcntl.flock
        def replaced(fd, flags):
            original(fd, flags)
            lock.rename(lock.with_name('original-retained'))
            self.f.write(lock, b'', 0o600)
        with mock.patch.object(self.f.m.fcntl, 'flock', replaced):
            with self.assertRaisesRegex(ValueError, 'lock replaced'):
                with self.f.m.production_lock(): pass


if __name__ == '__main__':
    unittest.main()
