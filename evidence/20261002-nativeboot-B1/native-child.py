"""B1 review-only payload: actual pinned native CLI in one private mount namespace."""
import ctypes
import errno
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import selectors
import signal
import stat
import struct
import subprocess
import sys
import time

NAMES = ('gw_native_b1_f9a79bd_cold', 'gw_native_b1_f9a79bd_interrupt')
SIZE = 15667032
PIN = 'f6f4a0a94d55827e874e45b9484daf8e9af74bae60ac048e7416ba433881c813'
NEEDED = {'libdl.so.2', 'libpthread.so.0', 'librt.so.1', 'libstdc++.so.6',
          'libm.so.6', 'libgcc_s.so.1', 'libc.so.6'}
VERSIONS = {"libdl.so.2":["GLIBC_2.17"],"libpthread.so.0":["GLIBC_2.17"],"libgcc_s.so.1":["GCC_3.0","GCC_4.2.0"],"librt.so.1":["GLIBC_2.17"],"libstdc++.so.6":["CXXABI_1.3","CXXABI_1.3.5","CXXABI_1.3.9","GLIBCXX_3.4","GLIBCXX_3.4.11","GLIBCXX_3.4.18","GLIBCXX_3.4.19","GLIBCXX_3.4.20","GLIBCXX_3.4.21","GLIBCXX_3.4.9"],"libc.so.6":["GLIBC_2.17"]}
LOADER = '/lib/ld-linux-aarch64.so.1'
ENV = {'PATH': '/usr/bin:/bin', 'LC_ALL': 'C'}
libc = ctypes.CDLL(None, use_errno=True)
libc.ptrace.restype = ctypes.c_long
libc.mount.argtypes = (ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_ulong, ctypes.c_char_p)


def need(ok, message):
    if not ok:
        raise ValueError(message)


def pin(path, limit=64 * 1024**2):
    info = path.stat()
    need(stat.S_ISREG(info.st_mode) and info.st_uid == 0 and not info.st_mode & 0o022 and info.st_size <= limit,
         'untrusted/oversized native input: ' + str(path))
    identity = lambda s: (s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns)
    value = hashlib.sha256()
    with path.open('rb') as stream:
        need(identity(os.fstat(stream.fileno())) == identity(info), 'native input open drift')
        for data in iter(lambda: stream.read(65536), b''):
            value.update(data)
        need(identity(os.fstat(stream.fileno())) == identity(info) == identity(path.stat()), 'native input changed')
    return dict(path=str(path), bytes=info.st_size, sha256=value.hexdigest(), dev=info.st_dev, ino=info.st_ino)


def limits(native=False):
    for key, value in ((resource.RLIMIT_AS, 256 * 1024**2),
                       (resource.RLIMIT_CPU, 15 if native else 10),
                       (resource.RLIMIT_FSIZE, 32 * 1024**2 if native else 1024**2),
                       (resource.RLIMIT_NOFILE, 64), (resource.RLIMIT_CORE, 0)):
        resource.setrlimit(key, (value, value))


def bounded_text(path, limit):
    with path.open(encoding='ascii') as stream:
        value = stream.read(limit + 1)
    need(len(value) <= limit, 'native metadata read exceeds bound: ' + str(path))
    return value


def metadata(command, label, root):
    tool = Path(command[0]).resolve()
    for parent in tool.parents:
        value = parent.stat()
        need(value.st_uid == 0 and not value.st_mode & 0o022, 'untrusted inspection tool directory')
    tool_pin = pin(tool)
    path = root / ('dependency-' + label + '.log')
    with path.open('xb') as out:
        result = subprocess.run(command, stdout=out, stderr=subprocess.STDOUT, timeout=10,
                                env=ENV, preexec_fn=limits)
    need(result.returncode == 0 and path.stat().st_size <= 1024**2, 'bounded dependency inspection failed')
    raw = path.read_bytes()
    return raw.decode('ascii'), dict(argv=command, exitCode=result.returncode, log=str(path),
                                    bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest(), tool=tool_pin)


