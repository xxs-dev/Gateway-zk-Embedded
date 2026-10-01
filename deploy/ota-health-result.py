#!/usr/bin/env python3
"""Persist a job-bound OTA health decision after restart or rollback."""

import contextlib
import hashlib
import json
import fcntl
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import time
import importlib.util
import uuid


def active_job(staging, job, token, cancel=False):
    if not job or not token or any(c not in "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-" for c in job + token):
        raise ValueError("invalid active job identity")
    lock_fd = os.open(os.path.join(staging, "ota_state.lock"), os.O_CREAT | os.O_RDWR, 0o600)
    fcntl.flock(lock_fd, fcntl.LOCK_EX)
    active_path = os.path.join(staging, "ota_active_job.txt")
    expected = ("jobId=" + job + "\ntoken=" + token + "\n").encode("ascii")
    try:
        with open(active_path, "rb") as source:
            current = source.read(257)
    except FileNotFoundError:
        current = b""
    if cancel:
        if current == expected and not os.path.exists(os.path.join(staging, "ota_job_" + job + ".json")):
            os.unlink(active_path)
            sync_dir(staging)
        return
    if current == expected:
        return
    if current:
        lines = current.decode("ascii").splitlines()
        if len(lines) != 2 or not lines[0].startswith("jobId=") or not lines[1].startswith("token="):
            raise ValueError("invalid active job marker")
        previous_job, previous_token = lines[0][6:], lines[1][6:]
        if (not previous_job or not previous_token or
            any(c not in "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-" for c in previous_job + previous_token) or
            current != ("jobId=" + previous_job + "\ntoken=" + previous_token + "\n").encode("ascii")):
            raise ValueError("invalid active job marker")
        final = os.path.join(staging, "ota_final_" + previous_job + ".txt")
        health = os.path.join(staging, "health_" + previous_job + ".txt")
        if os.path.exists(os.path.join(staging, "ota_job_" + previous_job + ".json")):
            with open(os.path.join(staging, "ota_job_" + previous_job + ".json"), encoding="utf-8") as source:
                intent = json.load(source)
            if (intent.get("format") != "ota-job-v1" or intent.get("jobId") != previous_job or
                intent.get("token") != previous_token or not intent.get("version")):
                raise ValueError("previous OTA intent is invalid")
            with open(final, "rb") as source:
                terminal = source.read(257)
            completed = ("jobId=" + previous_job + "\ntoken=" + previous_token + "\nstatus=completed\n").encode("ascii")
            failed = ("jobId=" + previous_job + "\ntoken=" + previous_token + "\nstatus=failed\n").encode("ascii")
            if terminal == failed:
                with open(health, "rb") as source:
                    result = json.loads(source.read(4097))
                if (result.get("jobId") != previous_job or result.get("version") != intent["version"] or
                    result.get("token") != previous_token or
                    result.get("status") != "failed" or result.get("rollback") not in ("succeeded", "unchanged")):
                    raise ValueError("previous OTA rollback is unresolved")
            elif terminal != completed:
                raise ValueError("previous OTA job is unresolved")
        else:
            with open(health, "rb") as source:
                terminal = source.read(4097)
            if len(terminal) > 4096 or not terminal.endswith(b"\n"):
                raise ValueError("previous OTA health is incomplete")
            result = json.loads(terminal)
            if (result.get("jobId") != previous_job or result.get("token") != previous_token or
                not (result.get("status") == "healthy" or
                     (result.get("status") == "failed" and result.get("rollback") == "succeeded"))):
                raise ValueError("previous OTA job is unresolved")
    temporary = active_path + ".tmp." + str(os.getpid())
    try:
        with open(temporary, "xb") as output:
            output.write(expected)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, active_path)
        sync_dir(staging)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def sync_dir(path):
    directory = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        os.fsync(directory)
    finally:
        os.close(directory)


