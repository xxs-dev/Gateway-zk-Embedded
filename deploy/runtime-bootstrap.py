#!/usr/bin/env python3
"""Prepare a first runtime after reprovision, without changing live startup.

Creates exclusively new empty native stores and a bound stopped receipt. This
entry does not activate services, remove fences, change live configuration or
delete any store. A later activation must consume the exact prepared receipt.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import time

sys.dont_write_bytecode = True


def paired(name):
    path = Path(__file__).with_name(name)
    spec = importlib.util.spec_from_file_location(name.replace('.', '_'), str(path))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


reprovision = paired('gateway-reprovision.py')
policy = paired('production_role_policy.py')
lifecycle, guard = reprovision.lifecycle, reprovision.guard
require = lifecycle.require
HOME = Path('/opt/modbus-gateway')
ROOT = Path('/opt/gateway-runtime-bootstrap')
SHM = Path('/dev/shm')
TRUST = Path('/etc/modbus-gateway/ota.pub')
BOOT = Path('/proc/sys/kernel/random/boot_id')
ID = re.compile(r'[A-Za-z0-9_-]{1,63}\Z')
SHA = re.compile(r'[0-9a-f]{64}\Z')


def boot_id():
    value = lifecycle.regular(BOOT).read_text().strip()
    require(ID.fullmatch(value), 'invalid bootstrap boot ID')
    return value


def helper_pins():
    paths = (Path(__file__), Path(reprovision.__file__), Path(policy.__file__),
             Path(lifecycle.__file__), Path(guard.__file__))
    return {p.name: guard.production_pin(p) for p in paths}


def inventory():
    meta = SHM.lstat()
    require(stat.S_ISDIR(meta.st_mode) and not SHM.is_symlink() and meta.st_uid == 0,
            'untrusted bootstrap SHM directory')
    result = {}
    for path in sorted(SHM.iterdir()):
        value = path.lstat()
        result[path.name] = {k: getattr(value, 'st_' + k) for k in ('dev', 'ino', 'size', 'mode', 'uid', 'gid', 'nlink')}
    return result


def stopped(units):
    require(reprovision.trusted(reprovision.ROOT / 'STOP') and
            (reprovision.ROOT / 'STOP').read_bytes() == reprovision.STOP_BODY,
            'bootstrap requires retained reprovision STOP')
    for unit in units:
        names = {unit, unit.split('@')[0] + '@.service'} if '@' in unit else {unit}
        fences = [reprovision.UNIT_DIR / (name + '.d') / reprovision.FENCE_NAME for name in names]
        existing = [path for path in fences if path.exists() or path.is_symlink()]
        require(existing and all(reprovision.trusted(p) and p.read_bytes() == reprovision.fence_body()
                                 for p in existing), 'bootstrap participant lacks stop fence: ' + unit)
        if unit.endswith('@.service'):
            continue
        show = lambda prop: guard.systemctl_value(unit, prop)
        require(show('ActiveState') in ('inactive', 'failed') and show('MainPID') == '0',
                'bootstrap participant is not stopped: ' + unit)
        if show('LoadState') != 'not-found':
            require(show('LoadState') == 'loaded' and show('NeedDaemonReload') == 'no' and
                    set(map(str, existing)) & set(show('DropInPaths').split()),
                    'bootstrap stop fence is not loaded: ' + unit)
    require(not reprovision.processes(), 'bootstrap runtime process remains')


def origin():
    pointer = reprovision.current_pointer()
    require(pointer is not None, 'first runtime requires a stopped reprovision receipt')
    require(not (reprovision.ROOT / 'pending.json').exists() and
            not (reprovision.ROOT / 'pending.json').is_symlink(), 'unfinished reprovision operation')
    transaction = reprovision.ROOT / 'transactions' / pointer['transaction']
    approval = reprovision.read_record(transaction / 'approval.json', pointer['approvalSha256'])
    state = reprovision.read_record(transaction / 'state.json')
    require(approval['gatewayHome'] == str(HOME) and approval['controlEnabled'] is False and
            approval['activation'] == 'stopped-only', 'invalid bootstrap reprovision origin')
    require(reprovision.tree(HOME) == approval['imageTree'], 'installed bootstrap image drift')
    require(reprovision.tree(transaction / 'previous-home') == state['previousTree'] and
            reprovision.identity(transaction / 'previous-home') == approval['previousIdentity'],
            'bootstrap recovery image drift')
    expected_units = {p: v for p, v in approval['previousUnits'].items() if not reprovision.owned_unit_path(p)}
    expected_units.update({str(reprovision.UNIT_DIR / p): v for p, v in approval['newUnits'].items()})
    require(reprovision.installed_units(approval['units']) == expected_units, 'installed bootstrap units drift')
    stopped(approval['units'])
    return dict(pointer, stateSha256=reprovision.digest(transaction / 'state.json'),
                imageIdentity=reprovision.identity(HOME), unitFiles=expected_units,
                units=approval['units'], machineCode=approval['machineCode'])


def configuration(source, authority, profile, trust):
    source = source.absolute()
    require(source.resolve() == source, 'bootstrap config path must be canonical')
    require(source != HOME and HOME not in source.parents and ROOT != source and ROOT not in source.parents,
            'bootstrap config payload overlaps live state')
    require(source not in HOME.parents and source not in ROOT.parents, 'bootstrap config contains live state')
    tree = reprovision.tree(source)
    require(all(row['kind'] in ('file', 'directory') for row in tree.values()), 'bootstrap config symlink')
    live = HOME / 'config/runtime'
    for name in ('device_identity.json', 'edge-package-manifest.json'):
        require(guard.production_pin(source / name) == guard.production_pin(live / name),
                'bootstrap must preserve installed identity/package manifest')
    for prefix in ('network', 'tls'):
        expected = {p.relative_to(live).as_posix(): guard.production_pin(p)
                    for p in (live / prefix).rglob('*') if p.is_file() or p.is_symlink()}
        actual = {p.relative_to(source).as_posix(): guard.production_pin(p)
                  for p in (source / prefix).rglob('*') if p.is_file() or p.is_symlink()}
        require(actual == expected, 'bootstrap network/TLS change is not approved')
    ota_enabled = guard.production_trust_verify(trust, TRUST, allow_disabled=True)
    require(trust['publicKeySha256'] == authority['otaPublicKeySha256'], 'bootstrap OTA authority differs')
    require(isinstance(profile, dict) and set(profile) == {'format', 'machineCode', 'ramMiB',
            'capabilities', 'simulatorBindings', 'allowedUnits', 'segmentNames'} and
            profile['format'] == 'board-simulator-profile-v1' and profile['ramMiB'] in (2048, 4096) and
            profile['machineCode'] == authority['machineCode'], 'invalid bootstrap board profile')
    resolved = policy.resolve(source, reprovision.UNIT_DIR, authority, TRUST,
        simulator_bindings=profile['simulatorBindings'], capabilities=profile['capabilities'],
        watchdog='gateway-health-watchdog.service' in profile['allowedUnits'], ota_enabled=ota_enabled)
    names = sorted({n for role in resolved['roles'] for n in role['shm']['names']})
    require(profile['allowedUnits'] == [r['unit'] for r in resolved['roles']] and
            profile['segmentNames'] == names and 0 < len(names) <= 128,
            'bootstrap profile differs from resolved roles/stores')
    for role in resolved['roles']:
        if role['family'] == 'ky-ems':
            require('bin/gateway-qt-native-run.sh' in role['runtimeFiles'], 'bootstrap requires native Qt')
            path = source / 'qt-display.env'
            environment = guard.production_environment_file(path, resolved['configSha256']['qt-display.env'])
            require(environment.get('GATEWAY_SYSTEM_MONITOR_SHARED_MEMORY_NAME') == authority['monitorShmName'],
                    'bootstrap native Qt monitor binding mismatch')
    return tree, resolved


def plan(source, creator, transaction, expires, authority, profile, trust):
    require(os.geteuid() == 0 and os.uname().machine == 'aarch64', 'bootstrap requires AArch64 root')
    require(ID.fullmatch(transaction) and type(expires) is int and time.time() < expires <= time.time() + 86400,
            'invalid bootstrap transaction/expiry')
    for name in ('pending.json', 'current.json'):
        require(not (ROOT / name).exists() and not (ROOT / name).is_symlink(), 'bootstrap state already reserved')
    installed = origin()
    tree, resolved = configuration(source, authority, profile, trust)
    require(installed['machineCode'] == authority['machineCode'], 'bootstrap machine identity differs')
    stopped(profile['allowedUnits'])
    creator = creator.absolute()
    require(creator == HOME / 'bin/pointctl', 'bootstrap creator must be the installed pointctl')
    reprovision.trusted(creator)
    pin = guard.production_pin(creator)
    require(pin['mode'] & 0o111, 'bootstrap creator must be executable')
    guard.production_architecture(creator)
    with creator.open('rb') as stream:
        require(stream.read(4) == b'\x7fELF', 'bootstrap requires native pointctl')
        marker, tail, found = b'create-empty requires exactly --shm <new-name>', b'', False
        for chunk in iter(lambda: stream.read(65536), b''):
            found = found or marker in tail + chunk
            tail = chunk[-len(marker):]
        require(found, 'installed pointctl lacks exclusive empty-store creation')
    retained = inventory()
    require(not set(profile['segmentNames']) & set(retained), 'bootstrap store name already occupied')
    return dict(format='runtime-bootstrap-prepare-v1', transaction=transaction, expiresAtUnix=expires,
                gatewayHome=str(HOME), controlEnabled=False, bootId=boot_id(), origin=installed,
                configSource=str(source.absolute()), configTree=tree, roleAuthority=resolved,
                boardProfile=profile, trust=trust, creatorPath=str(creator), creatorPin=pin,
                helperPins=helper_pins(), retainedShm=retained)


def retained(approval):
    current = inventory()
    for name, value in approval['retainedShm'].items():
        require(current.get(name) == value, 'retained SHM metadata changed: ' + name)
    require(set(current) <= set(approval['retainedShm']) | set(approval['boardProfile']['segmentNames']),
            'unreviewed SHM appeared during bootstrap')
    return current


def capacity(approval, count):
    lifecycle.tmpfs_mount(SHM)
    page = os.sysconf('SC_PAGE_SIZE')
    size = ((guard.POINT_STORE_SIZES[11] + page - 1) // page) * page
    available = os.statvfs(SHM)
    reserve = 64 * 1024 * 1024 + sum(r['reserveMiB'] * 1024 * 1024 for r in approval['roleAuthority']['roles'])
    require(available.f_frsize * available.f_bavail >= size * count and
            lifecycle.mem_available_bytes() >= reserve + size * count, 'insufficient bootstrap memory/tmpfs')


def verify_created(segments):
    for segment in segments:
        path = lifecycle.regular(SHM / segment['name'])
        value = path.stat()
        require((value.st_dev, value.st_ino) == (segment['dev'], segment['ino']) and
                value.st_uid == 0 and stat.S_IMODE(value.st_mode) == 0o600 and
                reprovision.digest(path) == segment['sha256'], 'created bootstrap store changed')
        guard.point_store_abi(path, 11)
    lifecycle.no_mappings([s['name'] for s in segments], include_fds=True)


class Bootstrap:
    def checkpoint(self, transaction, state, phase):
        state['phase'] = phase
        reprovision.record(transaction / 'state.json', state)

    def prepare(self, path, approval_sha):
        approval = reprovision.read_record(path, approval_sha)
        require(approval == plan(Path(approval['configSource']), Path(approval['creatorPath']),
                approval['transaction'], approval['expiresAtUnix'], approval['roleAuthority']['protected'],
                approval['boardProfile'], approval['trust']), 'bootstrap approval drift')
        require(lifecycle.production_locked_owner({'ownerProof': lifecycle.production_owner_proof()}),
                'bootstrap requires offline lock ownership')
        capacity(approval, len(approval['boardProfile']['segmentNames']))
        for directory in (ROOT, ROOT / 'transactions'):
            if not directory.exists():
                reprovision.trusted(directory.parent, directory=True)
                directory.mkdir(mode=0o700)
                lifecycle.sync_dir(directory.parent)
            require(stat.S_IMODE(reprovision.trusted(directory, directory=True).st_mode) == 0o700,
                    'bootstrap journal must be private')
        transaction = ROOT / 'transactions' / approval['transaction']
        require(not transaction.exists() and not transaction.is_symlink(), 'bootstrap transaction consumed')
        transaction.mkdir(mode=0o700)
        lifecycle.sync_dir(transaction.parent)
        lifecycle.publish_new(transaction / 'approval.json', lifecycle.regular(path).read_bytes())
        require(reprovision.digest(transaction / 'approval.json') == approval_sha, 'bootstrap approval copy drift')
        state = dict(format='runtime-bootstrap-state-v1', transaction=approval['transaction'],
                     approvalSha256=approval_sha, phase='INTENT', segments=[], inProgress=None)
        self.checkpoint(transaction, state, 'INTENT')
        pointer = dict(transaction=approval['transaction'], approvalSha256=approval_sha)
        lifecycle.publish_new(ROOT / 'pending.json', reprovision.encoded(pointer))
        reprovision.copy_tree(Path(approval['configSource']), transaction / 'config', approval['configTree'])
        self.checkpoint(transaction, state, 'CREATING')
        for name in approval['boardProfile']['segmentNames']:
            require(boot_id() == approval['bootId'] and time.time() < approval['expiresAtUnix'],
                    'bootstrap boot/window changed')
            require(origin() == approval['origin'] and helper_pins() == approval['helperPins'],
                    'bootstrap origin/helper drift')
            guard.production_trust_verify(approval['trust'], TRUST, allow_disabled=True)
            stopped(approval['boardProfile']['allowedUnits'])
            current = retained(approval)
            verify_created(state['segments'])
            require(name not in current, 'bootstrap target appeared before exclusive creation')
            capacity(approval, len(approval['boardProfile']['segmentNames']) - len(state['segments']))
            creator = Path(approval['creatorPath'])
            require(guard.production_pin(creator) == approval['creatorPin'], 'bootstrap creator drift')
            state['inProgress'] = name
            self.checkpoint(transaction, state, 'CREATING')
            result = subprocess.run([str(creator), 'create-empty', '--shm', name],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30,
                env={'PATH': '/usr/bin:/bin', 'LC_ALL': 'C'})
            require(result.returncode == 0 and len(result.stdout) <= 4096 and len(result.stderr) <= 4096 and
                    re.fullmatch(r'shm=/?' + re.escape(name) +
                        r' latest=0/[0-9]+ latestCapacity=[0-9]+ pendingWrites=0/[0-9]+ pendingCapacity=[0-9]+'
                        r' persistent=0/[0-9]+ persistentCapacity=[0-9]+ pointUpdates=0/[0-9]+'
                        r' writeSeq=0 persistentSeq=0 pointUpdateSeq=0\n?', result.stdout.decode('ascii')),
                    'bootstrap native empty-store creation failed')
            value = lifecycle.regular(SHM / name).stat()
            require(value.st_uid == 0 and stat.S_IMODE(value.st_mode) == 0o600 and value.st_dev == SHM.stat().st_dev,
                    'bootstrap created store ownership changed')
            guard.point_store_abi(SHM / name, 11)
            lifecycle.no_mappings([name], include_fds=True)
            state['segments'].append(dict(name=name, dev=value.st_dev, ino=value.st_ino,
                                          sha256=reprovision.digest(SHM / name)))
            state['inProgress'] = None
            self.checkpoint(transaction, state, 'CREATING')
        require(origin() == approval['origin'] and retained(approval) and boot_id() == approval['bootId'] and
                time.time() < approval['expiresAtUnix'] and helper_pins() == approval['helperPins'],
                'bootstrap state changed before receipt')
        guard.production_trust_verify(approval['trust'], TRUST, allow_disabled=True)
        stopped(approval['boardProfile']['allowedUnits'])
        verify_created(state['segments'])
        require(reprovision.tree(transaction / 'config') == approval['configTree'], 'bootstrap staged config drift')
        receipt = dict(format='runtime-bootstrap-prepared-v1', **pointer,
                       bootId=approval['bootId'], activation='not-authorized', controlEnabled=False,
                       segments=state['segments'], configTree=approval['configTree'],
                       roleAuthority=approval['roleAuthority'], trust=approval['trust'])
        lifecycle.publish_new(transaction / 'ready.json', reprovision.encoded(receipt))
        self.checkpoint(transaction, state, 'PREPARED_STOPPED')
        lifecycle.publish_new(ROOT / 'current.json', reprovision.encoded(
            dict(pointer, readySha256=reprovision.digest(transaction / 'ready.json'))))
        (ROOT / 'pending.json').unlink()
        lifecycle.sync_dir(ROOT)
        return transaction

    def recover(self, name, approval_sha):
        require(ID.fullmatch(name) and SHA.fullmatch(approval_sha), 'invalid bootstrap recovery identity')
        require(lifecycle.production_locked_owner({'ownerProof': lifecycle.production_owner_proof()}),
                'bootstrap recovery requires offline lock ownership')
        transaction = ROOT / 'transactions' / name
        approval = reprovision.read_record(transaction / 'approval.json', approval_sha)
        require(approval['format'] == 'runtime-bootstrap-prepare-v1' and approval['transaction'] == name and
                approval['gatewayHome'] == str(HOME) and approval['controlEnabled'] is False,
                'bootstrap recovery authority drift')
        require(helper_pins() == approval['helperPins'], 'bootstrap recovery requires the paired helpers')
        names = approval['boardProfile']['segmentNames']
        state_path = transaction / 'state.json'
        if not state_path.exists() and not state_path.is_symlink():
            # Approval may be durable before the first progress checkpoint. No
            # pointer or native creation is allowed before that checkpoint.
            require(all(not p.exists() and not p.is_symlink() for p in
                        (ROOT / 'pending.json', ROOT / 'current.json')) and
                    not set(names) & set(inventory()), 'bootstrap initialization is not untouched')
            state = dict(format='runtime-bootstrap-state-v1', transaction=name,
                         approvalSha256=approval_sha, phase='INTENT', segments=[], inProgress=None)
        else:
            state = reprovision.read_record(state_path)
        require(state['format'] == 'runtime-bootstrap-state-v1' and state['transaction'] == name and
                state['approvalSha256'] == approval_sha and state['phase'] in
                ('INTENT', 'CREATING', 'PREPARED_STOPPED', 'ABORTED_STOPPED'), 'bootstrap recovery authority drift')
        require(set(state) == {'format', 'transaction', 'approvalSha256', 'phase', 'segments', 'inProgress'} and
                isinstance(state['segments'], list) and len(state['segments']) <= len(names) and
                all(isinstance(s, dict) and set(s) == {'name', 'dev', 'ino', 'sha256'} and
                    type(s['dev']) is int and s['dev'] >= 0 and type(s['ino']) is int and s['ino'] > 0 and
                    isinstance(s['sha256'], str) and SHA.fullmatch(s['sha256']) for s in state['segments']) and
                [s['name'] for s in state['segments']] == names[:len(state['segments'])] and
                (state['inProgress'] is None or [state['inProgress']] == names[len(state['segments']):len(state['segments']) + 1]) and
                (state['phase'] != 'INTENT' or not state['segments'] and state['inProgress'] is None) and
                (state['phase'] != 'PREPARED_STOPPED' or len(state['segments']) == len(names) and state['inProgress'] is None),
                'invalid bootstrap recovery progress')
        pointer = dict(transaction=name, approvalSha256=approval_sha)
        for path in (ROOT / 'pending.json', ROOT / 'current.json'):
            if path.exists() or path.is_symlink():
                value = reprovision.read_record(path)
                expected = dict(pointer)
                if path.name == 'current.json':
                    expected['readySha256'] = reprovision.digest(lifecycle.regular(transaction / 'ready.json'))
                require(value == expected, 'another bootstrap owns the journal')
        require(origin() == approval['origin'], 'bootstrap recovery origin drift; keep STOP')
        stopped(approval['boardProfile']['allowedUnits'])
        # Aborted fresh stores are retained, including an interrupted native init.
        # No arbitrary old/new SHM is unlinked, rewound or re-initialized here.
        self.checkpoint(transaction, state, 'ABORTED_STOPPED')
        for path in (ROOT / 'pending.json', ROOT / 'current.json'):
            if path.exists():
                path.unlink()
                lifecycle.sync_dir(ROOT)
        return transaction


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    p = sub.add_parser('plan')
    for name in ('config', 'creator', 'authority', 'profile', 'trust', 'output'):
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--transaction', required=True)
    p.add_argument('--expires-at', type=int, required=True)
    p = sub.add_parser('prepare')
    p.add_argument('--approval', type=Path, required=True)
    p.add_argument('--approval-sha256', required=True)
    p = sub.add_parser('recover')
    p.add_argument('--transaction', required=True)
    p.add_argument('--approval-sha256', required=True)
    args = parser.parse_args()
    with lifecycle.production_lock():
        if args.action == 'plan':
            value = plan(args.config, args.creator, args.transaction, args.expires_at,
                         reprovision.read_record(args.authority), reprovision.read_record(args.profile),
                         reprovision.read_record(args.trust))
            lifecycle.publish_new(args.output, reprovision.encoded(value))
            print(reprovision.digest(args.output))
        elif args.action == 'prepare':
            print(Bootstrap().prepare(args.approval, args.approval_sha256))
        else:
            print(Bootstrap().recover(args.transaction, args.approval_sha256))


if __name__ == '__main__':
    main()