def dynamic(text):
    need(not re.search(r'\((?:RPATH|RUNPATH|FILTER|AUXILIARY|AUDIT|DEPAUDIT)\)', text), 'unexpected dynamic search/hook')
    return set(re.findall(r'\(NEEDED\).*?Shared library: \[([^\]]+)\]', text))


def dependencies(binary, root):
    commands = []
    text, row = metadata(['/usr/bin/readelf', '-W', '-d', str(binary)], 'pointctl-dynamic', root)
    commands.append(row)
    need(dynamic(text) == NEEDED, 'pointctl dependency drift')
    text, row = metadata(['/usr/bin/readelf', '-W', '-l', str(binary)], 'pointctl-program', root)
    commands.append(row)
    need(re.findall(r'Requesting program interpreter: ([^\]]+)', text) == [LOADER], 'native interpreter drift')
    text, row = metadata(['/sbin/ldconfig', '-p'], 'cache', root)
    commands.append(row)
    system_roots = {Path('/lib').resolve(), Path('/usr/lib').resolve()}
    paths = {}
    for name in sorted(NEEDED):
        candidates = {Path(path).resolve() for path in re.findall(
            r'^\s*' + re.escape(name) + r'\s+\([^\n]+\)\s+=>\s+(/\S+)\s*$', text, re.M)}
        need(len(candidates) == 1, 'dependency cache ambiguous/missing: ' + name)
        paths[name] = candidates.pop()
    paths['ld-linux-aarch64.so.1'] = Path(LOADER).resolve()
    pins = {}
    total = 0
    for name, path in sorted(paths.items()):
        need(any(path == base or base in path.parents for base in system_roots), 'dependency outside system roots')
        for parent in path.parents:
            s = parent.stat()
            need(s.st_uid == 0 and not s.st_mode & 0o022, 'untrusted resolved dependency directory')
        with path.open('rb') as stream:
            header = stream.read(20)
        need(header[:6] == b'\x7fELF\x02\x01' and struct.unpack_from('<H', header, 18)[0] == 183,
             'dependency is not ELF64 LE AArch64')
        pins[name] = pin(path)
        total += pins[name]['bytes']
        need(total <= 64 * 1024**2, 'dependency hash budget exceeded')
        text, row = metadata(['/usr/bin/readelf', '-W', '-d', str(path)], name + '-dynamic', root)
        commands.append(row)
        need(dynamic(text) <= set(paths), 'unapproved transitive native dependency')
        text, row = metadata(['/usr/bin/readelf', '-W', '--version-info', str(path)], name + '-versions', root)
        commands.append(row)
        definitions = text.split('Version definition section', 1)
        definitions = definitions[1].split('Version needs section', 1)[0] if len(definitions) == 2 else ''
        supplied = set(re.findall(r'\bName:\s+(\S+)', definitions))
        need(set(VERSIONS.get(name, [])) <= supplied, 'native symbol provider missing: ' + name)
    return dict(libraries=pins, commands=commands, totalBytes=total)


def mountinfo():
    with Path('/proc/self/mountinfo').open() as stream:
        raw = stream.read(262145)
    need(len(raw) <= 262144, 'mountinfo exceeds bound')
    return [line.split() for line in raw.splitlines()]


