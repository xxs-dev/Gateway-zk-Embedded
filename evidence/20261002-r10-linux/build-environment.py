"""L2 fixed builder/owner inspection. No remote writes or namespace creation."""
import ctypes
import errno
import os
from pathlib import Path
import platform
import re
import shutil
import stat
import subprocess
import sys

SHARED = '/srv/build/Gateway-zk'
COMPILER_BIN = '/home/tronlong/Linux/SZR/aarch64/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu/bin'
ROOT = '/srv/build/gw-r10-68de70f-build-L2'
GIT = '/usr/bin/git'
GIT_ENV = dict(PATH='/usr/bin:/bin', HOME='/nonexistent', XDG_CONFIG_HOME='/nonexistent', LC_ALL='C',
               GIT_OPTIONAL_LOCKS='0', GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL='/dev/null',
               GIT_ATTR_NOSYSTEM='1', GIT_NO_LAZY_FETCH='1')
BUILD_ENV = dict(PATH=COMPILER_BIN + ':/usr/local/bin:/usr/bin:/bin', HOME='/nonexistent', LC_ALL='C')


def directory(path, uid, gid):
    item = Path(path); info = item.lstat()
    assert stat.S_ISDIR(info.st_mode) and not item.is_symlink() and item.resolve() == item
    assert info.st_uid == uid and info.st_gid == gid and not info.st_mode & 0o022
    return dict(path=path, uid=uid, gid=gid, dev=info.st_dev, ino=info.st_ino, mode=stat.S_IMODE(info.st_mode))


def owner_identity():
    import pwd
    account = pwd.getpwuid(1000)
    assert account.pw_name == 'tronlong' and account.pw_gid == 1000
    return [directory(SHARED, 1000, 1000), directory(SHARED + '/.git', 1000, 1000)]


def git_read(args, owner):
    assert owner_identity() == owner, 'shared owner/path identity drift'
    allowed = (['status', '--porcelain=v1', '--untracked-files=all', '--ignore-submodules=all'],
               ['rev-parse', 'HEAD'],
               ['config', '--name-only', '--get-regexp', r'^(filter\..*\.(clean|smudge|process)|diff\..*\.textconv)$'])
    assert args in allowed, 'read-only Git vocabulary'
    def become_owner():
        os.setgroups([]); os.setgid(1000); os.setuid(1000)
        assert os.getuid() == os.geteuid() == 1000
    argv = [GIT, '--no-optional-locks', '-c', 'core.fsmonitor=false', '-c', 'core.hooksPath=/dev/null',
            '-c', 'core.untrackedCache=false', '-c', 'core.preloadIndex=false', '-c', 'diff.external=',
            '-c', 'diff.renames=false', '-C', SHARED] + args
    result = subprocess.run(argv, env=GIT_ENV, preexec_fn=become_owner, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=15)
    assert len(result.stdout) <= 65536 and len(result.stderr) <= 16384
    assert owner_identity() == owner, 'shared owner drift during Git read'
    return result


def inspect_environment(require_absent=True):
    result = dict(python=sys.version, architecture=platform.machine(), uid=os.getuid(), euid=os.geteuid(), paths=[])
    assert sys.version_info >= (3, 6) and result['architecture'] == 'x86_64' and result['uid'] == result['euid'] == 0
    assert all(hasattr(os, name) for name in ('fork', 'waitid', 'WNOWAIT', 'setgroups', 'setgid', 'setuid'))
    libc = ctypes.CDLL(None)
    assert hasattr(libc, 'prctl') and hasattr(libc, 'unshare')
    result['bootId'] = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
    result['parent'] = directory('/srv/build', 0, 0)
    result['owner'] = owner_identity()
    cmake = shutil.which('cmake', path=BUILD_ENV['PATH'])
    assert cmake in ('/usr/local/bin/cmake', '/usr/bin/cmake'), 'unknown existing CMake route'
    bash = shutil.which('bash', path=BUILD_ENV['PATH'])
    assert bash in ('/bin/bash', '/usr/bin/bash'), 'unknown existing bash route'
    result['cmakeExecutable'] = cmake
    result['bashExecutable'] = bash
    for path in ('/usr/bin/python3', GIT, '/usr/bin/unshare', cmake, bash,
                 COMPILER_BIN, COMPILER_BIN + '/aarch64-linux-gnu-g++', COMPILER_BIN + '/aarch64-linux-gnu-readelf',
                 COMPILER_BIN + '/aarch64-linux-gnu-strip'):
        item = Path(path); info = item.lstat(); resolved = item.resolve(); target = resolved.stat()
        assert target.st_uid in (0, 1000) and not target.st_mode & 0o6022
        assert (stat.S_ISREG(target.st_mode) and os.access(str(resolved), os.X_OK)) or stat.S_ISDIR(target.st_mode)
        if stat.S_ISREG(target.st_mode):
            try: capabilities = os.getxattr(str(resolved), 'security.capability')
            except OSError as error:
                assert error.errno in (errno.ENODATA, errno.ENOTSUP)
                capabilities = b''
            assert not capabilities, 'exec privilege/PDEATHSIG reset risk'
        result['paths'].append(dict(path=path, realPath=str(resolved), uid=info.st_uid, mode=stat.S_IMODE(info.st_mode),
                                    dev=target.st_dev, ino=target.st_ino, targetUid=target.st_uid, targetMode=stat.S_IMODE(target.st_mode)))
    if require_absent: assert not os.path.lexists(ROOT), 'L2 root occupied'
    mem = [line.split() for line in Path('/proc/meminfo').read_text().splitlines() if line.startswith('MemAvailable:')]
    assert len(mem) == 1 and len(mem[0]) == 3 and mem[0][2] == 'kB' and mem[0][1].isdigit()
    result['memAvailableBytes'] = int(mem[0][1]) * 1024
    result['freeBytes'] = os.statvfs('/srv/build').f_bavail * os.statvfs('/srv/build').f_frsize
    assert result['memAvailableBytes'] >= 5 * 1024**3 and result['freeBytes'] >= 2 * 1024**3
    result['commands'] = []
    for argv in ([GIT, '--version'], ['/usr/bin/unshare', '--version'], ['/usr/bin/unshare', '--help'],
                 [cmake, '--version'], [bash, '--version'], [COMPILER_BIN + '/aarch64-linux-gnu-g++', '--version']):
        completed = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5, env=BUILD_ENV)
        assert completed.returncode == 0 and len(completed.stdout) <= 16384 and len(completed.stderr) <= 4096
        if argv[0] == cmake:
            version = re.search(rb'cmake version (\d+)\.(\d+)', completed.stdout)
            assert version and tuple(map(int, version.groups())) >= (3, 20), 'CMake below source minimum'
        if argv[0].endswith('/aarch64-linux-gnu-g++'): assert b'6.3.1' in completed.stdout
        result['commands'].append(dict(argv=argv, exitCode=completed.returncode, stdout=completed.stdout.decode(), stderr=completed.stderr.decode()))
    result['namespaceRoute'] = 'libc CLONE_NEWPID/fork/PDEATHSIG, not unshare --kill-child'
    result['namespaceCreated'] = False; result['remoteWrites'] = 0
    return result
