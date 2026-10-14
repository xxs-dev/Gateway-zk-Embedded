#!/usr/bin/env python3
"""Run the unmodified reprovision CLI with private paths and a fake manager.

No installed gateway service or binary is executed. This tests CLI/lock/path
boundaries, not a real systemd manager or power-loss durability.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import time

HELPERS = ('gateway-reprovision.py', 'offline-runtime-upgrade.py', 'runtime-upgrade-guard.py')
HOME = Path('/opt/modbus-gateway')
ROOT = Path('/opt/gateway-reprovision')
FIXTURE = Path('/opt/reprovision-cli-fixture')
UNITS = Path('/etc/systemd/system')

MANAGER = r'''#!/usr/bin/python3
import hashlib
import json
from pathlib import Path
import sys
root = Path('/opt/reprovision-cli-fixture')
units = Path('/etc/systemd/system')
args = sys.argv[1:]
with (root / 'systemctl.log').open('a') as stream:
    stream.write(json.dumps(args) + '\n')
def inventory():
    return {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in units.rglob('*') if p.is_file()}
if args[0] == 'daemon-reload':
    (root / 'manager.json').write_text(json.dumps(inventory()))
elif args[0] in ('list-units', 'list-unit-files'):
    names = {p.name for p in units.glob('*.service')}
    names.update(p.name[:-2] for p in units.glob('*.service.d'))
    for name in sorted(names):
        print(name + ' inactive')
elif args[0] == 'stop':
    if (root / 'fail-stop').exists():
        sys.exit('injected stop failure')
elif len(args) == 4 and args[0] == 'show' and args[2] == '--value':
    prop, name = args[1], args[3]
    template = name.split('@')[0] + '@.service'
    if prop == '--property=LoadState':
        print('loaded' if (units / name).exists() or '@' in name and (units / template).exists()
              else 'not-found')
    elif prop == '--property=ActiveState':
        print('inactive')
    elif prop == '--property=MainPID':
        print('0')
    elif prop == '--property=NeedDaemonReload':
        print('no' if inventory() == json.loads((root / 'manager.json').read_text()) else 'yes')
    elif prop == '--property=DropInPaths':
        loaded = json.loads((root / 'manager.json').read_text())
        print(' '.join(p for p in loaded if str(Path(p).parent) == str(units / (name + '.d'))))
    else:
        sys.exit('unexpected property: ' + repr(args))
else:
    sys.exit('unexpected operation: ' + repr(args))
'''


def write(path, data, mode=0o644):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data if isinstance(data, bytes) else data.encode())
    path.chmod(mode)


def snapshot(root, ignore_fences=False):
    result = {}
    for path in sorted(root.rglob('*')):
        if path.is_file() and not (ignore_fences and path.name == 'zz-gateway-reprovision-stop.conf'):
            result[str(path.relative_to(root))] = {
                'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                'mode': path.stat().st_mode & 0o7777}
    return result


def command(*args, ok=True):
    env = dict(os.environ, PATH=str(FIXTURE / 'fake-bin') + ':/usr/sbin:/usr/bin:/sbin:/bin',
               PYTHONDONTWRITEBYTECODE='1', LC_ALL='C.UTF-8')
    result = subprocess.run([sys.executable, '-B', str(FIXTURE / 'deploy/gateway-reprovision.py'),
                             *map(str, args)], env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, universal_newlines=True, timeout=180)
    print('$ gateway-reprovision.py', ' '.join(map(str, args)), flush=True)
    print(result.stdout, flush=True)
    if (result.returncode == 0) != ok:
        raise RuntimeError('unexpected CLI exit: ' + str(result.returncode))
    return result.stdout


def inner(options):
    parent = json.loads(options.parent_namespaces)
    for name in ('mnt', 'net', 'pid', 'ipc'):
        if os.readlink('/proc/self/ns/' + name) == parent[name]:
            raise RuntimeError('namespace not isolated: ' + name)
    backing = options.private_backing.resolve(strict=True)
    subprocess.run(['mount', '--make-rprivate', '/'], check=True)
    for name in ('usr', 'bin', 'sbin', 'lib', 'lib64'):
        source, target = Path('/') / name, backing / name
        if source.is_symlink():
            target.symlink_to(os.readlink(str(source)))
        elif source.is_dir():
            target.mkdir()
            subprocess.run(['mount', '--bind', str(source), str(target)], check=True)
            subprocess.run(['mount', '-o', 'remount,bind,ro', str(target)], check=True)
    for name in ('run', 'tmp', 'dev'):
        target = backing / name
        target.mkdir()
        subprocess.run(['mount', '-t', 'tmpfs', '-o', 'mode=755,nosuid', 'tmpfs', str(target)], check=True)
    for name, minor in (('null', 3), ('zero', 5), ('random', 8), ('urandom', 9)):
        os.mknod(str(backing / 'dev' / name), stat.S_IFCHR | 0o666, os.makedev(1, minor))
    (backing / 'dev/shm').mkdir()
    (backing / 'proc').mkdir()
    subprocess.run(['mount', '-t', 'proc', 'proc', str(backing / 'proc')], check=True)
    shutil.copyfile('/etc/ld.so.cache', str(backing / 'etc/ld.so.cache'))
    os.chroot(str(backing))
    os.chdir('/')
    targets = ['/dev/shm', '/usr/lib/systemd/system', '/usr/local/lib/systemd/system']
    # /lib may be a merged-/usr alias; never mount the same vendor tree twice.
    if Path('/lib/systemd/system').resolve() != Path('/usr/lib/systemd/system').resolve():
        targets.append('/lib/systemd/system')
    for target in targets:
        if Path(target).exists():
            subprocess.run(['mount', '-t', 'tmpfs', '-o', 'mode=755,nosuid,nodev', 'tmpfs', target], check=True)
    if Path('/run/systemd/private').exists() or Path('/run/dbus/system_bus_socket').exists():
        raise RuntimeError('host manager socket visible')
    if list(Path('/dev/shm').iterdir()):
        raise RuntimeError('host SHM visible')
    UNITS.mkdir(parents=True)
    image, payload = FIXTURE / 'image', FIXTURE / 'units'
    for home, version in ((HOME, 'old'), (image, 'new')):
        for name in ('bin', 'config/runtime', 'data', 'run'):
            (home / name).mkdir(parents=True)
        write(home / 'config/runtime/device_identity.json', '{"machineCode":"CLI_FIXTURE"}\n', 0o600)
        write(home / 'bin/gateway-services.sh', '#!/bin/sh\nexit 96 # ' + version + '\n', 0o755)
        write(home / 'bin/runtime-upgrade-guard.py', '# fixture ' + version + '\n')
    write(HOME / 'data/retained-history.json', '{"preserve":true}\n', 0o600)
    write(UNITS / 'gateway-services.service', '[Service]\nExecStart=/bin/false\n')
    write(UNITS / 'system-monitor@.service', '[Service]\nExecStart=/bin/false\n')
    write(UNITS / 'system-monitor@dormant.service.d/10-existing.conf', '[Service]\nTasksMax=4\n')
    write(payload / 'gateway-services.service', '[Service]\nExecStart=/bin/false\n# new\n')
    write(payload / 'ky-ems.service', '[Service]\nExecStart=/bin/false\n')
    write(FIXTURE / 'fake-bin/systemctl', MANAGER, 0o755)
    old, old_units = snapshot(HOME), snapshot(UNITS)
    image_id = HOME.stat().st_ino

    def prepare(name):
        path = FIXTURE / (name + '.json')
        output = command('plan', '--image', image, '--units', payload, '--transaction', name,
                         '--expires-at', int(time.time()) + 3600, '--output', path)
        pin = hashlib.sha256(path.read_bytes()).hexdigest()
        assert output.strip() == pin
        approval = json.loads(path.read_text())
        assert 'system-monitor@dormant.service' in approval['units']
        return path, pin

    approval, pin = prepare('CLI_ONE')
    assert snapshot(HOME) == old and not (ROOT / 'STOP').exists()
    assert not (ROOT / 'pending.json').exists()
    command('apply', '--approval', approval, '--approval-sha256', '0' * 64, ok=False)
    assert snapshot(HOME) == old and not (ROOT / 'STOP').exists()
    subprocess.run(['mount', '--bind', str(UNITS), str(UNITS)], check=True)
    try:
        refused = command('apply', '--approval', approval, '--approval-sha256', pin, ok=False)
        assert 'one mount, not separate bind mounts' in refused
        assert snapshot(HOME) == old and not (ROOT / 'STOP').exists()
        assert not (ROOT / 'pending.json').exists()
    finally:
        subprocess.run(['umount', str(UNITS)], check=True)
    command('apply', '--approval', approval, '--approval-sha256', pin)
    assert snapshot(HOME) == snapshot(image)
    assert snapshot(ROOT / 'transactions/CLI_ONE/previous-home') == old
    assert (ROOT / 'STOP').is_file() and not (ROOT / 'pending.json').exists()
    command('recover', '--transaction', 'CLI_ONE', '--approval-sha256', pin)
    command('recover', '--transaction', 'CLI_ONE', '--approval-sha256', pin)
    assert snapshot(HOME) == old and HOME.stat().st_ino == image_id
    assert snapshot(UNITS, ignore_fences=True) == old_units
    assert not (ROOT / 'current.json').exists() and not (ROOT / 'pending.json').exists()
    assert snapshot(ROOT / 'transactions/CLI_ONE/retired-candidate') == snapshot(image)

    approval, pin = prepare('CLI_STOP_FAILURE')
    write(FIXTURE / 'fail-stop', '')
    command('apply', '--approval', approval, '--approval-sha256', pin, ok=False)
    assert snapshot(HOME) == old and (ROOT / 'pending.json').is_file()
    assert (ROOT / 'STOP').is_file()
    (FIXTURE / 'fail-stop').unlink()
    command('recover', '--transaction', 'CLI_STOP_FAILURE', '--approval-sha256', pin)
    assert snapshot(HOME) == old and not (ROOT / 'pending.json').exists()
    calls = [json.loads(line) for line in (FIXTURE / 'systemctl.log').read_text().splitlines()]
    assert all(call[0] not in ('start', 'restart', 'enable', 'reset-failed') for call in calls)
    assert not list(Path('/dev/shm').iterdir())
    print('CLI_PRIVATE_PATHS_LOCK_APPLY_RECOVER_STOP_FAILURE_PASS', flush=True)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--private-backing', type=Path, help=argparse.SUPPRESS)
    parser.add_argument('--parent-namespaces', help=argparse.SUPPRESS)
    options = parser.parse_args()
    if os.geteuid() != 0 or os.uname().machine != 'aarch64':
        parser.error('requires AArch64 root for private mount fixtures')
    if options.private_backing:
        if not options.parent_namespaces:
            parser.error('private namespace proof missing')
        return inner(options)
    repo = Path(__file__).resolve().parents[1]
    namespaces = {name: os.readlink('/proc/self/ns/' + name) for name in ('mnt', 'net', 'pid', 'ipc')}
    with tempfile.TemporaryDirectory(prefix='gateway-reprovision-cli-', dir='/var/tmp') as temp:
        backing = Path(temp)
        stage = backing / 'opt/reprovision-cli-fixture'
        (stage / 'deploy').mkdir(parents=True)
        (backing / 'etc').mkdir()
        for name in HELPERS:
            shutil.copyfile(str(repo / 'deploy' / name), str(stage / 'deploy' / name))
        entry = stage / 'cli-test.py'
        shutil.copyfile(str(Path(__file__).resolve()), str(entry))
        return subprocess.call(['unshare', '--mount', '--net', '--ipc', '--pid', '--mount-proc', '--fork',
                                sys.executable, '-B', str(entry), '--private-backing', str(backing),
                                '--parent-namespaces', json.dumps(namespaces)])


if __name__ == '__main__':
    sys.exit(main())