def isolate(expected, record):
    need(os.readlink('/proc/self/ns/mnt') == expected['namespace'], 'parent mount namespace drift')
    before = Path('/dev/shm').stat()
    need([before.st_dev, before.st_ino] == expected['shmIdentity'], 'parent SHM directory drift')
    need(libc.unshare(0x00020000) == 0, 'mount unshare unavailable: ' + str(ctypes.get_errno()))
    private = os.readlink('/proc/self/ns/mnt')
    need(private != expected['namespace'], 'mount namespace did not change')
    # No mount or native command before recursive private propagation is proved.
    need(libc.mount(None, b'/', None, (1 << 14) | (1 << 18), None) == 0, 'private propagation failed')
    need(all(not any(x.startswith(('shared:', 'master:', 'propagate_from:')) for x in row[6:row.index('-')])
             for row in mountinfo()), 'mount propagation is not private')
    home = Path('/opt/modbus-gateway')
    need(home.is_dir() and not home.is_symlink() and home.resolve() == home, 'business root not canonical')
    need(not any(row[4].startswith(str(home) + '/') for row in mountinfo()), 'business subtree mount unsupported')
    need(libc.mount(str(home).encode(), str(home).encode(), None, 4096, None) == 0, 'private readonly bind failed')
    need(libc.mount(None, str(home).encode(), None, 4096 | 32 | 1, None) == 0, 'business readonly remount failed')
    need(any(row[4] == str(home) and 'ro' in row[5].split(',') for row in mountinfo()), 'readonly business bind unproved')
    need(libc.mount(b'tmpfs', b'/dev/shm', b'tmpfs', 2 | 4 | 8, b'size=48m,mode=0700') == 0, 'private tmpfs mount failed')
    current = Path('/dev/shm').stat()
    rows = [row for row in mountinfo() if row[4] == '/dev/shm' and
            row[2] == '%d:%d' % (os.major(current.st_dev), os.minor(current.st_dev))]
    need(current.st_dev != before.st_dev and len(rows) == 1 and rows[0][rows[0].index('-') + 1] == 'tmpfs',
         'private SHM mount identity unproved')
    need(not list(Path('/dev/shm').iterdir()) and os.statvfs('/dev/shm').f_blocks *
         os.statvfs('/dev/shm').f_frsize <= 48 * 1024**2, 'private SHM is occupied/oversized')
    record['isolation'] = dict(parent=expected, namespace=private, shmDev=current.st_dev,
                               shmInode=current.st_ino, visibleMount=rows[0], businessReadonly=True,
                               propagationPrivateBeforeMount=True)
    return private, current.st_dev


def object_info(name, device, header=None, size=SIZE):
    need(name in NAMES, 'unapproved native role')
    path = Path('/dev/shm') / name
    value = path.lstat()
    need(stat.S_ISREG(value.st_mode) and not path.is_symlink() and value.st_uid == 0 and
         stat.S_IMODE(value.st_mode) == 0o600 and value.st_nlink == 1 and value.st_dev == device and
         value.st_size == size, 'private native object type/identity/size drift')
    with path.open('rb') as stream:
        actual = stream.read(8)
    if header is not None:
        need(actual == header, 'private native header drift')
    return dict(name=name, dev=value.st_dev, ino=value.st_ino, bytes=value.st_size,
                mode=stat.S_IMODE(value.st_mode), header=actual.hex())


