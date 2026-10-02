"""Read-only board admission and fixed ELF dependency checks; never installs."""
import ctypes
import hashlib
import os
from pathlib import Path
import platform
import re
import stat
import subprocess
import sys

ROOT = '/tmp/gw-r10-68de70f-23-B1'
ENV = dict(PATH='/usr/bin:/bin', LC_ALL='C', HOME='/nonexistent')
# Admission policy: up to three fork-related 512MiB address-space budgets,
# private tmpfs caps273MiB, outer/interpreter128MiB, system allowance64MiB.
# Conservative accounting (shared/COW pages counted again), not a cgroup/RSS guarantee.
MIN_AVAILABLE_BYTES = (3*512 + 273 + 128 + 64)*1024**2


def executable(path, require_execute=True):
    p = Path(path).resolve(); info = p.stat()
    assert stat.S_ISREG(info.st_mode) and info.st_uid == 0 and not info.st_mode & 0o6022
    if require_execute: assert os.access(str(p), os.X_OK)
    try: cap = os.getxattr(str(p), 'security.capability')
    except OSError as error:
        import errno
        assert error.errno in (errno.ENODATA, errno.ENOTSUP)
        cap = b''
    assert not cap, 'exec capability/PDEATHSIG reset risk'
    return dict(path=path, realPath=str(p), dev=info.st_dev, ino=info.st_ino,
                mode=stat.S_IMODE(info.st_mode), bytes=info.st_size)


def inspect(absent=True):
    assert platform.machine() == 'aarch64' and os.getuid() == os.geteuid() == 0
    assert sys.version_info >= (3, 6)
    assert all(hasattr(os, n) for n in ('fork', 'waitid', 'WNOWAIT', 'chroot', 'mknod'))
    libc = ctypes.CDLL(None)
    assert all(hasattr(libc, n) for n in ('unshare', 'prctl', 'mount', 'capset'))
    capabilities = dict(line.split(':', 1) for line in Path('/proc/self/status').read_text().splitlines() if ':' in line)
    effective = int(capabilities['CapEff'].strip(), 16)
    for bit in (5, 8, 18, 21, 27):  # KILL, SETPCAP, SYS_CHROOT, SYS_ADMIN, MKNOD.
        assert effective & (1 << bit), 'required capability absent: %d' % bit
    p = Path('/tmp'); info = p.lstat()
    assert p.resolve() == p and stat.S_ISDIR(info.st_mode) and info.st_uid == 0
    assert stat.S_IMODE(info.st_mode) == 0o1777
    if absent: assert not os.path.lexists(ROOT), 'fresh B1 root occupied; no replay'
    mem = [x.split() for x in Path('/proc/meminfo').read_text().splitlines() if x.startswith('MemAvailable:')]
    assert len(mem) == 1 and len(mem[0]) == 3 and mem[0][2] == 'kB' and mem[0][1].isdigit()
    memory = int(mem[0][1]) * 1024
    space = os.statvfs('/tmp'); free = space.f_bavail * space.f_frsize
    assert memory >= MIN_AVAILABLE_BYTES and free >= 1024**3
    preload = Path('/etc/ld.so.preload')
    assert not preload.is_symlink()
    if preload.exists(): assert preload.is_file() and preload.stat().st_size <= 4096 and not preload.read_bytes().strip()
    tools = [executable(p) for p in ('/usr/bin/python3', '/usr/bin/readelf', '/lib/ld-linux-aarch64.so.1', '/bin/sh', '/bin/sleep')]
    return dict(python=sys.version, bootId=Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
                tools=tools, memAvailableBytes=memory, freeBytes=free,
                parent=dict(dev=info.st_dev, ino=info.st_ino, mode=stat.S_IMODE(info.st_mode)),
                capEff=capabilities['CapEff'].strip(), namespaceCreated=False, remoteWrites=0)


def exported_symbols(raw):
    symbols = set()
    for line in raw.decode('utf-8', 'strict').splitlines():
        parts = line.split()
        if len(parts) < 8 or not parts[0].endswith(':') or not parts[0][:-1].isdigit(): continue
        if parts[4] not in ('GLOBAL', 'WEAK') or parts[5] not in ('DEFAULT', 'PROTECTED') or parts[6] == 'UND': continue
        name = parts[7]
        symbols.add(name.replace('@@', '@'))
        if '@@' in name or '@' not in name: symbols.add(name.split('@', 1)[0])
    return symbols


def listed_libraries(raw):
    text = raw.decode('utf-8', 'strict')
    assert 'not found' not in text
    found = set()
    for line in text.splitlines():
        parts = line.split()
        if '=>' in parts:
            index = parts.index('=>'); assert len(parts) > index + 1
            path = parts[index + 1]; assert path.startswith('/')
            found.add(path)
        elif parts and parts[0].startswith('/'): found.add(parts[0])
        else: assert parts and parts[0].startswith(('linux-vdso', 'linux-gate')), 'unknown loader listing'
    assert found and len(found) <= 32
    return found


def runtime_gate(root, requirements):
    libraries = set(); listings = []
    for name in sorted(requirements):
        path = str(Path(root) / 'bin' / name)
        verify = subprocess.run(['/lib/ld-linux-aarch64.so.1', '--verify', path], env=ENV,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
        assert verify.returncode == 0 and not verify.stdout and not verify.stderr, 'loader verification rejected'
        listed = subprocess.run(['/lib/ld-linux-aarch64.so.1', '--list', path], env=ENV,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
        assert listed.returncode == 0 and not listed.stderr and len(listed.stdout) <= 16384, 'loader/version dependency rejected'
        libraries.update(listed_libraries(listed.stdout))
        listings.append(dict(name=name, stdout=listed.stdout.decode(), exitCode=0))
    exports = set(); rows = []
    for path in sorted(libraries):
        resolved = Path(path).resolve()
        assert any(str(resolved).startswith(prefix) for prefix in ('/lib/', '/lib64/', '/usr/lib/'))
        meta = executable(path, require_execute=False)
        assert meta['bytes'] <= 32 * 1024**2
        completed = subprocess.run(['/usr/bin/readelf', '-W', '--dyn-syms', str(resolved)], env=ENV,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
        assert completed.returncode == 0 and not completed.stderr and len(completed.stdout) <= 2 * 1024**2
        exports.update(exported_symbols(completed.stdout))
        rows.append(dict(metadata=meta, dynsymSha256=hashlib.sha256(completed.stdout).hexdigest()))
    for name, row in requirements.items():
        assert set(row['symbols']) <= exports, 'required strong/versioned symbols missing: ' + name
        sonames = {Path(path).name for path in libraries}
        assert set(row['needed']) <= sonames, 'DT_NEEDED name not resolved: ' + name
    return dict(loaderListings=listings, libraries=rows, symbolRequirementsSatisfied=True,
                testExecutableMainRun=False, noDependencyInstall=True)
