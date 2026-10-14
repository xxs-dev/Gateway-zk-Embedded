#!/usr/bin/env python3
"""Explicit first-runtime activation from a prepared stopped reprovision.

No legacy activation is synthesized. Interrupted, unpromoted transactions require
paired stopped recovery; a healthy promoted pointer is never rolled back here.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import sys
import time

sys.dont_write_bytecode = True
import importlib.util

spec = importlib.util.spec_from_file_location('first_runtime_prepare',
    str(Path(__file__).with_name('runtime-bootstrap.py')))
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)
reprovision, lifecycle, guard = prepare.reprovision, prepare.lifecycle, prepare.guard
require = lifecycle.require
FORMAT = 'runtime-bootstrap-activation-v1'
STATE = 'runtime-bootstrap-activation-state-v1'
DIRS = {'config': 'config/runtime', 'data': 'data', 'run': 'run'}


def root():
    return prepare.ROOT / 'activations'


def encoded(value):
    return (json.dumps(value, sort_keys=True) + '\n').encode()


def pin_bytes(raw, mode=0o644):
    return dict(sha256=hashlib.sha256(raw).hexdigest(), mode=mode, bytes=len(raw))


def helper_pins():
    pins = prepare.helper_pins()
    for path in (Path(__file__), Path(lifecycle.__file__).with_name('ota-health-result.py')):
        pins[path.name] = guard.production_pin(path)
    return pins


def sentinel(transaction):
    return ('first-runtime bootstrap ' + transaction + ': guarded startup required\n').encode()


def prepared(name, ready_sha):
    require(prepare.ID.fullmatch(name) and prepare.SHA.fullmatch(ready_sha), 'invalid prepared identity')
    transaction = prepare.ROOT / 'transactions' / name
    ready = reprovision.read_record(transaction / 'ready.json', ready_sha)
    pointer = dict(transaction=name, approvalSha256=ready['approvalSha256'], readySha256=ready_sha)
    require(reprovision.read_record(prepare.ROOT / 'current.json') == pointer and
            not (prepare.ROOT / 'pending.json').exists() and not (prepare.ROOT / 'pending.json').is_symlink(),
            'prepared pointer is not current')
    approval = reprovision.read_record(transaction / 'approval.json', ready['approvalSha256'])
    state = reprovision.read_record(transaction / 'state.json')
    require(ready['format'] == 'runtime-bootstrap-prepared-v1' and ready['activation'] == 'not-authorized' and
            ready['controlEnabled'] is False and ready['transaction'] == name and
            state['phase'] == 'PREPARED_STOPPED' and state['approvalSha256'] == ready['approvalSha256'] and
            state['segments'] == ready['segments'] and state['inProgress'] is None and
            ready['configTree'] == approval['configTree'] and ready['roleAuthority'] == approval['roleAuthority'] and
            ready['trust'] == approval['trust'], 'prepared receipt authority differs')
    require(prepare.boot_id() == ready['bootId'] == approval['bootId'] and
            time.time() < approval['expiresAtUnix'], 'prepared boot/window expired')
    require(prepare.origin() == approval['origin'] and prepare.helper_pins() == approval['helperPins'],
            'prepared origin/helpers changed')
    require(reprovision.tree(transaction / 'config') == ready['configTree'], 'prepared config changed')
    prepare.retained(approval)
    prepare.verify_created(ready['segments'])
    require(guard.production_pin(prepare.TRUST)['sha256'] == ready['trust']['publicKeySha256'],
            'prepared signature trust changed')
    return transaction, approval, ready


def unit_additions(transaction, units, resolved):
    result = {}
    gates = {}
    for unit in units:
        path = reprovision.UNIT_DIR / (unit + '.d') / ('90-offline-bootstrap-' + transaction + '.conf')
        gates[unit] = str(path)
        result[str(path)] = lifecycle.production_gate(prepare.HOME, unit)
    for role in resolved['roles']:
        if role['family'] == 'system-monitor':
            path = reprovision.UNIT_DIR / (role['unit'] + '.d') / ('91-bootstrap-monitor-' + transaction + '.conf')
            result[str(path)] = ('[Service]\nEnvironment=GATEWAY_SYSTEM_MONITOR_SHARED_MEMORY_NAME=' +
                                resolved['protected']['monitorShmName'] + '\n').encode()
    return gates, result


def plan(name, ready_sha, transaction, expires, release, health):
    require(os.geteuid() == 0 and os.uname().machine == 'aarch64', 'first runtime requires AArch64 root')
    require(prepare.ID.fullmatch(transaction) and type(expires) is int and
            time.time() < expires <= time.time() + 86400, 'invalid first-runtime transaction/window')
    for path in (root() / 'pending.json', root() / 'current.json', root() / transaction):
        require(not path.exists() and not path.is_symlink(), 'first-runtime activation already reserved')
    source, approval, ready = prepared(name, ready_sha)
    home = prepare.HOME
    require(expires <= approval['expiresAtUnix'], 'activation outlives preparation approval')
    for path in (home / 'data/runtime-production', home / 'data/runtime-upgrade-stop'):
        require(not path.exists() and not path.is_symlink(), 'first runtime requires no prior production state')
    require(all(p.stat().st_dev == home.stat().st_dev and
                reprovision.mount_id(p) == reprovision.mount_id(home)
                for p in (prepare.ROOT, reprovision.UNIT_DIR)), 'bootstrap switching requires one mount')
    before = {key: dict(identity=reprovision.identity(home / relative), tree=reprovision.tree(home / relative))
              for key, relative in DIRS.items()}
    require(all(row['kind'] == 'directory' for key in ('data', 'run') for row in before[key]['tree'].values()),
            'first runtime cannot replace existing runtime data')
    units = [u for u in approval['origin']['units'] if u != guard.PRODUCTION_RECOVERY_UNIT]
    require(set(approval['boardProfile']['allowedUnits']) <= set(units), 'first-runtime unit inventory incomplete')
    gates, additions = unit_additions(transaction, units, ready['roleAuthority'])
    require(all(not Path(p).exists() and not Path(p).is_symlink() for p in additions), 'bootstrap gate name occupied')
    recovery_dir = reprovision.UNIT_DIR / (guard.PRODUCTION_RECOVERY_UNIT + '.d')
    require({p.name for p in recovery_dir.iterdir()} == {reprovision.FENCE_NAME}, 'unreviewed recovery override')
    files = guard.production_files(home)
    for helper in (Path(lifecycle.__file__), Path(guard.__file__), Path(prepare.policy.__file__),
                   Path(lifecycle.__file__).with_name('ota-health-result.py')):
        installed = files.get('bin/' + helper.name)
        source_pin = guard.production_pin(helper)
        require(installed is not None and all(installed[k] == source_pin[k] for k in ('sha256', 'bytes')),
                'installed lifecycle helper is not paired: ' + helper.name)
    boot_unit = reprovision.UNIT_DIR / guard.PRODUCTION_RECOVERY_UNIT
    require(guard.production_pin(boot_unit) == guard.production_pin(Path(lifecycle.__file__).with_name(boot_unit.name)),
            'installed first-runtime recovery unit is not paired')
    for role in ready['roleAuthority']['roles']:
        require(set(role['runtimeFiles']) <= set(files), 'first-runtime role binary missing')
    for relative in files:
        guard.production_architecture(home / relative)
    historical = {str(path): guard.production_pin(path) for path in
                  (source / 'approval.json', source / 'ready.json')}
    origin_tx = reprovision.ROOT / 'transactions' / approval['origin']['transaction']
    historical.update({str(path): guard.production_pin(path) for path in
                       (origin_tx / 'approval.json', origin_tx / 'state.json')})
    historical[str(home / 'data/runtime-upgrade-stop')] = pin_bytes(sentinel(transaction), 0o600)
    retired = {}
    for name, meta in approval['retainedShm'].items():
        if stat.S_ISREG(meta['mode']) and meta['nlink'] == 1 and re.fullmatch(r'[A-Za-z0-9_-]{1,127}', name):
            with (prepare.SHM / name).open('rb') as stream:
                if stream.read(4) == b'TSPM':
                    retired[name] = {k: meta[k] for k in ('dev', 'ino')}
    profile_raw = encoded(approval['boardProfile'])
    candidate = dict(format='runtime-baseline-v2', baselineId=transaction, generation=1,
        parentSha256=None, gatewayHome=str(home), machineCode=approval['origin']['machineCode'],
        architecture='aarch64', pointStoreAbi=11, pointStoreSize=guard.POINT_STORE_SIZES[11],
        clusterProtocol=2, controlEnabled=False, files=files, configSha256=ready['roleAuthority']['configSha256'],
        units=units, allowedUnits=approval['boardProfile']['allowedUnits'],
        unitFiles=dict(approval['origin']['unitFiles'], **{p: pin_bytes(raw) for p, raw in additions.items()}),
        gates=gates, segments=[{k: row[k] for k in ('name', 'dev', 'ino')} for row in ready['segments']],
        retiredSources=retired, historical=historical, bootId=ready['bootId'], release=release,
        trust=ready['trust'], healthPolicy=health,
        roleAuthority=dict(format='configured-role-authority-v2',
            profileSha256=hashlib.sha256(profile_raw).hexdigest(), resolved=ready['roleAuthority']))
    guard.production_schema(candidate, home)
    capacity = os.statvfs(home)
    needed = sum(p.get('bytes', 0) for p in ready['configTree'].values()) + 64 * 1024 * 1024
    require(capacity.f_frsize * capacity.f_bavail >= needed, 'insufficient bootstrap activation disk')
    return dict(format=FORMAT, transaction=transaction, gatewayHome=str(home), expiresAtUnix=expires,
        operation='bootstrap', previous=None, candidate=candidate, preparedTransaction=ready['transaction'],
        preparedSha256=ready_sha, helperPins=helper_pins(), originalDirectories=before,
        unitAdditions={p: raw.decode() for p, raw in additions.items()}, profile=approval['boardProfile'],
        prepareApprovalSha256=ready['approvalSha256'], origin=approval['origin'])


class Activation:
    def checkpoint(self, transaction, state, phase):
        state['phase'] = phase
        reprovision.record(transaction / 'state.json', state)

    def ownership(self, transaction, approval):
        require(transaction == root() / approval['transaction'] and approval['format'] == FORMAT and
                approval['operation'] == 'bootstrap' and approval['previous'] is None and
                approval['gatewayHome'] == str(prepare.HOME) and helper_pins() == approval['helperPins'],
                'bootstrap activation paired authority changed')
        require(lifecycle.production_locked_owner({'ownerProof': lifecycle.production_owner_proof()}),
                'bootstrap activation requires offline lock')

    def move(self, source, target):
        require(not target.exists() and not target.is_symlink(), 'bootstrap move target occupied')
        reprovision.trusted(source, directory=True)
        reprovision.trusted(target.parent, directory=True)
        reprovision.no_mounts(source)
        os.rename(str(source), str(target))
        lifecycle.sync_dir(source.parent)
        lifecycle.sync_dir(target.parent)

    def apply(self, path, approval_sha):
        approval = reprovision.read_record(path, approval_sha)
        expected = plan(approval['preparedTransaction'], approval['preparedSha256'], approval['transaction'],
                        approval['expiresAtUnix'], approval['candidate']['release'], approval['candidate']['healthPolicy'])
        require(approval == expected, 'bootstrap activation approval drift')
        transaction = root() / approval['transaction']
        self.ownership(transaction, approval)
        if not root().exists():
            root().mkdir(mode=0o700)
            lifecycle.sync_dir(root().parent)
        require(stat.S_IMODE(reprovision.trusted(root(), directory=True).st_mode) == 0o700,
                'activation journal must be private')
        transaction.mkdir(mode=0o700)
        lifecycle.sync_dir(transaction.parent)
        lifecycle.publish_new(transaction / 'approval.json', lifecycle.regular(path).read_bytes())
        require(reprovision.digest(transaction / 'approval.json') == approval_sha, 'activation approval copy drift')
        state = dict(format=STATE, transaction=approval['transaction'], approvalSha256=approval_sha,
            candidateSha256=hashlib.sha256(encoded(approval['candidate'])).hexdigest(), previousSha256=None,
            phase='INTENT', directories={}, ownerProof=lifecycle.production_owner_proof())
        self.checkpoint(transaction, state, 'INTENT')
        lifecycle.publish_new(root() / 'pending.json', encoded(dict(transaction=transaction.name, approvalSha256=approval_sha)))
        for name in ('new', 'old', 'retired', 'retired-units'):
            (transaction / name).mkdir(mode=0o700)
        source = prepare.ROOT / 'transactions' / approval['preparedTransaction']
        reprovision.copy_tree(source / 'config', transaction / 'new/config', reprovision.tree(source / 'config'))
        for key in ('data', 'run'):
            reprovision.copy_tree(prepare.HOME / key, transaction / 'new' / key,
                                 approval['originalDirectories'][key]['tree'])
        runtime_root = transaction / 'new/data/runtime-production'
        runtime_root.mkdir(mode=0o700)
        for relative in ('profiles', 'transactions/' + transaction.name, 'baselines'):
            (runtime_root / relative).mkdir(parents=True, mode=0o700)
        runtime_tx = runtime_root / 'transactions' / transaction.name
        lifecycle.publish_new(runtime_tx / 'approval.json', lifecycle.regular(path).read_bytes())
        lifecycle.publish_new(runtime_tx / 'candidate.json', encoded(approval['candidate']))
        profile = approval['candidate']['roleAuthority']['profileSha256']
        lifecycle.publish_new(runtime_root / 'profiles' / (profile + '.json'), encoded(approval['profile']))
        lifecycle.publish_new(transaction / 'new/data/runtime-upgrade-stop', sentinel(transaction.name))
        for key in DIRS:
            state['directories'][key] = reprovision.identity(transaction / 'new' / key)
        reprovision.sync_directories(transaction / 'new')
        self.checkpoint(transaction, state, 'SWITCHING')
        try:
            prepared(approval['preparedTransaction'], approval['preparedSha256'])
            require(helper_pins() == approval['helperPins'], 'activation helpers changed before switch')
            require(time.time() < approval['expiresAtUnix'], 'activation window expired before switch')
            for key, relative in DIRS.items():
                live = prepare.HOME / relative
                require(reprovision.identity(live) == approval['originalDirectories'][key]['identity'] and
                        reprovision.tree(live) == approval['originalDirectories'][key]['tree'], 'original directory drift')
                self.move(live, transaction / 'old' / key)
                self.move(transaction / 'new' / key, live)
            runtime = lifecycle.ProductionRuntime(prepare.HOME)
            live_tx = runtime.root / 'transactions' / transaction.name
            runtime.state(live_tx, state)
            lifecycle.publish_new(runtime.root / 'pending.json', encoded(dict(transaction=transaction.name, approvalSha256=approval_sha)))
            for name, body in approval['unitAdditions'].items():
                target = Path(name)
                if not target.parent.exists():
                    target.parent.mkdir(mode=0o755)
                reprovision.trusted(target.parent, directory=True)
                lifecycle.publish_new(target, body.encode(), 0o644)
            self.checkpoint(transaction, state, 'OPENING_GATES')
            for unit in approval['origin']['units']:
                fence = reprovision.UNIT_DIR / (unit + '.d') / reprovision.FENCE_NAME
                require(reprovision.trusted(fence) and fence.read_bytes() == reprovision.fence_body(), 'stop fence changed')
                fence.unlink()
                lifecycle.sync_dir(fence.parent)
            recovery_dir = reprovision.UNIT_DIR / (guard.PRODUCTION_RECOVERY_UNIT + '.d')
            recovery_dir.rmdir()
            lifecycle.sync_dir(recovery_dir.parent)
            lifecycle.systemctl('daemon-reload')
            self.checkpoint(transaction, state, 'STARTING')
            runtime.start(live_tx, state, approval['candidate'], 'STARTING')
            health = runtime.health(approval['candidate'])
            health.update(baselineSha256=state['candidateSha256'], transaction=transaction.name, status='healthy')
            lifecycle.publish_new(live_tx / 'health.json', encoded(health))
            runtime.promote(live_tx, state, approval['candidate'])
            self.finish(transaction, approval, state)
        except BaseException:
            # The healthy pointer is the commit point, including uncertain fsync.
            runtime = lifecycle.ProductionRuntime(prepare.HOME)
            if runtime.promoted(runtime.root / 'transactions' / transaction.name, state):
                raise
            self.recover(transaction.name, approval_sha)
            raise
        return transaction

    def finish(self, transaction, approval, state):
        runtime = lifecycle.ProductionRuntime(prepare.HOME)
        live_tx = runtime.root / 'transactions' / transaction.name
        runtime.finish_promoted(live_tx, guard.production_json(live_tx / 'state.json'), approval['candidate'])
        self.checkpoint(transaction, state, 'COMPLETED')
        pointer = dict(transaction=transaction.name, approvalSha256=state['approvalSha256'])
        current = root() / 'current.json'
        if not current.exists():
            lifecycle.publish_new(current, encoded(pointer))
        require(reprovision.read_record(current) == pointer, 'foreign activation current pointer')
        pending = root() / 'pending.json'
        if pending.exists():
            require(reprovision.read_record(pending) == pointer, 'foreign activation pending pointer')
            pending.unlink()
            lifecycle.sync_dir(root())

    def recover(self, name, approval_sha):
        require(prepare.ID.fullmatch(name) and prepare.SHA.fullmatch(approval_sha), 'invalid activation recovery identity')
        transaction = root() / name
        approval = reprovision.read_record(transaction / 'approval.json', approval_sha)
        self.ownership(transaction, approval)
        state = reprovision.read_record(transaction / 'state.json')
        require(state['format'] == STATE and state['transaction'] == name and state['approvalSha256'] == approval_sha and
                state['candidateSha256'] == hashlib.sha256(encoded(approval['candidate'])).hexdigest() and
                state.get('previousSha256') is None and state['phase'] in
                ('INTENT', 'SWITCHING', 'OPENING_GATES', 'STARTING', 'PROMOTING', 'COMPLETED',
                 'RECOVERING_STOPPED', 'RECOVERED_STOPPED') and
                set(state) in ({'format', 'transaction', 'approvalSha256', 'candidateSha256', 'previousSha256',
                               'phase', 'directories', 'ownerProof'},
                              {'format', 'transaction', 'approvalSha256', 'candidateSha256', 'previousSha256',
                               'phase', 'directories', 'ownerProof', 'startingBaselineSha256'}) and
                isinstance(state['directories'], dict) and set(state['directories']) <= set(DIRS) and
                all(isinstance(v, dict) and set(v) == {'dev', 'ino'} and type(v['dev']) is int and
                    type(v['ino']) is int and v['ino'] > 0 for v in state['directories'].values()),
                'activation recovery state drift')
        pointer = dict(transaction=name, approvalSha256=approval_sha)
        for path in (root() / 'pending.json', root() / 'current.json'):
            if path.exists() or path.is_symlink():
                require(reprovision.read_record(path) == pointer, 'another activation owns recovery')
        runtime = lifecycle.ProductionRuntime(prepare.HOME)
        if runtime.promoted(runtime.root / 'transactions' / name, state):
            self.finish(transaction, approval, state)
            return transaction
        require(not (runtime.root / 'current.json').exists() and not (runtime.root / 'current.json').is_symlink(),
                'another production baseline owns the installation')
        require(state['phase'] != 'COMPLETED', 'consumed activation is not current')
        self.checkpoint(transaction, state, 'RECOVERING_STOPPED')
        reprovision.persistent_stop(approval['origin']['units'])
        for index, (name, body) in enumerate(sorted(approval['unitAdditions'].items())):
            path = Path(name)
            if path.exists() or path.is_symlink():
                require(guard.production_pin(path) == pin_bytes(body.encode()), 'foreign bootstrap unit edit')
                target = transaction / 'retired-units' / str(index)
                require(not target.exists(), 'retired bootstrap unit occupied')
                os.rename(str(path), str(target))
                lifecycle.sync_dir(path.parent)
                lifecycle.sync_dir(target.parent)
        for key, relative in DIRS.items():
            live, old, retired = prepare.HOME / relative, transaction / 'old' / key, transaction / 'retired' / key
            before = approval['originalDirectories'][key]
            if old.exists():
                require(reprovision.identity(old) == before['identity'] and reprovision.tree(old) == before['tree'],
                        'original bootstrap recovery directory changed')
                if live.exists():
                    require(reprovision.identity(live) == state['directories'].get(key), 'foreign live bootstrap directory')
                    self.move(live, retired)
                self.move(old, live)
            require(reprovision.identity(live) == before['identity'] and reprovision.tree(live) == before['tree'],
                    'bootstrap original directory is not restored')
        lifecycle.systemctl('daemon-reload')
        require(prepare.origin() == approval['origin'], 'bootstrap stopped origin not restored')
        self.checkpoint(transaction, state, 'RECOVERED_STOPPED')
        pending = root() / 'pending.json'
        if pending.exists():
            pending.unlink()
            lifecycle.sync_dir(root())
        return transaction


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    p = sub.add_parser('plan')
    for name in ('prepared-transaction', 'prepared-sha256', 'transaction', 'release'):
        p.add_argument('--' + name, required=True)
    p.add_argument('--expires-at', type=int, required=True)
    p.add_argument('--health', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p = sub.add_parser('activate')
    p.add_argument('--approval', type=Path, required=True)
    p.add_argument('--approval-sha256', required=True)
    p = sub.add_parser('recover')
    p.add_argument('--transaction', required=True)
    p.add_argument('--approval-sha256', required=True)
    args = parser.parse_args()
    with lifecycle.production_lock():
        if args.action == 'plan':
            value = plan(args.prepared_transaction, args.prepared_sha256, args.transaction,
                         args.expires_at, args.release, reprovision.read_record(args.health))
            lifecycle.publish_new(args.output, reprovision.encoded(value))
            print(reprovision.digest(args.output))
        elif args.action == 'activate':
            print(Activation().apply(args.approval, args.approval_sha256))
        else:
            print(Activation().recover(args.transaction, args.approval_sha256))


if __name__ == '__main__':
    main()