def capacity(record, lock_fd, namespace, device):
    need(os.readlink('/proc/self/ns/mnt') == namespace and Path('/dev/shm').stat().st_dev == device,
         'private mount namespace drift before native execution')
    lock = os.fstat(lock_fd)
    needle = (os.major(lock.st_dev), os.minor(lock.st_dev), lock.st_ino)
    found = 0
    for line in bounded_text(Path('/proc/locks'), 262144).splitlines():
        fields = line.split()
        if len(fields) >= 6 and fields[1:5] == ['FLOCK', 'ADVISORY', 'WRITE', str(os.getpid())]:
            major, minor, inode = fields[5].split(':')
            found += (int(major, 16), int(minor, 16), int(inode)) == needle
    need(found == 1, 'private offline flock ownership unproved')
    lines = [s for s in bounded_text(Path('/proc/meminfo'), 65536).splitlines() if s.startswith('MemAvailable:')]
    need(len(lines) == 1 and re.fullmatch(r'MemAvailable:\s+[0-9]+\s+kB\s*', lines[0]), 'invalid real MemAvailable')
    page = os.sysconf('SC_PAGE_SIZE')
    need(type(page) is int and page > 0, 'invalid native page size')
    rounded = ((SIZE + page - 1) // page) * page
    added = 0
    for name in NAMES:
        path = Path('/dev/shm') / name
        added += rounded - (min(rounded, path.stat().st_blocks * 512) if path.exists() else 0)
    available = int(lines[0].split()[1]) * 1024
    space = os.statvfs('/dev/shm')
    need(space.f_frsize > 0 and space.f_bavail >= 0, 'invalid private tmpfs capacity')
    report = dict(memAvailableBytes=available, additionalPageBytes=added, systemPolicyReserveBytes=64 * 1024**2,
                  memRequiredBytes=added + 64 * 1024**2, shmAvailableBytes=space.f_bavail * space.f_frsize,
                  migrationHeapBytes=0, measuredPeak=False, lockDev=lock.st_dev, lockInode=lock.st_ino)
    record['capacity'].append(report)
    need(available >= report['memRequiredBytes'] and space.f_bavail * space.f_frsize >= added,
         'insufficient real native test capacity')


def stats(binary, name, record, expected_exit=0):
    need(name in NAMES and pin(binary, 674504)['sha256'] == PIN, 'unapproved native command')
    command = [str(binary), 'stats', '--shm', name]
    child = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=ENV,
                             preexec_fn=lambda: limits(True))
    output = {child.stdout: bytearray(), child.stderr: bytearray()}
    selector = selectors.DefaultSelector()
    for stream in output:
        selector.register(stream, selectors.EVENT_READ)
    end = time.monotonic() + 15
    try:
        while selector.get_map():
            need(time.monotonic() < end, 'native stats deadline exceeded')
            for key, _ in selector.select(min(0.1, max(0, end - time.monotonic()))):
                data = os.read(key.fd, 4097 - len(output[key.fileobj]))
                if not data:
                    selector.unregister(key.fileobj)
                else:
                    output[key.fileobj].extend(data)
                    need(len(output[key.fileobj]) <= 4096, 'native output exceeds 4 KiB')
        code = child.wait(timeout=max(0.001, end - time.monotonic()))
    finally:
        if child.poll() is None:
            child.kill()
        child.wait(timeout=2)
        selector.close()
        child.stdout.close(); child.stderr.close()
    out, err = [bytes(output[stream]) for stream in output]
    record['commands'].append(dict(argv=command, exitCode=code, stdout=out.decode('ascii'), stderr=err.decode('ascii')))
    need(code == expected_exit, 'native stats exit mismatch')
    if expected_exit == 0:
        pattern = ('shm=/?' + re.escape(name) + r' latest=0/[0-9]+ latestCapacity=100000 pendingWrites=0/[0-9]+'
                   r' pendingCapacity=4096 persistent=0/[0-9]+ persistentCapacity=20000 pointUpdates=0/65536'
                   r' writeSeq=0 persistentSeq=0 pointUpdateSeq=0\n')
        need(not err and re.fullmatch(pattern, out.decode('ascii')), 'native empty queue/stats proof failed')
    else:
        need(not out and err == ('pointctl error: shared memory size mismatch: /' + name + '\n').encode(),
             'wrong-size refusal not exact')


def ptrace(request, pid, addr=0, data=0):
    ctypes.set_errno(0)
    value = libc.ptrace(ctypes.c_uint(request), ctypes.c_int(pid), ctypes.c_void_p(addr), ctypes.c_void_p(data))
    if value == -1:
        raise OSError(ctypes.get_errno(), 'owned tracee ptrace failed')
    return value


