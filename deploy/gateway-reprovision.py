#!/usr/bin/env python3
"""Journaled whole-image replacement. Install/recovery always remain stopped.

This is separate from same-ABI production-install. It never starts a service,
deletes an old image, clears SHM, or grants permission for physical output.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import sys
import time
import uuid

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('reprovision_lifecycle',
                                            str(Path(__file__).with_name('offline-runtime-upgrade.py')))
lifecycle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lifecycle)
guard = lifecycle.guard
require = lifecycle.require
HOME = Path('/opt/modbus-gateway')
ROOT = Path('/opt/gateway-reprovision')
BOOTSTRAP_ROOT = Path('/opt/gateway-runtime-bootstrap')
UNIT_DIR = Path('/etc/systemd/system')
FENCE_NAME = 'zz-gateway-reprovision-stop.conf'
STOP_BODY = b'gateway full reprovision: explicit activation required\n'
IMMUTABLE = ('bin', 'config', 'ky-ems', 'scada')
MAX_FILE = 256 * 1024 * 1024
MAX_TREE = 2 * 1024 * 1024 * 1024


def trusted(path, directory=False):
    path = Path(path)
    require(path.is_absolute(), 'absolute path required')
    for part in (path,) + tuple(path.parents):
        info = part.lstat()
        require(not stat.S_ISLNK(info.st_mode) and info.st_uid == 0 and
                not info.st_mode & 0o022, 'untrusted path: ' + str(part))
    info = path.stat()
    require(stat.S_ISDIR(info.st_mode) if directory else
            stat.S_ISREG(info.st_mode) and info.st_nlink == 1,
            'unexpected file type: ' + str(path))
    return info


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(65536), b''):
            result.update(block)
    return result.hexdigest()


def encoded(value):
    return (json.dumps(value, sort_keys=True, indent=2) + '\n').encode()


def record(path, value, new=False):
    data = encoded(value)
    if new:
        lifecycle.write_new(path, data)
    else:
        if path.exists() or path.is_symlink():
            trusted(path)
        temporary = path.with_name('.' + path.name + '.' + uuid.uuid4().hex)
        lifecycle.write_new(temporary, data)
        os.replace(str(temporary), str(path))
        lifecycle.sync_dir(path.parent)


def read_record(path, expected=None):
    info = trusted(path)
    require(info.st_size <= 4 * 1024 * 1024, 'record too large')
    if expected is not None:
        require(digest(path) == expected, 'record digest mismatch: ' + str(path))
    return guard.production_json(path)


def identity(path):
    info = trusted(path, directory=True)
    return {'dev': info.st_dev, 'ino': info.st_ino}


def mount_id(path):
    fd = os.open(str(path), os.O_PATH | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        fields = dict(line.split(':', 1) for line in Path('/proc/self/fdinfo/' + str(fd)).read_text().splitlines())
        require('mnt_id' in fields, 'kernel mount identity unavailable')
        return int(fields['mnt_id'].strip())
    finally:
        os.close(fd)


def no_mounts(root):
    for line in Path('/proc/self/mountinfo').read_text().splitlines():
        name = re.sub(r'\\([0-7]{3})', lambda m: chr(int(m.group(1), 8)), line.split()[4])
        path = Path(name)
        require(path != root and root not in path.parents, 'mounted image path: ' + name)


def tree(root, selected=None):
    trusted(root, directory=True)
    no_mounts(root)
    result, total = {}, 0
    paths = [root]
    if selected is None:
        paths += sorted(root.rglob('*'))
    else:
        for name in selected:
            path = root / name
            if path.exists() or path.is_symlink():
                require(path.is_dir() and not path.is_symlink(), 'runtime tree root is not a regular directory')
                paths += [path] + sorted(path.rglob('*'))
    require(len(paths) <= 20000, 'image has too many entries')
    for path in paths:
        info = path.lstat()
        name = '.' if path == root else str(path.relative_to(root))
        require(info.st_uid == 0 and info.st_gid == 0 and
                (stat.S_ISLNK(info.st_mode) or not stat.S_IMODE(info.st_mode) & ~0o755),
                'image ownership/mode unsafe: ' + name)
        row = {'mode': stat.S_IMODE(info.st_mode)}
        if stat.S_ISDIR(info.st_mode):
            row['kind'] = 'directory'
        elif stat.S_ISREG(info.st_mode):
            require(info.st_nlink == 1 and info.st_size <= MAX_FILE, 'image file bound/type invalid')
            row.update(kind='file', bytes=info.st_size, sha256=digest(path))
            after = path.stat()
            require((info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns) ==
                    (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns),
                    'image changed while hashing')
            total += info.st_size
        elif stat.S_ISLNK(info.st_mode):
            target = PurePosixPath(os.readlink(str(path)))
            require(not target.is_absolute() and '..' not in target.parts and
                    root in path.resolve(strict=True).parents, 'image symlink escapes or dangles: ' + name)
            row.update(kind='symlink', target=str(target))
        else:
            raise ValueError('special image entry: ' + name)
        result[name] = row
    require(total <= MAX_TREE, 'image exceeds size bound')
    return result


def immutable(rows):
    return {name: pin for name, pin in rows.items() if name.split('/')[0] in IMMUTABLE}


def sync_directories(root):
    for directory in sorted((p for p in root.rglob('*') if p.is_dir() and not p.is_symlink()),
                            key=lambda p: len(p.parts), reverse=True):
        lifecycle.sync_dir(directory)
    lifecycle.sync_dir(root)
    lifecycle.sync_dir(root.parent)


def copy_tree(source, destination, expected):
    require(not destination.exists() and not destination.is_symlink(), 'staging destination occupied')
    destination.mkdir(mode=expected['.']['mode'])
    destination.chmod(expected['.']['mode'])
    for name, pin in sorted(expected.items(), key=lambda item: (item[0].count('/'), item[0])):
        if name == '.':
            continue
        target = destination / name
        if pin['kind'] == 'directory':
            target.mkdir(mode=pin['mode'])
            target.chmod(pin['mode'])
        elif pin['kind'] == 'symlink':
            target.symlink_to(pin['target'])
        else:
            lifecycle.production_copy(source / name, target,
                                      {k: pin[k] for k in ('sha256', 'bytes', 'mode')}, new=True)
    sync_directories(destination)
    require(tree(destination) == expected and tree(source) == expected, 'staged image/source drift')


def unit_name(name):
    return bool(guard.PRODUCTION_UNIT.fullmatch(name) or name == guard.PRODUCTION_RECOVERY_UNIT)


def unit_relative(name):
    parts = PurePosixPath(name).parts
    require(len(parts) in (1, 2) and all(p not in ('.', '..', '') for p in parts), 'unit path invalid')
    name = parts[0] if len(parts) == 1 else parts[0][:-2]
    require(unit_name(name) and (len(parts) == 1 or parts[0].endswith('.service.d') and
            re.fullmatch(r'[A-Za-z0-9_.-]+\.conf', parts[1]) and parts[1] != FENCE_NAME),
            'unit outside gateway ownership')
    return name


def owned_unit_path(path):
    path = Path(path)
    if UNIT_DIR not in path.parents:
        return False
    parts = path.relative_to(UNIT_DIR).parts
    return (len(parts) == 1 and unit_name(parts[0]) or len(parts) == 2 and
            parts[0].endswith('.service.d') and unit_name(parts[0][:-2]) and
            parts[1].endswith('.conf') and parts[1] != FENCE_NAME)


def fence_body():
    return ('[Unit]\nConditionPathExists=!' + str(ROOT / 'STOP') + '\n').encode()


def check_unit_text(path):
    text = path.read_text(encoding='utf-8').replace('\\\n', ' ')
    # An empty reset could erase the persistent stop condition after a reload.
    require(not re.search(r'^\s*Condition[A-Za-z]+\s*=\s*(?:""|\'\')?\s*(?:[#;].*)?$', text, re.MULTILINE),
            'unit resets the reprovision stop condition: ' + str(path))


def candidate_units(source):
    trusted(source, directory=True)
    result = {}
    for path in sorted(source.rglob('*')):
        trusted(path, directory=path.is_dir())
        if path.is_dir():
            require(path.parent == source and path.name.endswith('.service.d') and
                    unit_name(path.name[:-2]), 'unexpected unit payload directory')
            continue
        relative = str(path.relative_to(source))
        unit_relative(relative)
        require(path.stat().st_size <= 1024 * 1024, 'unit too large')
        check_unit_text(path)
        result[relative] = guard.production_pin(path)
    require(result and 'gateway-services.service' in result and len(result) <= 512,
            'gateway launcher/full bounded unit payload required')
    return result


def present_units():
    result = set()
    for action in ('list-units', 'list-unit-files'):
        for line in lifecycle.systemctl(action, '--all', '--no-legend', '--no-pager', '--plain').splitlines():
            fields = line.split()
            if fields and unit_name(fields[0]):
                result.add(fields[0])
    # systemd may have garbage-collected inactive instances whose drop-ins remain.
    for location in guard.SYSTEMD_ROOTS:
        base = Path(location)
        if base.exists():
            for path in base.iterdir():
                if unit_name(path.name):
                    result.add(path.name)
                elif path.name.endswith('.service.d') and unit_name(path.name[:-2]):
                    result.add(path.name[:-2])
    return result


def installed_units(units):
    names = set(units)
    names.update(u.split('@')[0] + '@.service' for u in units if '@' in u)
    directories = {'service.d'} | {u + '.d' for u in names}
    for unit in names:
        prefix = unit.split('@')[0]
        directories.update(prefix[:i + 1] + '.service.d' for i, c in enumerate(prefix) if c == '-')
    result = {}
    for location in guard.SYSTEMD_ROOTS:
        base = Path(location)
        if location == '/lib/systemd/system' and base.resolve() == Path('/usr/lib/systemd/system').resolve():
            continue
        if not base.exists():
            continue
        for path in base.iterdir():
            selected = []
            if unit_name(path.name):
                require(path.name in names, 'uncovered gateway service')
                selected = [path]
            elif path.name in directories or path.name.endswith('.service.d') and unit_name(path.name[:-2]):
                require(path.name in directories, 'uncovered gateway override')
                trusted(path, directory=True)
                selected = list(path.iterdir())
                require(all(p.name.endswith('.conf') for p in selected), 'uncovered override entry')
            for child in selected:
                pin = guard.production_pin(child)
                if child.name == FENCE_NAME:
                    require(child.parent.parent == UNIT_DIR and child.read_bytes() == fence_body(),
                            'foreign reprovision fence')
                else:
                    check_unit_text(child)
                    result[str(child)] = pin
    return result


def processes():
    found = []
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit() or int(entry.name) == os.getpid():
            continue
        try:
            exe = os.readlink(str(entry / 'exe'))
            if exe.endswith(' (deleted)'):
                exe = exe[:-10]
            argv = (entry / 'cmdline').read_bytes().split(b'\0')
        except (FileNotFoundError, ProcessLookupError):
            continue
        if Path(exe).name in guard.RUNTIME_BINARIES or any(
                item.startswith(os.fsencode(str(HOME)) + b'/') for item in argv[:4]):
            found.append(entry.name)
    return found


def root_setup():
    require(os.geteuid() == 0 and os.uname().machine == 'aarch64', 'AArch64 root operation required')
    require(HOME not in Path(__file__).absolute().parents,
            'stage the paired reprovision helpers outside the live installation first')
    for path in (ROOT, ROOT / 'transactions'):
        if not path.exists():
            trusted(path.parent, directory=True)
            path.mkdir(mode=0o700)
            lifecycle.sync_dir(path.parent)
        require(stat.S_IMODE(trusted(path, directory=True).st_mode) == 0o700, 'private journal root required')
    trusted(UNIT_DIR, directory=True)
    require(HOME.parent.stat().st_dev == ROOT.stat().st_dev == UNIT_DIR.stat().st_dev,
            'image and unit switching require one filesystem')
    require(mount_id(HOME.parent) == mount_id(ROOT) == mount_id(UNIT_DIR),
            'image and unit switching require one mount, not separate bind mounts')


def current_pointer():
    path = ROOT / 'current.json'
    if not path.exists():
        require(not path.is_symlink(), 'symlink current pointer')
        return None
    pointer = read_record(path)
    require(isinstance(pointer, dict) and set(pointer) == {'transaction', 'approvalSha256'} and
            re.fullmatch('[A-Za-z0-9_-]{1,63}', pointer['transaction']) and
            re.fullmatch('[0-9a-f]{64}', pointer['approvalSha256']), 'invalid current pointer')
    transaction = ROOT / 'transactions' / pointer['transaction']
    approval = read_record(transaction / 'approval.json', pointer['approvalSha256'])
    state = read_record(transaction / 'state.json')
    require(state['phase'] == 'INSTALLED_STOPPED' and state['approvalSha256'] == pointer['approvalSha256'] and
            state['transaction'] == approval['transaction'] == pointer['transaction'] and
            identity(HOME) == state['newIdentity'], 'current pointer has no matching installed image')
    return pointer


def helper_pins():
    result = {}
    for path in (Path(__file__), Path(lifecycle.__file__), Path(guard.__file__)):
        trusted(path)
        result[path.name] = digest(path)
    return result


def capacity_preflight(approval):
    size = sum(row.get('bytes', 0) for row in approval['imageTree'].values())
    size += sum(row['bytes'] for row in approval['newUnits'].values())
    size += sum(row['bytes'] for row in approval['previousUnits'].values())
    size += 4096 * len(approval['imageTree']) + 256 * 1024 * 1024
    available = os.statvfs(str(ROOT))
    require(available.f_bavail * available.f_frsize >= size, 'insufficient durable staging space')


def bootstrap_journal_snapshot():
    for path in (BOOTSTRAP_ROOT / 'pending.json', BOOTSTRAP_ROOT / 'activations/pending.json',
                 HOME / 'data/runtime-production/pending.json'):
        require(not path.exists() and not path.is_symlink(), 'pending bootstrap/runtime transaction cannot be retired')
    trusted(BOOTSTRAP_ROOT, directory=True)
    require(mount_id(BOOTSTRAP_ROOT) == mount_id(ROOT) and
            BOOTSTRAP_ROOT.stat().st_dev == ROOT.stat().st_dev, 'bootstrap journal must share the reprovision mount')
    pointer = read_record(BOOTSTRAP_ROOT / 'activations/current.json')
    require(isinstance(pointer, dict) and set(pointer) == {'transaction', 'approvalSha256'} and
            isinstance(pointer['transaction'], str) and re.fullmatch('[A-Za-z0-9_-]{1,63}', pointer['transaction']) and
            isinstance(pointer['approvalSha256'], str) and re.fullmatch('[0-9a-f]{64}', pointer['approvalSha256']),
            'invalid completed bootstrap pointer')
    transaction = BOOTSTRAP_ROOT / 'activations' / pointer['transaction']
    approval = read_record(transaction / 'approval.json', pointer['approvalSha256'])
    state = read_record(transaction / 'state.json')
    require(state.get('phase') == 'COMPLETED' and state.get('transaction') == pointer['transaction'] and
            state.get('approvalSha256') == pointer['approvalSha256'] and
            approval.get('format') == 'runtime-bootstrap-activation-v1' and approval.get('operation') == 'bootstrap' and
            approval.get('transaction') == pointer['transaction'] and approval.get('gatewayHome') == str(HOME),
            'bootstrap activation is not completed')
    origin = approval.get('origin', {})
    require({key: origin.get(key) for key in ('transaction', 'approvalSha256')} == current_pointer() and
            origin.get('imageIdentity') == identity(HOME), 'bootstrap journal belongs to a different installed image')
    prepared = read_record(BOOTSTRAP_ROOT / 'current.json')
    require(prepared == dict(transaction=approval.get('preparedTransaction'),
                            approvalSha256=approval.get('prepareApprovalSha256'),
                            readySha256=approval.get('preparedSha256')) and
            isinstance(prepared['transaction'], str) and re.fullmatch('[A-Za-z0-9_-]{1,63}', prepared['transaction']) and
            all(isinstance(prepared[k], str) and re.fullmatch('[0-9a-f]{64}', prepared[k])
                for k in ('approvalSha256', 'readySha256')),
            'bootstrap preparation lineage mismatch')
    directory = BOOTSTRAP_ROOT / 'transactions' / prepared['transaction']
    read_record(directory / 'approval.json', prepared['approvalSha256'])
    read_record(directory / 'ready.json', prepared['readySha256'])
    current = read_record(HOME / 'data/runtime-production/current.json')
    require(current.get('transaction') == pointer['transaction'] and isinstance(current.get('baselineId'), str) and
            re.fullmatch('[A-Za-z0-9_-]{1,63}', current['baselineId']) and
            isinstance(current.get('sha256'), str) and re.fullmatch('[0-9a-f]{64}', current['sha256']),
            'bootstrap retirement requires the original completed baseline')
    require(read_record(HOME / 'data/runtime-production/baselines' / (current['baselineId'] + '.json'),
                        current['sha256']) == approval.get('candidate'), 'bootstrap baseline changed')
    return {'identity': identity(BOOTSTRAP_ROOT), 'tree': tree(BOOTSTRAP_ROOT)}


def bootstrap_recovery_location(transaction, approval):
    expected = approval.get('bootstrapJournal')
    require(approval['format'] == 'gateway-reprovision-approval-v2' and isinstance(expected, dict) and
            set(expected) == {'identity', 'tree'}, 'invalid bootstrap journal recovery authority')
    archived = transaction / 'previous-bootstrap'
    existing = [path for path in (BOOTSTRAP_ROOT, archived) if path.exists() or path.is_symlink()]
    require(len(existing) == 1 and identity(existing[0]) == expected['identity'] and
            tree(existing[0]) == expected['tree'], 'bootstrap journal missing, changed or replaced; keep stopped')
    return existing[0]


def plan(image, unit_source, transaction, expires, retire_bootstrap=False):
    require(re.fullmatch('[A-Za-z0-9_-]{1,63}', transaction), 'invalid transaction ID')
    require(time.time() < expires <= time.time() + 86400, 'approval expiry must be within one day')
    require(not (ROOT / 'pending.json').exists(), 'unfinished reprovision transaction')
    bootstrap = bootstrap_journal_snapshot() if retire_bootstrap else None
    require(retire_bootstrap or not (BOOTSTRAP_ROOT.exists() or BOOTSTRAP_ROOT.is_symlink()),
            'existing bootstrap journal requires explicit --retire-bootstrap')
    image, unit_source = image.absolute(), unit_source.absolute()
    require(HOME not in image.parents and image != HOME and image not in HOME.parents and
            ROOT not in image.parents and image != ROOT and image not in ROOT.parents,
            'candidate overlaps live image or journal')
    require(HOME not in unit_source.parents and ROOT not in unit_source.parents,
            'unit payload overlaps live image or journal')
    new_tree, old_tree = tree(image), tree(HOME, IMMUTABLE)
    new_units = candidate_units(unit_source)
    units = present_units() | {unit_relative(name) for name in new_units}
    units |= {u.split('@')[0] + '@.service' for u in units if '@' in u}
    previous_units = installed_units(sorted(units))
    for name in previous_units:
        if owned_unit_path(name):
            unit_relative(str(Path(name).relative_to(UNIT_DIR)))
    identity_path = 'config/runtime/device_identity.json'
    old_identity = read_record(HOME / identity_path)
    new_identity = read_record(image / identity_path)
    require(old_identity.get('machineCode') and old_identity.get('machineCode') == new_identity.get('machineCode'),
            'reprovision must preserve device identity')
    require(new_tree.get(identity_path) == old_tree.get(identity_path), 'device identity bytes/mode changed')
    require(all(new_tree.get(name, {}).get('kind') == 'directory' for name in ('bin', 'config', 'data', 'run')),
            'candidate runtime layout incomplete')
    require('bin/gateway-services.sh' in new_tree and 'bin/runtime-upgrade-guard.py' in new_tree,
            'candidate startup guards missing')
    for name, pin in new_tree.items():
        require(name.split('/')[0] not in ('data', 'run', 'ota', 'tmp') or pin['kind'] == 'directory',
                'new image must not carry runtime state/queued work')
        if pin['kind'] == 'file' and name.split('/')[0] in ('bin', 'ky-ems'):
            guard.production_architecture(image / name)
    result = {'format': 'gateway-reprovision-approval-v1', 'transaction': transaction,
            'expiresAtUnix': expires, 'gatewayHome': str(HOME), 'architecture': 'aarch64',
            'controlEnabled': False, 'activation': 'stopped-only', 'machineCode': old_identity['machineCode'],
            'helperSha256': helper_pins(), 'image': str(image), 'imageTree': new_tree,
            'unitSource': str(unit_source), 'newUnits': new_units, 'units': sorted(units),
            'previousImmutable': immutable(old_tree), 'previousIdentity': identity(HOME),
            'previousUnits': previous_units,
            'previousPointer': current_pointer()}
    if bootstrap is not None:
        result.update(format='gateway-reprovision-approval-v2', bootstrapJournal=bootstrap)
    return result


def persistent_stop(units):
    path = ROOT / 'STOP'
    if path.exists():
        require(trusted(path) and path.read_bytes() == STOP_BODY, 'foreign stop marker')
    else:
        lifecycle.write_new(path, STOP_BODY)
    for unit in units:
        directory = UNIT_DIR / (unit + '.d')
        if not directory.exists():
            directory.mkdir(mode=0o755)
            lifecycle.sync_dir(directory.parent)
        trusted(directory, directory=True)
        path = directory / FENCE_NAME
        if path.exists():
            require(trusted(path) and path.read_bytes() == fence_body(), 'stop fence changed')
        else:
            lifecycle.write_new(path, fence_body(), 0o644)
    lifecycle.systemctl('daemon-reload')
    concrete = [u for u in units if not u.endswith('@.service') and
                lifecycle.systemctl('show', '--property=LoadState', '--value', u) != 'not-found']
    if concrete:
        lifecycle.systemctl('stop', *concrete)
        lifecycle.units_stopped(concrete)
        for unit in concrete:
            require(lifecycle.systemctl('show', '--property=MainPID', '--value', unit) == '0',
                    'service still has a main process: ' + unit)
            effective = lifecycle.systemctl('show', '--property=DropInPaths', '--value', unit).split()
            require(str(UNIT_DIR / (unit + '.d') / FENCE_NAME) in effective and
                    lifecycle.systemctl('show', '--property=NeedDaemonReload', '--value', unit) == 'no',
                    'service manager has not loaded the persistent stop fence: ' + unit)
    require(not processes(), 'runtime participants remain after stop')


class Reprovision:
    def checkpoint(self, transaction, state, phase):
        state['phase'] = phase
        record(transaction / 'state.json', state)

    def apply(self, path, approval_sha):
        root_setup()
        approval = read_record(path, approval_sha)
        require(approval == plan(Path(approval['image']), Path(approval['unitSource']),
                                 approval['transaction'], approval['expiresAtUnix'],
                                 retire_bootstrap=approval.get('format') == 'gateway-reprovision-approval-v2'),
                'approval drift or unrecognized fields')
        capacity_preflight(approval)
        transaction = ROOT / 'transactions' / approval['transaction']
        require(not transaction.exists(), 'transaction already consumed')
        transaction.mkdir(mode=0o700)
        lifecycle.sync_dir(transaction.parent)
        record(transaction / 'approval.json', approval, new=True)
        require(digest(transaction / 'approval.json') == approval_sha,
                'approval must use canonical JSON encoding')
        copy_tree(Path(approval['image']), transaction / 'new-home', approval['imageTree'])
        for relative, pin in approval['newUnits'].items():
            lifecycle.production_copy(Path(approval['unitSource']) / relative,
                                      transaction / 'unit-ready' / relative, pin, new=True)
        old_units = {}
        for index, (name, pin) in enumerate(sorted(approval['previousUnits'].items())):
            backup = transaction / 'old-units' / str(index)
            lifecycle.production_copy(Path(name), backup, pin, new=True)
            old_units[name] = str(backup.relative_to(transaction))
        sync_directories(transaction / 'unit-ready')
        if old_units:
            sync_directories(transaction / 'old-units')
        state = {'format': 'gateway-reprovision-state-v1', 'transaction': approval['transaction'],
                 'approvalSha256': approval_sha, 'newIdentity': identity(transaction / 'new-home'),
                 'oldUnits': old_units, 'previousTree': None}
        self.checkpoint(transaction, state, 'PREPARED')
        record(ROOT / 'pending.json', {'transaction': approval['transaction'], 'approvalSha256': approval_sha}, new=True)
        persistent_stop(approval['units'])
        require(identity(HOME) == approval['previousIdentity'] and
                immutable(tree(HOME, IMMUTABLE)) == approval['previousImmutable'] and
                installed_units(approval['units']) == approval['previousUnits'], 'old installation drift after stop')
        require(time.time() < approval['expiresAtUnix'], 'approval expired after stop')
        state['previousTree'] = tree(HOME)
        self.checkpoint(transaction, state, 'SWITCH_INTENT')
        if 'bootstrapJournal' in approval:
            require(bootstrap_journal_snapshot() == approval['bootstrapJournal'], 'bootstrap journal drift after stop')
            os.rename(str(BOOTSTRAP_ROOT), str(transaction / 'previous-bootstrap'))
            lifecycle.sync_dir(BOOTSTRAP_ROOT.parent)
            lifecycle.sync_dir(transaction)
            self.checkpoint(transaction, state, 'BOOTSTRAP_RETIRED')
        os.rename(str(HOME), str(transaction / 'previous-home'))
        lifecycle.sync_dir(HOME.parent)
        lifecycle.sync_dir(transaction)
        self.checkpoint(transaction, state, 'OLD_MOVED')
        os.rename(str(transaction / 'new-home'), str(HOME))
        lifecycle.sync_dir(HOME.parent)
        lifecycle.sync_dir(transaction)
        self.checkpoint(transaction, state, 'IMAGE_INSTALLED')
        self.switch_units(transaction, approval, state, recover=False)
        lifecycle.systemctl('daemon-reload')
        expected_units = {name: pin for name, pin in approval['previousUnits'].items()
                          if not owned_unit_path(name)}
        expected_units.update({str(UNIT_DIR / name): pin for name, pin in approval['newUnits'].items()})
        require(not processes() and tree(HOME) == approval['imageTree'] and
                installed_units(approval['units']) == expected_units, 'installed stopped image/unit drift')
        self.checkpoint(transaction, state, 'INSTALLED_STOPPED')
        record(ROOT / 'current.json', {'transaction': approval['transaction'], 'approvalSha256': approval_sha})
        (ROOT / 'pending.json').unlink()
        lifecycle.sync_dir(ROOT)
        return transaction

    def switch_units(self, transaction, approval, state, recover):
        old = {name: pin for name, pin in approval['previousUnits'].items() if owned_unit_path(name)}
        new = {str(UNIT_DIR / name): pin for name, pin in approval['newUnits'].items()}
        for name in sorted(set(old) | set(new)):
            target = Path(name)
            actual = guard.production_pin(target) if target.exists() or target.is_symlink() else None
            require(actual in (old.get(name), new.get(name)), 'foreign unit edit; keep stopped: ' + name)
            wanted = old.get(name) if recover else new.get(name)
            if actual == wanted:
                continue
            if wanted is None:
                target.unlink()
                lifecycle.sync_dir(target.parent)
                continue
            if not target.parent.exists():
                target.parent.mkdir(mode=0o755)
                lifecycle.sync_dir(target.parent.parent)
            trusted(target.parent, directory=True)
            if recover:
                source = transaction / state['oldUnits'][name]
                staged = transaction / 'restore-units' / str(sorted(old).index(name))
                if not staged.exists():
                    temporary = staged.with_name('.' + staged.name + '.' + uuid.uuid4().hex)
                    lifecycle.production_copy(source, temporary, wanted, new=True)
                    os.rename(str(temporary), str(staged))
                    lifecycle.sync_dir(staged.parent)
            else:
                staged = transaction / 'unit-ready' / str(target.relative_to(UNIT_DIR))
            require(guard.production_pin(staged) == wanted, 'staged unit drift')
            os.replace(str(staged), str(target))
            lifecycle.sync_dir(target.parent)
            lifecycle.sync_dir(staged.parent)

    def check_unit_transition(self, approval):
        actual = installed_units(approval['units'])
        old = approval['previousUnits']
        new = {name: pin for name, pin in old.items() if not owned_unit_path(name)}
        new.update({str(UNIT_DIR / name): pin for name, pin in approval['newUnits'].items()})
        require(set(actual) <= set(old) | set(new), 'unapproved unit added during transaction')
        for name in set(old) | set(new):
            require(actual.get(name) in (old.get(name), new.get(name)),
                    'foreign or missing unit during transaction: ' + name)

    def recover(self, transaction_id, approval_sha):
        root_setup()
        require(re.fullmatch('[A-Za-z0-9_-]{1,63}', transaction_id), 'invalid transaction ID')
        transaction = ROOT / 'transactions' / transaction_id
        approval = read_record(transaction / 'approval.json', approval_sha)
        state = read_record(transaction / 'state.json')
        require(approval['transaction'] == state['transaction'] == transaction_id and
                state['approvalSha256'] == approval_sha and
                approval['format'] in ('gateway-reprovision-approval-v1', 'gateway-reprovision-approval-v2') and
                approval['gatewayHome'] == str(HOME) and approval['controlEnabled'] is False and
                approval['activation'] == 'stopped-only', 'recovery authority mismatch')
        require(('bootstrapJournal' in approval) == (approval['format'] == 'gateway-reprovision-approval-v2'),
                'bootstrap journal recovery format mismatch')
        require(state['oldUnits'] == {name: 'old-units/' + str(index) for index, name in
                                     enumerate(sorted(approval['previousUnits']))}, 'unit backup journal changed')
        require(all(unit_name(u) for u in approval['units']), 'recovery unit scope invalid')
        for name in approval['newUnits']:
            unit_relative(name)
        bootstrap_location = (bootstrap_recovery_location(transaction, approval)
                              if approval['format'] == 'gateway-reprovision-approval-v2' else None)
        pending = ROOT / 'pending.json'
        pointer = ROOT / 'current.json'
        expected = {'transaction': transaction_id, 'approvalSha256': approval_sha}
        if pending.exists():
            require(read_record(pending) == expected, 'another pending transaction owns recovery')
        else:
            current = read_record(pointer) if pointer.exists() else None
            require(current == expected or state['phase'] == 'RECOVERED_STOPPED' and
                    current == approval['previousPointer'] and identity(HOME) == approval['previousIdentity'],
                    'recovery is not the current transaction')
            record(pending, expected, new=True)
        persistent_stop(approval['units'])
        self.check_unit_transition(approval)
        self.checkpoint(transaction, state, 'RECOVERING_STOPPED')
        previous = transaction / 'previous-home'
        retired = transaction / 'retired-candidate'
        if previous.exists():
            require(identity(previous) == approval['previousIdentity'] and tree(previous) == state['previousTree'],
                    'original image changed; do not retire the installed candidate')
        if HOME.exists():
            current_id = identity(HOME)
            require(current_id in (approval['previousIdentity'], state['newIdentity']), 'foreign live image')
            if current_id == state['newIdentity']:
                require(immutable(tree(HOME, IMMUTABLE)) == immutable(approval['imageTree']) and not retired.exists(),
                        'candidate changed or retirement path occupied')
                os.rename(str(HOME), str(retired))
                lifecycle.sync_dir(HOME.parent)
                lifecycle.sync_dir(transaction)
        if not HOME.exists():
            require(identity(previous) == approval['previousIdentity'] and tree(previous) == state['previousTree'],
                    'original image changed or missing')
            os.rename(str(previous), str(HOME))
            lifecycle.sync_dir(HOME.parent)
            lifecycle.sync_dir(transaction)
        require(identity(HOME) == approval['previousIdentity'] and
                immutable(tree(HOME, IMMUTABLE)) == approval['previousImmutable'], 'restored original image drift')
        if bootstrap_location is not None:
            location = bootstrap_recovery_location(transaction, approval)
            if location != BOOTSTRAP_ROOT:
                os.rename(str(location), str(BOOTSTRAP_ROOT))
                lifecycle.sync_dir(BOOTSTRAP_ROOT.parent)
                lifecycle.sync_dir(transaction)
        self.switch_units(transaction, approval, state, recover=True)
        lifecycle.systemctl('daemon-reload')
        require(installed_units(approval['units']) == approval['previousUnits'] and not processes(),
                'recovery is not verified stopped')
        self.checkpoint(transaction, state, 'RECOVERED_STOPPED')
        if approval['previousPointer'] is None:
            if pointer.exists():
                pointer.unlink()
        else:
            record(pointer, approval['previousPointer'])
        if pending.exists():
            pending.unlink()
        lifecycle.sync_dir(ROOT)
        return transaction


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action')
    sub.required = True
    prepare = sub.add_parser('plan')
    prepare.add_argument('--image', type=Path, required=True)
    prepare.add_argument('--units', type=Path, required=True)
    prepare.add_argument('--transaction', required=True)
    prepare.add_argument('--expires-at', type=int, required=True)
    prepare.add_argument('--output', type=Path, required=True)
    prepare.add_argument('--retire-bootstrap', action='store_true',
                         help='retain the completed first-runtime journal with the previous image')
    apply = sub.add_parser('apply')
    apply.add_argument('--approval', type=Path, required=True)
    apply.add_argument('--approval-sha256', required=True)
    recover = sub.add_parser('recover')
    recover.add_argument('--transaction', required=True)
    recover.add_argument('--approval-sha256', required=True)
    args = parser.parse_args()
    with lifecycle.production_lock():
        if args.action == 'plan':
            root_setup()
            value = plan(args.image, args.units, args.transaction, args.expires_at, args.retire_bootstrap)
            trusted(args.output.parent, directory=True)
            record(args.output, value, new=True)
            print(digest(args.output))
        elif args.action == 'apply':
            print('INSTALLED_STOPPED', Reprovision().apply(args.approval, args.approval_sha256))
        else:
            print('RECOVERED_STOPPED', Reprovision().recover(args.transaction, args.approval_sha256))


if __name__ == '__main__':
    main()