class RuntimeHandoff:
    """Job-local independent owner; mutation requires proved production admission."""
    def __init__(self, staging, job, token):
        self.root = Path(staging)
        if (os.geteuid() != 0 or not self.root.is_absolute() or
                any(p.is_symlink() for p in (self.root,) + tuple(self.root.parents)) or
                not re.fullmatch(r'[A-Za-z0-9._-]{1,128}', job) or '..' in job or
                not re.fullmatch(r'[0-9a-f-]{36}', token)):
            raise ValueError('invalid runtime handoff identity/root')
        info = self.root.stat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022:
            raise ValueError('runtime handoff requires private root-owned staging')
        self.job, self.token = job, token
        self.path = self.root / ('ota_handoff_' + job + '.json')
        self.intent_path = self.root / ('ota_job_' + job + '.json')
        self.unit = 'ota-runtime-' + job + '-' + token + '.service'

    def file_bytes(self, path, limit=16384):
        fd = os.open(str(path), os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
        try:
            s = os.fstat(fd)
            if (not stat.S_ISREG(s.st_mode) or s.st_uid != 0 or s.st_nlink != 1 or
                    s.st_mode & 0o022 or s.st_size > limit):
                raise ValueError('invalid handoff file: ' + path.name)
            data = os.read(fd, limit + 1)
            if len(data) > limit:
                raise ValueError('handoff file too large')
            return data
        finally:
            os.close(fd)

    def json_file(self, path):
        def unique(pairs):
            result = {}
            for key, value in pairs:
                if key in result:
                    raise ValueError('duplicate handoff JSON field')
                result[key] = value
            return result
        raw = self.file_bytes(path)
        if not raw.endswith(b'\n'):
            raise ValueError('incomplete handoff record')
        return json.loads(raw, object_pairs_hook=unique)

    def intent(self):
        value = self.json_file(self.intent_path)
        if (value.get('format') != 'ota-job-v1' or value.get('jobId') != self.job or
                value.get('token') != self.token or value.get('lifecycle') != 'systemd-handoff-v1' or
                not re.fullmatch(r'[A-Za-z0-9._-]{1,128}', value.get('version', '')) or
                not value.get('machineCode') or
                not re.fullmatch(r'[0-9a-f]{64}', value.get('artifactSha256', '')) or
                not re.fullmatch(r'[1-9][0-9]{0,18}', value.get('artifactSize', '')) or
                not re.fullmatch(r'[0-9]{1,20}', value.get('leaseDev', '')) or
                not re.fullmatch(r'[1-9][0-9]{0,19}', value.get('leaseInode', ''))):
            raise ValueError('runtime handoff intent mismatch')
        return value

    def artifact_hash(self, path, size):
        fd = os.open(str(path), os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
        try:
            before = os.fstat(fd)
            if (not stat.S_ISREG(before.st_mode) or before.st_uid != 0 or before.st_nlink != 1 or
                    before.st_mode & 0o022 or before.st_size != size or size > 512 * 1024 * 1024):
                raise ValueError('runtime artifact type/size mismatch')
            digest = hashlib.sha256()
            remaining = size
            while remaining:
                data = os.read(fd, min(65536, remaining))
                if not data:
                    raise ValueError('runtime artifact truncated')
                digest.update(data)
                remaining -= len(data)
            after = os.fstat(fd)
            named = path.lstat()
            signature = lambda s: (s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns)
            if signature(before) != signature(after) or signature(before) != signature(named):
                raise ValueError('runtime artifact changed during hash')
            return digest.hexdigest()
        finally:
            os.close(fd)

    def verify_inputs(self, record):
        intent = self.intent()
        if self.file_bytes(self.root / 'ota_active_job.txt', 256) != (
                'jobId=' + self.job + '\ntoken=' + self.token + '\n').encode('ascii'):
            raise ValueError('runtime active job changed')
        stage = Path(record['entry'])
        names = {'offline-runtime-upgrade.py', 'runtime-upgrade-guard.py', 'ota-health-result.py'}
        if (stage != self.root / ('runtime-entry-' + self.job) or stage.is_symlink() or
                set(record['pins']) != names or {p.name for p in stage.iterdir()} != names):
            raise ValueError('runtime staged entry inventory changed')
        for name, pin in record['pins'].items():
            if hashlib.sha256(self.file_bytes(stage / name, 1024 * 1024)).hexdigest() != pin:
                raise ValueError('runtime staged entry changed')
        if self.artifact_hash(Path(intent['artifactPath']), int(intent['artifactSize'])) != intent['artifactSha256']:
            raise ValueError('runtime artifact digest changed')

    @contextlib.contextmanager
    def lock(self, name, wait=0, create=True):
        path = self.root / name
        fd = os.open(str(path), (os.O_CREAT if create else 0) | os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
        try:
            s = os.fstat(fd)
            if (not stat.S_ISREG(s.st_mode) or s.st_uid != 0 or s.st_nlink != 1 or
                    s.st_size or stat.S_IMODE(s.st_mode) != 0o600):
                raise ValueError('invalid runtime handoff lock')
            deadline = time.monotonic() + wait
            while True:
                try:
                    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    break
                except BlockingIOError:
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(0.02)
            named = path.stat()
            if (s.st_dev, s.st_ino) != (named.st_dev, named.st_ino):
                raise ValueError('runtime handoff lock replaced')
            yield fd
        finally:
            os.close(fd)

    def mutex(self):
        return self.lock('ota_handoff_' + self.job + '.lock', wait=2)

    def lease(self):
        @contextlib.contextmanager
        def bound():
            with self.lock('ota_job_' + self.job + '.lock', create=False) as fd:
                value = self.intent()
                info = os.fstat(fd)
                if (info.st_dev, info.st_ino) != (int(value['leaseDev']), int(value['leaseInode'])):
                    raise ValueError('runtime intent lease inode changed')
                yield fd
        return bound()

    def save(self, value):
        if value['phase'] != 'OWNER_FINISHED':
            self.index_unfinished(value)
        temporary = self.path.with_name(self.path.name + '.tmp.' + str(os.getpid()))
        try:
            fd = os.open(str(temporary), os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
            with os.fdopen(fd, 'wb') as output:
                output.write((json.dumps(value, sort_keys=True) + '\n').encode())
                output.flush()
                os.fsync(output.fileno())
            os.replace(str(temporary), str(self.path))
            sync_dir(str(self.root))
            if value['phase'] == 'OWNER_FINISHED':
                self.retire_index(value)
        finally:
            if temporary.exists():
                temporary.unlink()

    def index_directory(self, name, create=False):
        path = self.root / name
        if create and not path.exists() and not path.is_symlink():
            path.mkdir(mode=0o700)
        value = path.lstat()
        if path.is_symlink() or not stat.S_ISDIR(value.st_mode) or value.st_uid != 0 or stat.S_IMODE(value.st_mode) != 0o700:
            raise ValueError('untrusted handoff index directory')
        if create:
            sync_dir(str(self.root))  # also repair mkdir succeeded / parent fsync failed
        return path

    def index_unfinished(self, record):
        directory = self.index_directory('runtime-unfinished', create=True)
        marker = directory / (self.job + '.json')
        expected = dict(format='runtime-unfinished-v1', jobId=self.job, token=self.token,
                        intentSha256=record['intentSha256'])
        if marker.exists() or marker.is_symlink():
            if self.json_file(marker) != expected:
                raise ValueError('unfinished handoff index drift')
            self.publish_index(marker, expected, 0o600)
            return
        count, jobs = 0, set()
        for path in directory.iterdir():
            count += 1
            final = re.fullmatch(r'([A-Za-z0-9._-]{1,128})\.json', path.name)
            partial = re.fullmatch(r'\.([A-Za-z0-9._-]{1,128})\.json\.([0-9a-f-]{36})\.publish', path.name)
            if count > 256 or not final and not partial:
                raise ValueError('unknown/excess unfinished index entries')
            job = (final or partial)[1]
            if partial and job == self.job and partial[2] != self.token:
                raise ValueError('unfinished publication token drift')
            jobs.add(job)
            if len(jobs - {self.job}) >= 128:
                raise ValueError('unfinished handoff count exceeds bound')
        self.publish_index(marker, expected, 0o600)

    def publish_index(self, path, value, mode):
        # Called by the original job's serialized writer. The exact job/token
        # namespace plus durable intent/terminal bytes owns this temporary only.
        data = (json.dumps(value, sort_keys=True) + '\n').encode()
        temporary = path.with_name('.' + path.name + '.' + self.token + '.publish')
        if path.exists() or path.is_symlink():
            final = path.lstat()
            if stat.S_IMODE(final.st_mode) != mode or self.file_bytes(path) != data:
                raise ValueError('index publication target drift')
        retained_path = self.root / 'runtime-interrupted-index'
        if retained_path.exists() or retained_path.is_symlink():
            # A prior move may have succeeded before its archive-directory fsync.
            # Repair that durability without scanning/deleting retained evidence.
            retained = self.index_directory('runtime-interrupted-index', create=True)
            sync_dir(str(retained))
        if temporary.exists() or temporary.is_symlink():
            before = temporary.lstat()
            if (not stat.S_ISREG(before.st_mode) or before.st_uid != 0 or before.st_nlink != 1 or
                    stat.S_IMODE(before.st_mode) not in (0o600, mode) or before.st_size > len(data)):
                raise ValueError('unowned interrupted index publication')
            partial = self.file_bytes(temporary)
            if partial != data[:len(partial)]:
                raise ValueError('interrupted index bytes differ from original job')
            retained = self.index_directory('runtime-interrupted-index', create=True)
            archive = retained / (self.job + '-' + self.token + '-' + uuid.uuid4().hex)
            named = temporary.lstat()
            if not stat.S_ISREG(named.st_mode) or (named.st_dev, named.st_ino) != (before.st_dev, before.st_ino):
                raise ValueError('interrupted index inode changed')
            os.rename(str(temporary), str(archive))
            sync_dir(str(retained))
            sync_dir(str(path.parent))
        if path.exists():
            sync_dir(str(path.parent))  # repair an uncertain publish directory-fsync
            return
        fd = os.open(str(temporary), os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, 'wb') as out:
            out.write(data)
            out.flush()
            os.fchmod(out.fileno(), mode)
            os.fsync(out.fileno())
        # The job mutex/lease excludes competing official writers. Never replace
        # a pre-existing final object, including a dangling symlink.
        if path.exists() or path.is_symlink():
            raise ValueError('index publication namespace changed')
        os.rename(str(temporary), str(path))
        sync_dir(str(path.parent))

    def retire_index(self, record):
        if record['phase'] not in ('OWNER_FINISHED', 'CANCELLED'):
            raise ValueError('unfinished handoff cannot retire')
        directory = self.root / 'runtime-unfinished'
        if not directory.exists() and not directory.is_symlink():
            return  # pre-index consumed history stays untouched
        directory = self.index_directory('runtime-unfinished')
        marker = directory / (self.job + '.json')
        if not marker.exists() and not marker.is_symlink():
            sync_dir(str(directory))  # retry marker-unlink directory durability
            return
        expected = dict(format='runtime-unfinished-v1', jobId=self.job, token=self.token,
                        intentSha256=record['intentSha256'])
        if self.json_file(marker) != expected:
            raise ValueError('unfinished retirement owner mismatch')
        health = self.root / ('health_' + self.job + '.txt')
        decision = self.json_file(health)
        if decision.get('jobId') != self.job or decision.get('token') != self.token:
            raise ValueError('unfinished retirement health mismatch')
        retained = self.index_directory('runtime-retired', create=True)
        receipt = retained / (self.job + '-' + self.token + '.json')
        value = dict(format='runtime-retired-v1', originalIndex=expected,
                     terminalSha256=hashlib.sha256(self.file_bytes(self.path)).hexdigest(),
                     healthSha256=hashlib.sha256(self.file_bytes(health)).hexdigest())
        self.publish_index(receipt, value, 0o400)
        # Only the verified pending POINTER is removed, never a terminal journal,
        # health, staged helper, lease, original intent or retained audit receipt.
        marker.unlink()
        sync_dir(str(directory))

    @staticmethod
    def unfinished(staging):
        """Bounded pending pointers only; completed journals remain audit history."""
        scanner = RuntimeHandoff(staging, 'INDEX_SCAN', '0' * 36)
        directory = scanner.root / 'runtime-unfinished'
        owners = {}
        if directory.exists() or directory.is_symlink():
            directory = scanner.index_directory('runtime-unfinished')
            count = 0
            for path in directory.iterdir():
                count += 1
                if count > 256:  # at most 128 pointers plus their interrupted publications
                    raise ValueError('unfinished index entry count exceeds bound')
                final = re.fullmatch(r'([A-Za-z0-9._-]{1,128})\.json', path.name)
                partial = re.fullmatch(r'\.([A-Za-z0-9._-]{1,128})\.json\.([0-9a-f-]{36})\.publish', path.name)
                if not final and not partial:
                    raise ValueError('unknown unfinished index entry')
                job = (final or partial)[1]
                intent_path = scanner.root / ('ota_job_' + job + '.json')
                intent = scanner.json_file(intent_path)
                protocol = RuntimeHandoff(staging, job, intent.get('token', ''))
                protocol.intent()
                expected = dict(format='runtime-unfinished-v1', jobId=job, token=protocol.token,
                                intentSha256=hashlib.sha256(protocol.file_bytes(intent_path)).hexdigest())
                if final:
                    if protocol.json_file(path) != expected:
                        raise ValueError('unfinished index owner drift')
                else:
                    # validate/retain partial bytes only through the original job's
                    # mutex and publish_index; an unfinished pointer cannot authorize mutation.
                    if partial[2] != protocol.token:
                        raise ValueError('unfinished publication token drift')
                if job in owners and owners[job].token != protocol.token:
                    raise ValueError('ambiguous unfinished owner')
                owners[job] = protocol
                if len(owners) > 128:
                    raise ValueError('unfinished handoff count exceeds bound')
        # Pre-index versions had one durable active-job pointer. Directly bind it,
        # never glob all old journals or discard completed evidence to meet a cap.
        active = scanner.root / 'ota_active_job.txt'
        if active.exists() or active.is_symlink():
            raw = scanner.file_bytes(active, 256).decode('ascii')
            match = re.fullmatch(r'jobId=([A-Za-z0-9._-]{1,128})\ntoken=([0-9a-f-]{36})\n', raw)
            if not match:
                raise ValueError('invalid boot active job pointer')
            protocol = RuntimeHandoff(staging, match[1], match[2])
            if protocol.intent_path.exists() or protocol.intent_path.is_symlink():
                intent = protocol.json_file(protocol.intent_path)
                if intent.get('lifecycle') == 'systemd-handoff-v1':
                    protocol.intent()
                    owners[protocol.job] = protocol
                elif protocol.path.exists() or protocol.path.is_symlink():
                    raise ValueError('active handoff intent lifecycle drift')
            elif protocol.path.exists() or protocol.path.is_symlink():
                raise ValueError('active handoff intent missing')
        if len(owners) > 128:
            raise ValueError('unfinished handoff count exceeds bound')
        return list(owners.values())

    def read(self):
        value = self.json_file(self.path)
        intent = self.intent()
        if (value.get('format') != 'ota-handoff-v1' or value.get('jobId') != self.job or
                value.get('token') != self.token or value.get('unit') != self.unit or
                value.get('intentSha256') != hashlib.sha256(self.file_bytes(self.intent_path)).hexdigest() or
                value.get('version') != intent['version'] or type(value.get('runtimeMayChange')) is not bool or
                value.get('phase') not in ('HANDOFF_PENDING', 'OWNER_READY', 'OWNER_OWNED',
                                          'OWNER_BLOCKED', 'CANCEL_REQUESTED', 'CANCELLED',
                                          'OWNER_INSTALLING', 'OWNER_FINISHED') or
                value['runtimeMayChange'] != (value['phase'] in ('OWNER_INSTALLING', 'OWNER_FINISHED'))):
            raise ValueError('runtime handoff record mismatch')
        return value

    @staticmethod
    def boot():
        return Path('/proc/sys/kernel/random/boot_id').read_text().strip()

    @staticmethod
    def process(pid):
        # starttime distinguishes same-boot PID reuse; setsid alone is not isolation.
        ticks = Path('/proc', str(pid), 'stat').read_text().rsplit(')', 1)[1].split()[19]
        mounted = RuntimeHandoff.cgroup_mount()
        memberships = RuntimeHandoff.cgroup_memberships('/proc/' + str(pid) + '/cgroup')
        matches = [group for hierarchy, controllers, group in memberships
                   if hierarchy == mounted['hierarchyId'] and controllers == mounted['controller']]
        if len(matches) != 1:
            raise ValueError('runtime owner systemd cgroup membership is unverified')
        group = matches[0]
        return dict(pid=pid, startTicks=ticks, cgroup=group)

    @staticmethod
    def kernel_text(path, limit=131072):
        with Path(path).open() as source:
            contents = source.read(limit + 1)
        if len(contents) > limit:
            raise ValueError('runtime cgroup kernel read exceeds bound')
        return contents

    @staticmethod
    def cgroup_memberships(path):
        rows, identifiers, controllers_seen = [], set(), set()
        for line in RuntimeHandoff.kernel_text(path, 16384).splitlines():
            hierarchy, controllers, group = line.split(':', 2)
            if (not hierarchy.isdigit() or (hierarchy == '0') != (controllers == '') or
                    hierarchy in identifiers or not group.startswith('/') or
                    any(part in ('.', '..') for part in group.split('/')) or '//' in group):
                raise ValueError('runtime cgroup membership is invalid')
            for controller in controllers.split(','):
                if controller in controllers_seen:
                    raise ValueError('runtime cgroup controller membership is ambiguous')
                controllers_seen.add(controller)
            identifiers.add(hierarchy)
            rows.append((hierarchy, controllers, group))
        if not rows:
            raise ValueError('runtime cgroup membership is missing')
        return rows

    @staticmethod
    def cgroup_mount():
        rows = []
        for line in RuntimeHandoff.kernel_text('/proc/self/mountinfo').splitlines():
            left, right = line.split(' - ', 1)
            fields, metadata = left.split(), right.split()
            if metadata[0] in ('cgroup', 'cgroup2'):
                rows.append((metadata[0], fields[3], fields[4], set(metadata[2].split(','))))
        current = RuntimeHandoff.cgroup_memberships('/proc/self/cgroup')
        manager = RuntimeHandoff.cgroup_memberships('/proc/1/cgroup')
        named = [row for row in current if 'name=systemd' in row[1].split(',')]
        if named:
            # Hybrid systemd245 owns the named v1 tree, not the secondary v2
            # mount. Resource-controller paths cannot establish owner absence.
            if len(named) != 1 or named[0][1] != 'name=systemd':
                raise ValueError('runtime systemd cgroup hierarchy is ambiguous')
            hierarchy, controller, self_group = named[0]
            selected = [row for row in rows if row[0] == 'cgroup' and 'name=systemd' in row[3]]
            expected = '/sys/fs/cgroup/systemd'
        else:
            if len(current) != 1 or current[0][:2] != ('0', '') or any(row[0] == 'cgroup' for row in rows):
                raise ValueError('runtime systemd cgroup hierarchy is unbound')
            hierarchy, controller, self_group = current[0]
            selected = [row for row in rows if row[0] == 'cgroup2']
            expected = '/sys/fs/cgroup'
        manager_groups = [group for identifier, controllers, group in manager
                          if (identifier, controllers) == (hierarchy, controller)]
        if len(selected) != 1 or selected[0][1:3] != ('/', expected) or len(manager_groups) != 1:
            raise ValueError('runtime systemd cgroup root/mount is unverified')
        root = Path(expected)
        if (any(path.is_symlink() for path in (root,) + tuple(root.parents)) or not root.is_dir() or
                (controller == '' and not (root / 'cgroup.controllers').is_file())):
            raise ValueError('runtime systemd cgroup root/mount is unverified')
        # Positively bind the selected mount to both caller and PID1 membership.
        for group, pid in ((self_group, os.getpid()), (manager_groups[0], 1)):
            directory = root / group.lstrip('/')
            if any(path.is_symlink() for path in (directory,) + tuple(directory.parents)):
                raise ValueError('runtime systemd cgroup path contains symlink')
            tasks = directory / 'cgroup.procs'
            if tasks.is_symlink() or str(pid) not in RuntimeHandoff.kernel_text(tasks, 65536).splitlines():
                raise ValueError('runtime systemd cgroup process binding is unverified')
        info = root.stat()
        return dict(path=expected, dev=info.st_dev, inode=info.st_ino,
                    filesystem=selected[0][0], controller=controller, hierarchyId=hierarchy)

    def command(self, *args):
        return subprocess.run(args, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              timeout=15, encoding='utf-8').stdout

    def properties(self):
        raw = self.command('/usr/bin/systemctl', 'show', self.unit, '--no-pager',
                           '--property=Id,LoadState,ActiveState,MainPID,ControlGroup,InvocationID,Restart,Job')
        result = dict(line.split('=', 1) for line in raw.splitlines())
        if set(result) != {'Id', 'LoadState', 'ActiveState', 'MainPID', 'ControlGroup',
                           'InvocationID', 'Restart', 'Job'} or result['Id'] != self.unit:
            raise ValueError('unexpected transient unit properties')
        return result

    def verify_owner(self, record, live=True):
        properties = self.properties()
        if record['bootId'] != self.boot():
            if (live or properties['LoadState'] != 'not-found' or properties['MainPID'] != '0' or
                    properties['ActiveState'] != 'inactive' or properties['ControlGroup'] or
                    properties['InvocationID'] or properties['Job']):
                raise ValueError('runtime owner boot changed but unit absence is unproved')
            return properties
        if record.get('cgroupMount') != self.cgroup_mount():
            raise ValueError('runtime cgroup root changed or is unverified')
        if properties['Restart'] != 'no':
            raise ValueError('runtime owner may automatically restart')
        owner = record.get('owner')
        if owner:
            gone = (not live and properties['ActiveState'] in ('inactive', 'failed') and
                    properties['MainPID'] == '0' and not properties['InvocationID'] and
                    not properties['ControlGroup'] and not properties['Job'])
            if properties['InvocationID'] != record['invocationId'] and not gone:
                raise ValueError('runtime owner invocation changed')
            try:
                current = self.process(owner['pid'])
            except FileNotFoundError:
                current = None
            if current is not None and current != owner:
                raise ValueError('runtime owner PID reused or cgroup changed')
            if live and (current != owner or properties['MainPID'] != str(owner['pid']) or
                         properties['ControlGroup'] != record['unitControlGroup'] or
                         properties['ActiveState'] not in ('active', 'activating')):
                raise ValueError('runtime owner is not independently active')
        elif live:
            raise ValueError('runtime owner is not ready')
        return properties

    def no_unit_tasks(self, record):
        mounted = self.cgroup_mount()
        if record['bootId'] != self.boot():
            # A new boot may remount cgroupfs. Require exact manager absence;
            # the caller still holds the original lease and cancellation mutex.
            self.verify_owner(record, live=False)
        elif record.get('cgroupMount') != mounted:
            raise ValueError('runtime cgroup root changed or is unverified')
        group = record.get('unitControlGroup')
        if not group:
            return
        if group != '/system.slice/' + self.unit:
            raise ValueError('runtime cancellation cgroup escaped')
        root = Path(mounted['path']) / group.lstrip('/')
        if root.is_symlink() or root.parent.is_symlink():
            raise ValueError('runtime cgroup contains symlink')
        try:
            info = root.lstat()
        except FileNotFoundError:
            return
        if not stat.S_ISDIR(info.st_mode):
            raise ValueError('runtime cgroup is not a directory')
        count = 0
        def scan_error(error):
            raise error
        for directory, children, files in os.walk(str(root), followlinks=False, onerror=scan_error):
            count += 1
            if count > 128:
                raise ValueError('runtime cgroup inventory exceeds bound')
            path = Path(directory)
            if path.is_symlink() or any((path / child).is_symlink() for child in children):
                raise ValueError('runtime cgroup contains symlink')
            with (path / 'cgroup.procs').open() as source:
                if source.read(4097).strip():
                    raise ValueError('runtime cgroup still has tasks')

    def unit_process(self, record, properties):
        if record.get('cgroupMount') != self.cgroup_mount():
            raise ValueError('runtime cgroup root changed or is unverified')
        pid = int(properties['MainPID'])
        owner = self.process(pid)
        expected = ['/usr/bin/python3', '-B', str(Path(record['entry']) / 'offline-runtime-upgrade.py'),
                    'ota-owner', str(self.root), self.job, self.token]
        command = Path('/proc', str(pid), 'cmdline').read_bytes().rstrip(b'\0').split(b'\0')
        if (command != [s.encode() for s in expected] or properties['ControlGroup'] != owner['cgroup'] or
                owner['cgroup'] != '/system.slice/' + self.unit or properties['Restart'] != 'no' or
                not re.fullmatch(r'[0-9a-f]{32}', properties['InvocationID'])):
            raise ValueError('runtime owner command/unit/cgroup mismatch')
        return owner

    def launch(self, entry, donor_fd):
        entry = Path(entry)
        expected = (self.root / ('ota_job_' + self.job + '.lock')).stat()
        actual = os.fstat(donor_fd)
        if (expected.st_dev, expected.st_ino) != (actual.st_dev, actual.st_ino):
            raise ValueError('donor lease inode mismatch')
        try:
            with self.lease():
                raise ValueError('donor lease is not held')
        except BlockingIOError:
            pass
        with self.mutex():
            if self.path.exists() or self.path.is_symlink():
                raise ValueError('runtime handoff already consumed')
            intent = self.intent()
            mounted = self.cgroup_mount()
            props = self.properties()
            if props['LoadState'] != 'not-found' or props['MainPID'] != '0' or props['Job']:
                raise ValueError('runtime transient unit namespace already occupied')
            stage = self.root / ('runtime-entry-' + self.job)
            stage.mkdir(mode=0o700)
            pins = {}
            for name in ('offline-runtime-upgrade.py', 'runtime-upgrade-guard.py', 'ota-health-result.py'):
                data = self.file_bytes(entry.with_name(name), 1024 * 1024)
                path = stage / name
                fd = os.open(str(path), os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o400)
                with os.fdopen(fd, 'wb') as output:
                    output.write(data)
                    output.flush()
                    os.fsync(output.fileno())
                pins[name] = hashlib.sha256(data).hexdigest()
            sync_dir(str(stage))
            record = dict(format='ota-handoff-v1', jobId=self.job, token=self.token,
                          version=intent['version'], unit=self.unit, bootId=self.boot(),
                          phase='HANDOFF_PENDING', runtimeMayChange=False, entry=str(stage), pins=pins,
                          intentSha256=hashlib.sha256(self.file_bytes(self.intent_path)).hexdigest(),
                          leaseIdentity=dict(dev=actual.st_dev, inode=actual.st_ino), cgroupMount=mounted,
                          deadlineUptime=time.monotonic() + 20)
            self.verify_inputs(record)
            self.save(record)
            # Serialize dispatch with cancellation: no delayed StartUnit after stop proof.
            # The worker takes the mutex only after exec; no readiness wait here.
            self.command('/usr/bin/systemd-run', '--quiet', '--unit=' + self.unit,
                         '--property=Restart=no', '--property=KillMode=control-group',
                         '--property=TimeoutStopSec=5s', '--property=RuntimeMaxSec=900s',
                         '/usr/bin/python3', '-B', str(stage / 'offline-runtime-upgrade.py'),
                         'ota-owner', str(self.root), self.job, self.token)
        # Neither ota_state nor the job mutex is held across handshake waits.
        while time.monotonic() < record['deadlineUptime']:
            with self.mutex():
                ready = self.read()
                if ready['phase'] == 'OWNER_READY':
                    self.verify_owner(ready)
                    return
                if ready['phase'] != 'HANDOFF_PENDING':
                    raise ValueError('runtime owner readiness cancelled')
            time.sleep(0.02)
        raise ValueError('runtime owner readiness timeout')

    def own(self, install=None):
        with self.mutex():
            record = self.read()
            if record['phase'] != 'HANDOFF_PENDING' or record['bootId'] != self.boot():
                raise ValueError('runtime owner launch is cancelled or stale')
            self.verify_inputs(record)
            props = self.properties()
            owner = self.unit_process(record, props)
            if (props['MainPID'] != str(os.getpid()) or
                    os.environ.get('INVOCATION_ID') != props['InvocationID']):
                raise ValueError('runtime owner is not the exact transient unit')
            record.update(phase='OWNER_READY', owner=owner, invocationId=props['InvocationID'],
                          unitControlGroup=props['ControlGroup'])
            self.save(record)
        while time.monotonic() < record['deadlineUptime']:
            try:
                with self.lease() as lease:
                    with self.mutex():
                        record = self.read()
                        if record['phase'] != 'OWNER_READY':
                            raise ValueError('runtime ownership cancelled')
                        info = os.fstat(lease)
                        if record['leaseIdentity'] != dict(dev=info.st_dev, inode=info.st_ino):
                            raise ValueError('runtime lease was replaced')
                        self.verify_owner(record)
                        self.verify_inputs(record)
                        record['phase'] = 'OWNER_OWNED'
                        self.save(record)
                    # Keep the job lease, release the short journal mutex BEFORE
                    # taking the offline lock or stopping any parent daemon.
                    if install is not None:
                        try:
                            install(self, record)
                            return
                        except BaseException:
                            with self.mutex():
                                fresh = self.read()
                                if fresh['runtimeMayChange']:
                                    raise  # mutation is unresolved, never failed/unchanged
                                record = fresh
                    with self.mutex():
                        record.update(phase='OWNER_BLOCKED', reason='production runtime baseline admission unavailable or rejected')
                        self.save(record)
                    return
            except BlockingIOError:
                time.sleep(0.02)
        raise ValueError('runtime lease acquisition timeout')

    def cancel(self):
        with self.mutex():
            record = self.read()
            if record['runtimeMayChange']:
                raise ValueError('production runtime mutation requires official lifecycle recovery')
            if record['phase'] != 'CANCELLED':
                props = self.verify_owner(record, live=False)
                if not record.get('owner') and props['MainPID'] != '0':
                    record.update(owner=self.unit_process(record, props), invocationId=props['InvocationID'],
                                  unitControlGroup=props['ControlGroup'])
                record['phase'] = 'CANCEL_REQUESTED'
                self.save(record)
        # Tombstone prevents future workers from writing even across the stop race.
        try:
            self.command('/usr/bin/systemctl', 'stop', self.unit)
        except subprocess.CalledProcessError as error:
            props = self.properties()
            if error.returncode != 5 or props['LoadState'] != 'not-found':
                raise
        with self.lease() as lease, self.mutex():
            record = self.read()
            info = os.fstat(lease)
            if record['leaseIdentity'] != dict(dev=info.st_dev, inode=info.st_ino):
                raise ValueError('runtime cancellation lease changed')
            props = self.verify_owner(record, live=False)
            if (props['ActiveState'] not in ('inactive', 'failed') or props['MainPID'] != '0' or props['Job']):
                raise ValueError('runtime owner stop is unconfirmed')
            self.no_unit_tasks(record)
            if record['phase'] not in ('CANCEL_REQUESTED', 'CANCELLED'):
                raise ValueError('runtime cancellation tombstone changed')
            for name in ('ota-in-progress', 'ota_barrier_' + self.job + '.json',
                         self.job + '/health_pending.txt', 'rollback_' + self.job + '.txt',
                         'rollback_' + self.job + '_proof.txt'):
                if (self.root / name).exists() or (self.root / name).is_symlink():
                    raise ValueError('runtime mutation cannot be excluded')
            applied = self.root / 'applied_jobs.txt'
            if applied.exists() or applied.is_symlink():
                if self.job in self.file_bytes(applied, 16 * 1024 * 1024).decode().splitlines():
                    raise ValueError('runtime job was applied')
            if record['phase'] != 'CANCELLED':
                record['phase'] = 'CANCELLED'
                record['cancelProof'] = dict(unit=self.unit, mainPid=props['MainPID'], job=props['Job'],
                                            noWriter=True, bootId=self.boot())
                self.save(record)
            health = self.root / ('health_' + self.job + '.txt')
            with self.lock('ota_state.lock', wait=2):
                self.intent()
                if self.file_bytes(self.root / 'ota_active_job.txt', 256) != (
                        'jobId=' + self.job + '\ntoken=' + self.token + '\n').encode('ascii'):
                    raise ValueError('runtime cancellation active job changed')
                final = self.root / ('ota_final_' + self.job + '.txt')
                if (final.exists() or final.is_symlink()) and self.file_bytes(final, 256) != (
                        'jobId=' + self.job + '\ntoken=' + self.token + '\nstatus=failed\n').encode('ascii'):
                    raise ValueError('runtime cancellation final conflicts')
                if not (health.exists() or health.is_symlink()):
                    data = dict(format='ota-health-v1', jobId=self.job, version=record['version'],
                                token=self.token, status='failed', rollback='unchanged', ts=str(int(time.time() * 1000)))
                    temporary = health.with_name(health.name + '.tmp.' + str(os.getpid()))
                    try:
                        fd = os.open(str(temporary), os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
                        with os.fdopen(fd, 'wb') as output:
                            output.write((json.dumps(data) + '\n').encode())
                            output.flush()
                            os.fsync(output.fileno())
                        os.rename(str(temporary), str(health))
                        sync_dir(str(self.root))
                    finally:
                        if temporary.exists():
                            temporary.unlink()
                result = self.json_file(health)
                if (result.get('format') != 'ota-health-v1' or result.get('jobId') != self.job or
                        result.get('token') != self.token or result.get('version') != record['version'] or
                        result.get('status') != 'failed' or result.get('rollback') != 'unchanged'):
                    raise ValueError('runtime cancellation health mismatch')
                # Also repairs an uncertain prior directory-fsync after atomic publish.
                sync_dir(str(self.root))
            self.retire_index(record)

    def recover(self, boot_prepare=False, boot_work=None):
        # A missing record alone proves nothing: exclude donor/helper via the
        # intent-pinned lease and serialize a tombstone with all future dispatch.
        if not (self.path.exists() or self.path.is_symlink()):
            with self.lease() as lease, self.mutex():
                if not (self.path.exists() or self.path.is_symlink()):
                    intent = self.intent()
                    props = self.properties()
                    if (props['LoadState'] != 'not-found' or props['ActiveState'] != 'inactive' or
                            props['MainPID'] != '0' or props['ControlGroup'] or props['InvocationID'] or props['Job']):
                        raise ValueError('missing handoff record but manager owner exists')
                    info = os.fstat(lease)
                    self.save(dict(format='ota-handoff-v1', jobId=self.job, token=self.token,
                                   version=intent['version'], unit=self.unit, bootId=self.boot(),
                                   phase='CANCEL_REQUESTED', runtimeMayChange=False,
                                   intentSha256=hashlib.sha256(self.file_bytes(self.intent_path)).hexdigest(),
                                   leaseIdentity=dict(dev=info.st_dev, inode=info.st_ino), cgroupMount=self.cgroup_mount()))
        if self.path.exists() or self.path.is_symlink():
            with self.mutex():
                record = self.read()
                if record['phase'] == 'OWNER_FINISHED':
                    self.retire_index(record)
                    return
                mutated = record['runtimeMayChange']
            if mutated:
                with self.lease():
                    with self.mutex():
                        record = self.read()
                        if record['phase'] == 'OWNER_FINISHED':
                            self.retire_index(record)
                            return
                        props = self.verify_owner(record, live=False)
                        if props['ActiveState'] not in ('inactive', 'failed') or props['MainPID'] != '0' or props['Job']:
                            raise ValueError('production recovery requires positive stopped owner proof')
                        self.no_unit_tasks(record)
                        self.verify_inputs(record)
                        entry = self.production_entry(record)
                    if boot_prepare:
                        return entry.production_ota_resume(self, record, boot_prepare=True)
                    if boot_work is not None:
                        return entry.production_ota_resume(self, record, boot_work=boot_work)
                    entry.production_ota_resume(self, record)
                return
        self.cancel()

    def production_entry(self, record):
        # Original staged code, never the now-replaced live helper or arbitrary
        # manifest-provided hook. verify_inputs already checked all three pins.
        path = Path(record['entry']) / 'offline-runtime-upgrade.py'
        spec = importlib.util.spec_from_file_location('ota_pinned_production_recovery', str(path))
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def finish(self):
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            with self.mutex():
                record = self.read()
                if record['phase'] in ('OWNER_INSTALLING', 'OWNER_FINISHED'):
                    return  # asynchronous owner holds the lease; no cancellation or polling hot path
                if (record['phase'] in ('OWNER_BLOCKED', 'CANCEL_REQUESTED', 'CANCELLED') or
                        record['bootId'] != self.boot() or time.monotonic() >= record['deadlineUptime']):
                    break
            time.sleep(0.02)
        self.recover()


def main():
    if len(sys.argv) != 6:
        raise ValueError("expected staging, job, version, status, rollback")
    staging, job, version, status, rollback = sys.argv[1:]
    lock_fd = os.open(os.path.join(staging, "ota_state.lock"), os.O_CREAT | os.O_RDWR, 0o600)
    fcntl.flock(lock_fd, fcntl.LOCK_EX)
    if not job or any(c not in "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-" for c in job):
        raise ValueError("invalid job id")
    if status not in ("healthy", "failed") or rollback not in ("none", "succeeded", "failed", "skipped"):
        raise ValueError("invalid health result")
    if (status == "healthy") != (rollback == "none"):
        raise ValueError("invalid health combination")
    pending = os.path.join(staging, job, "health_pending.txt")
    fields = {}
    with open(pending, encoding="utf-8") as source:
        for line in source:
            key, sep, value = line.rstrip("\n").partition("=")
            if not sep or key in fields:
                raise ValueError("invalid pending health identity")
            fields[key] = value
    if fields.get("jobId") != job or fields.get("version") != version or not fields.get("token"):
        raise ValueError("health identity mismatch")
    intent_path = os.path.join(staging, "ota_job_" + job + ".json")
    if os.path.exists(intent_path):
        with open(intent_path, encoding="utf-8") as source:
            intent = json.load(source)
        if (intent.get("format") != "ota-job-v1" or intent.get("jobId") != job or
            intent.get("version") != version or intent.get("token") != fields["token"]):
            raise ValueError("health does not match OTA intent")
    active_path = os.path.join(staging, "ota_active_job.txt")
    if os.path.exists(active_path):
        with open(active_path, "rb") as source:
            active = source.read(257)
        expected = ("jobId=" + job + "\ntoken=" + fields["token"] + "\n").encode("ascii")
        if active != expected:
            raise ValueError("health result is not for active OTA job")
    else:
        raise ValueError("OTA active job marker is missing")
    state = None
    if status == "healthy":
        with open(os.path.join(staging, job, "applied_state_pending.txt"), encoding="utf-8") as source:
            state = source.read()
        if not state.startswith("jobId=" + job + "\nversion=" + version + "\n") or not state.endswith("\n"):
            raise ValueError("invalid pending version state")
    path = os.path.join(staging, "health_" + job + ".txt")
    if os.path.exists(path):
        raise FileExistsError("OTA health decision already exists")
    record = dict(format="ota-health-v1", jobId=job, version=version,
                  token=fields["token"], status=status, rollback=rollback,
                  ts=str(int(time.time() * 1000)))
    temporary = path + ".tmp." + str(os.getpid())
    try:
        with open(temporary, "x", encoding="utf-8") as output:
            output.write(json.dumps(record, separators=(",", ":")) + "\n")
            output.flush()
            os.fsync(output.fileno())
        os.link(temporary, path)
        directory = os.open(staging, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    if state is not None:
        for name, contents in (("current_version.txt", state), ("applied_version.txt", version + "\n")):
            target = os.path.join(staging, name)
            temporary = target + ".tmp." + str(os.getpid())
            try:
                with open(temporary, "x", encoding="utf-8") as output:
                    output.write(contents)
                    output.flush()
                    os.fsync(output.fileno())
                os.replace(temporary, target)
                directory = os.open(staging, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
                try:
                    os.fsync(directory)
                finally:
                    os.close(directory)
            finally:
                if os.path.exists(temporary):
                    os.unlink(temporary)


if __name__ == "__main__":
    if len(sys.argv) == 7 and sys.argv[1] == '--handoff':
        RuntimeHandoff(Path(sys.argv[2]), sys.argv[3], sys.argv[4]).launch(Path(sys.argv[6]), int(sys.argv[5]))
    elif len(sys.argv) == 5 and sys.argv[1] in ('--handoff-finish', '--handoff-recover'):
        protocol = RuntimeHandoff(Path(sys.argv[2]), sys.argv[3], sys.argv[4])
        if sys.argv[1] == '--handoff-finish':
            protocol.finish()
        else:
            protocol.recover()
    elif len(sys.argv) == 5 and sys.argv[1] in ("--activate", "--cancel"):
        active_job(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[1] == "--cancel")
    else:
        main()