def interrupted(binary, name, device, namespace, dependency, record):
    need(name == NAMES[1] and not (Path('/dev/shm') / name).exists(), 'interruption target already present')
    command = [str(binary), 'stats', '--shm', name]
    read_fd, write_fd = os.pipe()
    pid = os.fork()
    if pid == 0:
        try:
            os.close(read_fd)
            os.dup2(write_fd, 1); os.dup2(write_fd, 2); os.close(write_fd)
            limits(True)
            ptrace(0, 0)
            os.kill(os.getpid(), signal.SIGSTOP)
            os.execve(str(binary), command, ENV)
        finally:
            os._exit(127)
    os.close(write_fd)
    end = time.monotonic() + 15
    reaped = False
    def wait():
        while time.monotonic() < end:
            got, status = os.waitpid(pid, os.WNOHANG)
            if got:
                return status
            time.sleep(0.001)
        raise TimeoutError('owned native tracee deadline')
    try:
        status = wait()
        need(os.WIFSTOPPED(status) and os.WSTOPSIG(status) == signal.SIGSTOP, 'owned tracee initial stop unknown')
        ptrace(0x4200, pid, data=1 | 0x10 | 0x100000)  # TRACESYSGOOD / TRACEEXEC / EXITKILL
        entry = None
        while True:
            ptrace(24, pid)  # PTRACE_SYSCALL; no register or syscall mutation.
            status = wait()
            if os.WIFEXITED(status) or os.WIFSIGNALED(status):
                reaped = True
                raise ValueError('tracee exited before exact native interruption point')
            need(os.WIFSTOPPED(status), 'unknown native tracee wait state')
            stopped = os.WSTOPSIG(status)
            if stopped == signal.SIGTRAP:
                need(status >> 16 == 4, 'unexpected native ptrace event')  # EXEC
                continue
            need(stopped == (signal.SIGTRAP | 0x80), 'unexpected native tracee signal')
            buffer = ctypes.create_string_buffer(96)
            count = ptrace(0x420e, pid, addr=96, data=ctypes.addressof(buffer))
            need(33 <= count <= 96, 'unsupported syscall-info kernel contract')
            raw = buffer.raw
            need(struct.unpack_from('<I', raw, 4)[0] == 0xc00000b7, 'not native AArch64 syscall architecture')
            if raw[0] == 1:
                need(count >= 80, 'short native syscall-entry info')
                number, fd, length = struct.unpack_from('<QQQ', raw, 24)
                entry = None
                if number == 46:  # AArch64 ftruncate
                    fd_path = Path('/proc') / str(pid) / 'fd' / str(fd)
                    need(length == SIZE and os.readlink(str(fd_path)) == '/dev/shm/' + name,
                         'ftruncate is not exact private role/ABI size')
                    need(os.readlink('/proc/%d/ns/mnt' % pid) == namespace and os.getpgid(pid) == os.getpgrp(),
                         'tracee escaped owned namespace/group')
                    value = object_info(name, device, size=0)
                    opened = fd_path.stat()
                    need((opened.st_dev, opened.st_ino) == (value['dev'], value['ino']), 'tracee fd identity mismatch')
                    entry = dict(syscall=number, fd=fd, length=length, object=value)
            elif raw[0] == 2:
                if entry is None:
                    continue
                need(struct.unpack_from('<q', raw, 24)[0] == 0 and raw[32] == 0, 'ftruncate did not return success')
                value = object_info(name, device, header=b'\0' * 8)
                need((value['dev'], value['ino']) == (entry['object']['dev'], entry['object']['ino']),
                     'private native object replaced at syscall exit')
                need(Path('/proc/%d/exe' % pid).resolve() == binary, 'tracee executable drift')
                maps = bounded_text(Path('/proc/%d/maps' % pid), 262144)
                mapped = {str(Path(line.split(maxsplit=5)[5].strip()).resolve()) for line in maps.splitlines()
                          if len(line.split(maxsplit=5)) == 6 and line.split(maxsplit=5)[5].startswith('/')}
                providers = {row['path'] for row in dependency['libraries'].values()}
                need(providers <= mapped and mapped <= providers | {str(binary)}, 'actual native provider maps differ')
                record['interruption'] = dict(pid=pid, group=os.getpgrp(), namespace=namespace,
                    arch='AUDIT_ARCH_AARCH64', syscall=46, syscallReturn=0, entry=entry, after=value,
                    mappedProviders=sorted(mapped), stage='ftruncate EXIT before returning to user-space',
                    outputBytesBeforeKill=0)
                # Do not resume past this proven stop; only this owned tracee is killed.
                os.kill(pid, signal.SIGKILL)
                status = wait(); reaped = True
                need(os.WIFSIGNALED(status) and os.WTERMSIG(status) == signal.SIGKILL, 'tracee termination unproved')
                out = os.read(read_fd, 4097)
                need(not out, 'native output before interrupted initialization')
                record['commands'].append(dict(argv=command, exitCode=-signal.SIGKILL, stdout='', stderr='',
                                                intentionalInterruption=True))
                return value
            else:
                raise ValueError('unknown native syscall-info stop')
    finally:
        if not reaped:
            try: os.kill(pid, signal.SIGKILL)
            except ProcessLookupError: pass
            os.waitpid(pid, 0)
        os.close(read_fd)


