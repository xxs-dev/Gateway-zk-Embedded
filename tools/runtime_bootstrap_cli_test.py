#!/usr/bin/env python3
"""Unmodified lifecycle CLI with real native binaries in a private chroot.

The manager is a test double; pointctl, Monitor, Qt, guards, process/mapping
health and HTTP checks are real. No host service, SHM, device or network is used.
The caller supplies pinned factory/tools/project artifacts in a private /srv
directory. Evidence and the private installation are retained, not deleted.
"""
import argparse
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tarfile
import time

HOME = Path('/opt/modbus-gateway')
ROOT = Path('/opt/bootstrap-cli')
DEPLOY = ROOT / 'bundle/deploy'
UNITS = Path('/etc/systemd/system')
EVIDENCE = ROOT / 'evidence'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write(path, raw, mode=0o644):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(raw.encode() if isinstance(raw, str) else raw)
    path.chmod(mode)


def record(path, value):
    write(path, json.dumps(value, sort_keys=True, indent=2) + '\n', 0o600)


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, str(path))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run(args, label, ok=True, timeout=300, env=None):
    try:
        result = subprocess.run(list(map(str, args)), env=env, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        write(EVIDENCE / (label + '.log'), error.stdout or b'', 0o600)
        print(label, 'TIMEOUT', timeout, flush=True)
        raise
    write(EVIDENCE / (label + '.log'), result.stdout, 0o600)
    print(label, result.returncode, result.stdout[-2500:].decode(errors='replace'), flush=True)
    if (result.returncode == 0) != ok:
        raise RuntimeError('unexpected CLI result: ' + label)
    return result.stdout.decode()


def cli(helper, *args, **kwargs):
    return run([sys.executable, '-B', DEPLOY / helper, *args], **kwargs)


def manager(args):
    state_path = ROOT / 'manager.json'
    state = json.loads(state_path.read_text()) if state_path.exists() else {'live': {}, 'loaded': {}}
    with (EVIDENCE / 'manager.log').open('a') as log:
        log.write(json.dumps(args) + '\n')

    def files():
        return {str(p): sha(p) for p in UNITS.rglob('*') if p.is_file()}

    def alive(unit):
        pid = state['live'].get(unit)
        if pid == 0:
            return True
        if pid is None:
            return False
        try:
            return Path('/proc', str(pid), 'stat').read_text().rsplit(')', 1)[1].split()[0] != 'Z'
        except FileNotFoundError:
            return False

    def environment(unit):
        result = {}
        for path in sorted((UNITS / (unit + '.d')).glob('*.conf')):
            for line in path.read_text().splitlines():
                if line.startswith('Environment='):
                    key, value = line[len('Environment='):].split('=', 1)
                    result[key] = value
        return result

    def prop(unit, name):
        template = unit.split('@')[0] + '@.service' if '@' in unit else unit
        fragment = UNITS / template
        values = {'Id': unit, 'LoadState': 'loaded' if fragment.exists() else 'not-found',
            'ActiveState': 'active' if alive(unit) else 'inactive',
            'SubState': 'running' if alive(unit) and state['live'].get(unit) else 'exited' if alive(unit) else 'dead',
            'MainPID': str(state['live'].get(unit, 0)) if alive(unit) else '0', 'NRestarts': '0',
            'NeedDaemonReload': 'no' if files() == state['loaded'] else 'yes',
            'FragmentPath': str(fragment) if fragment.exists() else '',
            'DropInPaths': ' '.join(p for p in sorted(state['loaded']) if
                Path(p).parent.name in {unit + '.d', template + '.d', 'service.d'}),
            'Environment': ' '.join(k + '=' + v for k, v in environment(unit).items()),
            'EnvironmentFiles': str(HOME / 'config/runtime/qt-display.env') + ' (ignore_errors=yes)'
                if unit == 'ky-ems.service' else '',
            'PassEnvironment': '', 'UnsetEnvironment': '', 'PAMName': ''}
        if name not in values:
            raise ValueError('unsupported manager property: ' + name)
        return values[name]

    if args[0] == 'daemon-reload':
        state['loaded'] = files()
    elif args[0] in ('list-units', 'list-unit-files'):
        names = {p.name for p in UNITS.glob('*.service')}
        names.update(p.name[:-2] for p in UNITS.glob('*.service.d'))
        for name in sorted(names):
            print(name + ' loaded')
    elif args[0] == 'show':
        fields = next(a[len('--property='):] for a in args if a.startswith('--property=')).split(',')
        names = [a for a in args[1:] if not a.startswith('-')]
        for unit in names:
            for field in fields:
                print(prop(unit, field) if '--value' in args else field + '=' + prop(unit, field))
            if '--value' not in args:
                print()
    elif args[0] == 'is-active':
        if not alive(args[-1]):
            return 1
    elif args[0] == 'start':
        unit = args[1]
        commands = {'system-monitor@monitor-service.service': [str(HOME / 'bin/SystemMonitor'),
            '--app-config', str(HOME / 'config/runtime/apps/monitor-service.json')],
            'ky-ems.service': ['/bin/sh', str(HOME / 'bin/gateway-qt-native-run.sh')]}
        assert unit in set(commands) | {'gateway-services.service'}
        guard = load('cli_manager_guard', HOME / 'bin/runtime-upgrade-guard.py')
        guard.production_unit(HOME, unit)
        if unit == 'gateway-services.service':
            state['live'][unit] = 0
        elif not alive(unit):
            env = dict(os.environ, **environment(unit))
            if unit == 'ky-ems.service':
                path = HOME / 'config/runtime/qt-display.env'
                env.update(guard.production_environment_file(path, sha(path)))
            with (EVIDENCE / (unit + '.log')).open('ab') as log:
                child = subprocess.Popen(commands[unit], cwd=str(HOME), env=env,
                    stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            state['live'][unit] = child.pid
            record(state_path, state)
            expected = str(HOME / ('bin/SystemMonitor' if unit.startswith('system-monitor') else 'ky-ems/KY-EMS'))
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                if child.poll() is not None:
                    raise RuntimeError('native child exited: ' + unit)
                if os.readlink('/proc/' + str(child.pid) + '/exe') == expected:
                    time.sleep(2)
                    break
                time.sleep(.05)
            else:
                raise RuntimeError('native child did not exec: ' + unit)
    elif args[0] == 'stop':
        for unit in args[1:]:
            pid = state['live'].get(unit)
            if pid and alive(unit):
                os.killpg(pid, signal.SIGTERM)
                deadline = time.monotonic() + 15
                while alive(unit) and time.monotonic() < deadline:
                    time.sleep(.05)
                if alive(unit):
                    raise RuntimeError('native stop timed out: ' + unit)
            state['live'].pop(unit, None)
    else:
        raise ValueError('unsupported manager operation: ' + repr(args))
    record(state_path, state)
    return 0


def extract(path, destination, prefix):
    destination.mkdir()
    with tarfile.open(str(path)) as archive:
        for item in archive.getmembers():
            name = Path(item.name)
            assert not name.is_absolute() and '..' not in name.parts and name.parts[0] == prefix
            assert item.isfile() or item.isdir()
            assert item.uid == item.gid == 0 and item.mode & 0o022 == 0
        archive.extractall(str(destination))


def prepare_inputs(suffix):
    prepare = load('cli_prepare_inputs', DEPLOY / 'runtime-bootstrap.py')
    source = ROOT / ('config-' + suffix)
    source.mkdir(mode=0o700)
    for name in ('device_identity.json', 'edge-package-manifest.json'):
        shutil.copy2(str(HOME / 'config/runtime' / name), str(source / name))
    for name in ('network', 'tls'):
        if (HOME / 'config/runtime' / name).exists():
            shutil.copytree(str(HOME / 'config/runtime' / name), str(source / name))
    data_name, monitor_name = 'cli_data_' + suffix, 'cli_monitor_' + suffix
    common = dict(runtimeMode='ems', identityConfigFile=str(HOME / 'config/runtime/device_identity.json'),
        deviceConfigFiles=[], mqtt={'enabled': False},
        mqttDriver={'enabled': False, 'sharedMemoryName': data_name, 'sharedMemoryNames': [data_name]},
        localDisplay={'enabled': False}, systemMonitor={'enabled': False})
    record(source / 'apps/mqtt-service.json', common)
    monitor = copy.deepcopy(common)
    monitor['systemMonitor'] = dict(enabled=True, diagEnabled=False, defaultIntervalMs=1000,
        cellular={'enabled': False}, recordingTransfer={'enabled': False}, scadaUpperComputerSafety={'enabled': False},
        directMaintenance=dict(enabled=True, listenHost='127.0.0.1', listenHosts=['127.0.0.1'],
            listenPort=29443, allowedClientCidrs=['127.0.0.1/32'],
            identityConfigFile=str(HOME / 'config/runtime/device_identity.json'),
            appConfigFile=str(HOME / 'config/runtime/apps/monitor-service.json'),
            otaAppConfigFile=str(HOME / 'config/runtime/apps/monitor-service.json'),
            authStateFile=str(HOME / 'run/direct-maintenance-state.json'),
            credentialFile=str(HOME / 'run/direct-maintenance-credential.json'),
            otaStatusFile=str(HOME / 'run/ota-status.jsonl'), includeKyEmsSnapshot=False,
            realtimeHeartbeatRequiresAuth=True, maxRealtimePoints=2000))
    monitor['localDisplay'] = dict(enabled=True, renderer='nativeqt', sharedMemoryNames=[data_name],
        scada=dict(enabled=True, projectDirectory=str(HOME / 'scada/current'), autoReload=True),
        kiosk=dict(enabled=False, width=1280, height=720, setDisplayMode=False, requireDisplayConnected=False))
    record(source / 'apps/monitor-service.json', monitor)
    write(source / 'qt-display.env', 'QT_QPA_PLATFORM=offscreen\nXDG_RUNTIME_DIR=/run/qt-bootstrap\n'
        'GATEWAY_SYSTEM_MONITOR_SHARED_MEMORY_NAME=' + monitor_name + '\n')
    authority = dict(machineCode='COMM202600998', identitySha256=sha(source / 'device_identity.json'),
        networkSha256={}, tlsSha256={}, otaPublicKeySha256=sha(prepare.TRUST), monitorShmName=monitor_name)
    for directory, key in (('network', 'networkSha256'), ('tls', 'tlsSha256')):
        authority[key] = {p.relative_to(source).as_posix(): sha(p) for p in (source / directory).rglob('*') if p.is_file()}
    resolved = prepare.policy.resolve(source, UNITS, authority, prepare.TRUST, capabilities=['display-simulator'])
    profile = dict(format='board-simulator-profile-v1', machineCode='COMM202600998', ramMiB=2048,
        capabilities=['display-simulator'], simulatorBindings={}, allowedUnits=[r['unit'] for r in resolved['roles']],
        segmentNames=sorted({s for role in resolved['roles'] for s in role['shm']['names']}))
    trust = dict(requireSignature=True, checksumRequired=True, allowInsecureHttp=False,
        publicKeySha256=authority['otaPublicKeySha256'], httpsOrigins=['https://bootstrap-fixture.invalid'])
    for name, value in (('authority', authority), ('profile', profile), ('trust', trust)):
        record(ROOT / (name + '-' + suffix + '.json'), value)
    return source, profile


def exercise_upgrades():
    lifecycle = load('cli_upgrade', DEPLOY / 'offline-runtime-upgrade.py')
    policy = load('cli_upgrade_policy', DEPLOY / 'production_role_policy.py')
    guard = lifecycle.guard
    runtime = lifecycle.ProductionRuntime(HOME)
    cases = []
    for name, fail in (('CONFIG_NEXT', False), ('CONFIG_FAIL', True)):
        previous = guard.production_context(HOME)
        pointer = json.loads((runtime.root / 'current.json').read_text())
        payload = ROOT / name
        payload.mkdir(mode=0o700)
        for relative in previous['files']:
            target = payload / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(str(HOME / relative), str(target))
        shutil.copytree(str(HOME / 'config/runtime'), str(payload / 'config/runtime'))
        relative = 'apps/monitor-service.json'
        changed = payload / 'config/runtime' / relative
        value = json.loads(changed.read_text())
        value['systemMonitor']['reportIntervalMs'] = 5000
        if fail:
            value['systemMonitor']['directMaintenance']['listenPort'] = 29445
        record(changed, value)
        resolved = policy.resolve(payload / 'config/runtime', UNITS,
            previous['roleAuthority']['resolved']['protected'], Path('/etc/modbus-gateway/ota.pub'),
            capabilities=['display-simulator'])
        candidate = copy.deepcopy(previous)
        candidate.update(baselineId=name, generation=previous['generation'] + 1,
            parentSha256=pointer['sha256'], release=name, configSha256=resolved['configSha256'])
        candidate['roleAuthority']['resolved'] = resolved
        approval = ROOT / (name + '.json')
        record(approval, dict(format='runtime-provisioning-v2', transaction=name, operation='install',
            expiresAtUnix=int(time.time()) + 900, gatewayHome=str(HOME), machineCode=previous['machineCode'],
            parentSha256=pointer['sha256'], previous=previous, candidate=candidate, legacy=None,
            configWriteSet=[dict(path=relative, before=guard.production_pin(HOME / 'config/runtime' / relative),
                after=guard.production_pin(changed))]))
        transaction = runtime.root / 'transactions' / name
        output = cli('offline-runtime-upgrade.py', 'production-install', '--approval', approval,
            '--approval-sha256', sha(approval), '--payload', payload, '--state', transaction,
            label=name, ok=not fail, timeout=300)
        state = json.loads((transaction / 'state.json').read_text())
        record(EVIDENCE / (name + '-install-state.json'), state)
        if fail:
            assert 'production endpoint health failed' in output
            assert state['phase'] == 'FAILED_PENDING_RECOVERY'
            assert sha(HOME / 'config/runtime' / relative) == sha(changed)
        cli('offline-runtime-upgrade.py', 'production-recover', '--state', transaction,
            '--approval-sha256', sha(approval), label=name + '-recover', timeout=300)
        current = guard.production_context(HOME)
        assert current == (previous if fail else candidate)
        assert current['historical'] == previous['historical']
        record(EVIDENCE / (name + '-health.json'), runtime.health(current))
        final = json.loads((transaction / 'state.json').read_text())
        assert final['phase'] == ('RECOVERED' if fail else 'COMPLETED')
        cases.append(dict(transaction=name, phase=final['phase'], generation=current['generation']))
    return cases


def exercise():
    assert os.getpid() == 1 and os.geteuid() == 0
    status = dict(line.split(':', 1) for line in Path('/proc/self/status').read_text().splitlines())
    assert all(int(status[key].strip(), 16) == 0 for key in ('CapEff', 'CapPrm', 'CapBnd', 'CapInh', 'CapAmb'))
    assert status['NoNewPrivs'].strip() == '1'
    assert status['Seccomp'].strip() == '2'
    assert not Path('/run/systemd/private').exists() and not Path('/run/dbus/system_bus_socket').exists()
    assert not list(Path('/dev/shm').iterdir()) and len(Path('/proc/sysvipc/shm').read_text().splitlines()) == 1
    assert not any(Path('/dev').glob('tty*')) and not any(Path('/dev').glob('gpio*'))
    links = json.loads(subprocess.check_output(['ip', '-j', 'link', 'show']))
    assert any(row['ifname'] == 'lo' for row in links)
    assert all(row['ifname'] == 'lo' or row['ifname'] == 'sit0' and 'UP' not in row.get('flags', []) for row in links)
    routes = []
    for family in ('-4', '-6'):
        routes.extend(json.loads(subprocess.check_output(['ip', '-j', family, 'route', 'show', 'table', 'all'])))
    assert all(row.get('dev') == 'lo' for row in routes)
    for family, kind, proto in ((socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW),
        (socket.AF_PACKET, socket.SOCK_RAW, 0), (socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_ICMP)):
        try:
            probe = socket.socket(family, kind, proto)
        except OSError:
            continue
        probe.close()
        raise AssertionError('raw control socket not denied')
    record(EVIDENCE / 'isolation.json', dict(pid=1, capabilities='zero', noNewPrivileges=True,
        links=links, hostManagerHidden=True, physicalDevicesHidden=True, privateShm=True))
    pins = json.loads((ROOT / 'inputs.json').read_text())
    for name, pin in pins.items():
        assert sha(ROOT / name) == pin
    extract(ROOT / 'factory.tar.gz', ROOT / 'factory', 'gateway-factory-defaults')
    extract(ROOT / 'tools.tar.gz', ROOT / 'tools', 'gateway-bootstrap-tools')
    (ROOT / 'tools/gateway-bootstrap-tools').rename(ROOT / 'bundle')
    package = ROOT / 'factory/gateway-factory-defaults'
    HOME.mkdir(parents=True)
    env = dict(os.environ, INSTALL_SYSTEMD='0', GATEWAY_HOME=str(HOME), INIT_WORK_DIR=str(ROOT / 'init-work'))
    run(['/bin/sh', package / 'deploy/production-init.sh', '--auto', '--package', ROOT / 'factory.tar.gz',
        '--runtime-mode', 'ems', '--package-profile', 'project', '--machine-code', 'COMM202600998',
        '--mqtt-broker', 'tcp://127.0.0.1:1883', '--no-mqtt-tls', '--no-start', '--no-smoke',
        '--no-direct-maintenance'], 'factory-install', env=env)
    run(['/bin/sh', HOME / 'bin/install-scada-project.sh', '--package', ROOT / 'ems2.kyscada',
        '--machine-code', 'COMM202600998', '--app-config', HOME / 'config/runtime/apps/monitor-service.json',
        '--enable-native-display', '--state-file', ROOT / 'project-state.txt'], 'project-install', env=env)
    manifest = json.loads((HOME / 'config/runtime/edge-package-manifest.json').read_text())
    assert len(manifest['components']) == 18 and manifest['qtDataSource'] == 'native-point-store'
    for item in manifest['components']:
        path = HOME / ('ky-ems/KY-EMS' if item['binary'] == 'KY-EMS' else 'bin/' + item['binary'])
        assert sha(path) == item['sha256']
        assert b'not found' not in subprocess.check_output(['ldd', str(path)])
    tools = json.loads((ROOT / 'bundle/manifest.json').read_text())
    for name, pin in tools['factoryPairedHelpers'].items():
        assert sha(HOME / 'bin' / name) == pin == sha(DEPLOY / name)
    image = ROOT / 'image'
    HOME.rename(image)
    for name in ('bin', 'config/runtime', 'data', 'run'):
        (HOME / name).mkdir(parents=True, exist_ok=True)
    shutil.copy2(str(image / 'config/runtime/device_identity.json'), str(HOME / 'config/runtime/device_identity.json'))
    write(HOME / 'bin/gateway-services.sh', '#!/bin/sh\nexit 97\n', 0o755)
    write(HOME / 'bin/runtime-upgrade-guard.py', '# stopped original fixture\n')
    write(UNITS / 'gateway-services.service', '[Service]\nExecStart=/bin/false\n')
    unit_source = ROOT / 'unit-source'
    unit_source.mkdir()
    reprovision = load('cli_reprovision', DEPLOY / 'gateway-reprovision.py')
    for path in (package / 'deploy').glob('*.service'):
        if reprovision.unit_name(path.name):
            shutil.copy2(str(path), str(unit_source / path.name))
    write(unit_source / 'system-monitor@monitor-service.service.d/10-instance.conf',
        '[Unit]\nDescription=Bootstrap CLI monitor fixture\n')
    write(ROOT / 'fake-bin/systemctl', '#!/bin/sh\nexec /usr/bin/python3 -B /opt/bootstrap-cli/entry.py --manager "$@"\n', 0o755)
    os.environ['PATH'] = str(ROOT / 'fake-bin') + ':/usr/sbin:/usr/bin:/sbin:/bin'
    approval = ROOT / 'reprovision.json'
    cli('gateway-reprovision.py', 'plan', '--image', image, '--units', unit_source,
        '--transaction', 'CLI_REPROVISION', '--expires-at', int(time.time()) + 3600,
        '--output', approval, label='reprovision-plan')
    cli('gateway-reprovision.py', 'apply', '--approval', approval, '--approval-sha256', sha(approval), label='reprovision-apply')
    origin = reprovision.tree(HOME)
    Path('/etc/modbus-gateway').mkdir()
    Path('/run/qt-bootstrap').mkdir(mode=0o700)
    run(['openssl', 'genpkey', '-algorithm', 'RSA', '-pkeyopt', 'rsa_keygen_bits:2048', '-out', ROOT / 'fixture-only.key'], 'fixture-key')
    run(['openssl', 'pkey', '-in', ROOT / 'fixture-only.key', '-pubout', '-out', '/etc/modbus-gateway/ota.pub'], 'fixture-public-key')
    cases = []
    try:
        for suffix, fail in (('failed', True), ('healthy', False)):
            source, profile = prepare_inputs(suffix)
            prepare_tx, activate_tx = 'PREPARE_' + suffix, 'ACTIVATE_' + suffix
            plan = ROOT / (prepare_tx + '.json')
            cli('runtime-bootstrap.py', 'plan', '--config', source, '--creator', HOME / 'bin/pointctl',
                '--authority', ROOT / ('authority-' + suffix + '.json'), '--profile', ROOT / ('profile-' + suffix + '.json'),
                '--trust', ROOT / ('trust-' + suffix + '.json'), '--transaction', prepare_tx,
                '--expires-at', int(time.time()) + 1800, '--output', plan, label=prepare_tx + '-plan')
            cli('runtime-bootstrap.py', 'prepare', '--approval', plan, '--approval-sha256', sha(plan), label=prepare_tx)
            ready = Path('/opt/gateway-runtime-bootstrap/transactions') / prepare_tx / 'ready.json'
            health = ROOT / ('health-' + suffix + '.json')
            record(health, dict(stableSeconds=2, timeoutSeconds=3,
                url='http://127.0.0.1:' + ('29444' if fail else '29443') + '/api/v1/realtime/points'))
            active = ROOT / (activate_tx + '.json')
            cli('runtime-bootstrap-activate.py', 'plan', '--prepared-transaction', prepare_tx,
                '--prepared-sha256', sha(ready), '--transaction', activate_tx, '--expires-at', int(time.time()) + 900,
                '--release', 'cli-dfb8108', '--health', health, '--output', active, label=activate_tx + '-plan')
            cli('runtime-bootstrap-activate.py', 'activate', '--approval', active, '--approval-sha256', '0' * 64,
                label=activate_tx + '-wrong-pin', ok=False)
            assert reprovision.tree(HOME) == origin
            manager_offset = (EVIDENCE / 'manager.log').stat().st_size
            output = cli('runtime-bootstrap-activate.py', 'activate', '--approval', active,
                '--approval-sha256', sha(active), label=activate_tx, ok=not fail)
            transaction = Path('/opt/gateway-runtime-bootstrap/activations') / activate_tx
            state = json.loads((transaction / 'state.json').read_text())
            record(EVIDENCE / (activate_tx + '-state.json'), state)
            cli('runtime-bootstrap-activate.py', 'recover', '--transaction', activate_tx,
                '--approval-sha256', sha(active), label=activate_tx + '-repeat-recovery')
            if fail:
                assert 'production endpoint health failed' in output
                events = (EVIDENCE / 'manager.log').read_text()[manager_offset:].splitlines()
                started = {event[1] for event in map(json.loads, events) if event[0] == 'start'}
                assert started == {'system-monitor@monitor-service.service', 'ky-ems.service', 'gateway-services.service'}
                assert state['phase'] == 'RECOVERED_STOPPED' and reprovision.tree(HOME) == origin
                assert not reprovision.processes()
                assert (transaction / 'retired/run').is_dir()
                cli('runtime-bootstrap.py', 'recover', '--transaction', prepare_tx,
                    '--approval-sha256', sha(plan), label=prepare_tx + '-abort')
            else:
                assert state['phase'] == 'COMPLETED'
                baseline = load('cli_current_guard', HOME / 'bin/runtime-upgrade-guard.py').production_context(HOME)
                assert baseline['generation'] == 1 and baseline['parentSha256'] is None
                assert set(baseline['allowedUnits']) == {'system-monitor@monitor-service.service', 'ky-ems.service', 'gateway-services.service'}
                record(EVIDENCE / 'first-baseline.json', baseline)
                current = json.loads((HOME / 'data/runtime-production/current.json').read_text())
                proof = HOME / 'data/runtime-production/transactions' / current['transaction'] / 'health.json'
                record(EVIDENCE / 'real-process-health.json', json.loads(proof.read_text()))
            cases.append(dict(transaction=activate_tx, phase=state['phase'], nativeStores=profile['segmentNames']))
        upgrades = exercise_upgrades()
    finally:
        run(['systemctl', 'stop', 'ky-ems.service', 'system-monitor@monitor-service.service', 'gateway-services.service'], 'cleanup')
        while True:
            try:
                if os.waitpid(-1, os.WNOHANG)[0] == 0:
                    break
            except ChildProcessError:
                break
    assert not reprovision.processes()
    regressions = ['runtime_bootstrap_activation_test.py', 'runtime_bootstrap_test.py',
        'production_role_policy_test.py', 'production_monitor_binding_test.py']
    for pattern in regressions:
        assert (ROOT / 'bundle/tools' / pattern).is_file()
        run([sys.executable, '-B', '-m', 'unittest', 'discover', '-s', ROOT / 'bundle/tools',
            '-p', pattern, '-v'], 'packaged-' + pattern, timeout=300)
    record(EVIDENCE / 'result.json', dict(passed=True, cases=cases, sourceCommit=tools['sourceCommit'],
        configUpgradeCases=upgrades,
        packagedRegressionGroups=regressions,
        serviceManager='test-double', nativeBinaries=True, realProcessHealth=True,
        canonicalSystemdActivationAccepted=False, physicalControlEnabled=False))
    print('BOOTSTRAP_REAL_CLI_NATIVE_MONITOR_QT_HEALTH_RECOVERY_PASS', flush=True)


def enter(args):
    for name, value in json.loads(args.namespaces).items():
        assert os.readlink('/proc/self/ns/' + name) != value
    backing = args.root / 'backing'
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
    for name in ('null', 'zero', 'random', 'urandom'):
        target = backing / 'dev' / name
        target.touch()
        subprocess.run(['mount', '--bind', '/dev/' + name, str(target)], check=True)
    (backing / 'proc').mkdir()
    subprocess.run(['mount', '-t', 'proc', 'proc', str(backing / 'proc')], check=True)
    (backing / 'dev/shm').mkdir()
    subprocess.run(['ip', 'link', 'set', 'lo', 'up'], check=True)
    os.chroot(str(backing))
    os.chdir('/')
    for target in ('/dev/shm', '/usr/lib/systemd/system', '/usr/local/lib/systemd/system'):
        if Path(target).exists():
            subprocess.run(['mount', '-t', 'tmpfs', '-o', 'mode=755,nosuid,nodev,size=128M', 'tmpfs', target], check=True)
    caps = '-sys_admin,-sys_chroot,-net_admin,-setpcap'
    os.environ['LC_ALL'] = 'C'
    os.execv('/usr/bin/setpriv', ['setpriv', '--no-new-privs', '--bounding-set=' + caps,
        '--inh-caps=' + caps, '--ambient-caps=' + caps,
        '/usr/bin/python3', '-B', '/opt/bootstrap-cli/entry.py', '--execute'])


def main():
    if len(sys.argv) > 1 and sys.argv[1] == '--manager':
        return manager(sys.argv[2:])
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path)
    parser.add_argument('--namespaces')
    parser.add_argument('--execute', action='store_true')
    args = parser.parse_args()
    assert os.geteuid() == 0 and os.uname().machine == 'aarch64'
    if args.execute:
        return exercise()
    if args.namespaces:
        return enter(args)
    assert args.root.parent == Path('/srv') and re.fullmatch('gateway-bootstrap-cli-[A-Za-z0-9-]+', args.root.name)
    assert args.root.resolve() == args.root and stat.S_IMODE(args.root.stat().st_mode) == 0o700
    backing = args.root / 'backing'
    stage = backing / ROOT.relative_to('/')
    stage.mkdir(parents=True)
    (stage / 'evidence').mkdir(mode=0o700)
    (backing / 'etc/systemd/system').mkdir(parents=True)
    for name in ('ld.so.cache', 'passwd', 'group', 'nsswitch.conf'):
        shutil.copy2('/etc/' + name, str(backing / 'etc' / name))
    if Path('/etc/fonts').is_dir():
        shutil.copytree('/etc/fonts', str(backing / 'etc/fonts'), symlinks=True)
    if Path('/etc/alternatives').is_dir():
        shutil.copytree('/etc/alternatives', str(backing / 'etc/alternatives'), symlinks=True)
    for name in ('factory.tar.gz', 'tools.tar.gz', 'ems2.kyscada', 'inputs.json'):
        shutil.copy2(str(args.root / name), str(stage / name))
    shutil.copy2(str(Path(__file__).resolve()), str(stage / 'entry.py'))
    namespaces = {name: os.readlink('/proc/self/ns/' + name) for name in ('mnt', 'net', 'pid', 'ipc')}
    return subprocess.call(['unshare', '--mount', '--net', '--ipc', '--pid', '--mount-proc', '--fork',
        sys.executable, '-B', str(Path(__file__).resolve()), '--root', str(args.root), '--namespaces', json.dumps(namespaces)])


if __name__ == '__main__':
    sys.exit(main())