def main():
    root = Path(sys.argv[1])
    expected = json.loads(sys.argv[2])
    need(root.resolve() == root and root.parent == Path('/tmp') and not root.is_symlink(), 'private root escaped')
    need(os.geteuid() == 0 and os.uname().machine == 'aarch64', 'native B needs test-board root/AArch64')
    os.umask(0o077)
    binary = root / 'source/pointctl'
    record = dict(format='native-creator-B1-v1', nativeStarted=False, checks=[], commands=[], capacity=[])
    fd = None
    try:
        need(pin(binary, 674504)['sha256'] == PIN and binary.stat().st_size == 674504, 'sealed native creator drift')
        dependency = dependencies(binary, root)
        record['dependencies'] = dependency
        namespace, device = isolate(expected, record)
        fd = os.open(str(root / 'work/private-offline-runtime.lock'), os.O_RDWR | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        def run(name, expected_exit=0):
            capacity(record, fd, namespace, device)
            record['nativeStarted'] = True
            stats(binary, name, record, expected_exit)
        run(NAMES[0]); first = object_info(NAMES[0], device, struct.pack('<II', 0x4d505354, 11))
        record['checks'].append(dict(case='cold-create', passed=True, object=first))
        run(NAMES[0]); need(object_info(NAMES[0], device, struct.pack('<II', 0x4d505354, 11)) == first, 'cold reopen identity drift')
        record['checks'].append(dict(case='reopen-cold', passed=True, object=first))
        capacity(record, fd, namespace, device)
        partial = interrupted(binary, NAMES[1], device, namespace, dependency, record)
        record['checks'].append(dict(case='interrupt-after-ftruncate', passed=True, object=partial))
        run(NAMES[1]); resumed = object_info(NAMES[1], device, struct.pack('<II', 0x4d505354, 11))
        need((resumed['dev'], resumed['ino']) == (partial['dev'], partial['ino']), 'native resume replaced interrupted inode')
        record['checks'].append(dict(case='resume-interrupted-zero-header', passed=True, object=resumed))
        run(NAMES[1]); need(object_info(NAMES[1], device, struct.pack('<II', 0x4d505354, 11)) == resumed, 'resumed reopen drift')
        record['checks'].append(dict(case='reopen-resumed', passed=True, object=resumed))
        # Explicit private size-corruption negative, not a claim of a crash-produced size.
        path = Path('/dev/shm') / NAMES[1]
        with path.open('r+b') as stream:
            need((os.fstat(stream.fileno()).st_dev, os.fstat(stream.fileno()).st_ino) == (resumed['dev'], resumed['ino']),
                 'private negative target open drift')
            stream.truncate(SIZE - 1)
        before = pin(path)
        run(NAMES[1], 1)
        need(pin(path) == before, 'native wrong-size refusal mutated private object')
        record['checks'].append(dict(case='reject-explicit-private-short-size', passed=True, object=before))
        need(set(p.name for p in Path('/dev/shm').iterdir()) == set(NAMES), 'unexpected private native role')
        for row in dependency['libraries'].values():
            need(pin(Path(row['path'])) == row, 'native dependency changed during test')
        need(os.readlink('/proc/self/ns/mnt') == namespace and Path('/dev/shm').stat().st_dev == device,
             'final child namespace drift')
        record['passed'] = len(record['checks']) == 6
    except BaseException as error:
        record.update(passed=False, errorType=type(error).__name__, errorMessage=str(error))
    finally:
        if fd is not None: os.close(fd)
        with (root / 'native-result.json').open('x') as stream:
            json.dump(record, stream, sort_keys=True, indent=2); stream.write('\n'); stream.flush(); os.fsync(stream.fileno())
        print(json.dumps(record))
    return 0 if record.get('passed') else 1


if __name__ == '__main__':
    raise SystemExit(main())
