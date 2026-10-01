"""Real manager/flock/proc admission checks with private no-business endpoints.

Not full production boot, install, RSA/HTTPS, native initialization or OS reboot.
All source imports are pinned by the outer executor before this script starts.
"""
import ast
import configparser
import contextlib
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import types
from unittest import mock

ROOT = Path('/tmp/gw-nativeboot-082071e-23-C1')
PREFIX = 'gw-native-c1-082071e-'
TOKEN = '082071ee-0000-4000-8000-000000000001'
JOBS = ('C1_082071E_HAND', 'C1_082071E_DONOR', 'C1_082071E_UNKNOWN')
CHECKS = ('manager-cgroup-binding', 'gate-queues-readers-and-worker', 'worker-after-gate',
          'reader-after-gate', 'failed-gate-keeps-reader-closed', 'real-guard-rejects-private-unadopted-home',
          'ready-while-donor-holds-lease', 'owned-live-bindings-and-negative-identity',
          'donor-exit-transfers-original-lease', 'uncertain-dispatch-no-replay-original-cancel')


def need(value, reason):
    if not value:
        raise ValueError(reason)


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    return module


def write(path, data):
    need(ROOT in path.parents and path.resolve() == path, 'private output escaped')
    with path.open('xb') as output:
        output.write(data); output.flush(); os.fsync(output.fileno())


def wait(predicate, label, seconds=10):
    end = time.monotonic() + seconds
    while not predicate():
        need(time.monotonic() < end, 'deadline: ' + label)
        time.sleep(0.02)


def worker_identifier(home):
    value = dict(format='c1-dispatch-adapter-v1', phase='PREPARED', entryPath=str(ROOT / 'source'), home=str(home))
    return hashlib.sha256((json.dumps(value, sort_keys=True) + '\n').encode()).hexdigest()[:32]


def unit_names():
    return [PREFIX + part + '.service' for part in ('gate-ok', 'reader-ok', 'gate-fail', 'reader-fail', 'reader-guard')] + [
        'gateway-native-boot-' + worker_identifier(ROOT / 'audit/gate-ok') + '.service'] + [
        'ota-runtime-' + job + '-' + TOKEN + '.service' for job in JOBS]


def planned_commands():
    units = unit_names()
    entry = str(ROOT / 'source/private-entry.py')
    good, bad = str(ROOT / 'audit/gate-ok'), str(ROOT / 'audit/gate-fail')
    values = [
        ['/usr/bin/python3', '-I', '-B', entry, 'gate', good],
        ['/usr/bin/python3', '-I', '-B', entry, 'reader', good],
        ['/usr/bin/python3', '-I', '-B', entry, 'gate', bad],
        ['/usr/bin/python3', '-I', '-B', entry, 'reader', bad],
        ['/usr/bin/python3', '-I', '-B', entry, 'reader', str(ROOT / 'audit/unadopted')],
        ['/usr/bin/python3', '-B', entry, 'production-boot-finish', good]]
    for job in JOBS:
        staging = ROOT / 'audit' / job / 'data/ota'
        values.append(['/usr/bin/python3', '-B', str(staging / ('runtime-entry-' + job) / 'offline-runtime-upgrade.py'),
                       'ota-owner', str(staging), job, TOKEN])
    return dict(zip(units, values))


def publish_record(path, value):
    temporary = path.with_name(path.name + '.' + str(os.getpid()) + '.partial')
    with temporary.open('x') as output:
        json.dump(value, output, sort_keys=True)
        output.write('\n'); output.flush(); os.fsync(output.fileno())
    os.rename(str(temporary), str(path))  # Unique reservation owner; never a competing final publisher.


def reserve_command(directory, command):
    """O_EXCL slots bound the entire fork family, not copied Python counters."""
    if command[0] == '/usr/bin/systemd-run':
        unit = next(v.split('=', 1)[1] for v in command if v.startswith('--unit='))
        unit = unit if unit.endswith('.service') else unit + '.service'
        marker = directory / ('dispatch-' + unit)
        fd = os.open(str(marker), os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        os.close(fd)  # Never replay an uncertain submission, even if logging fails next.
    for number in range(160):
        slot = directory / ('%03d.slot' % number)
        try:
            fd = os.open(str(slot), os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        except FileExistsError:
            continue
        os.close(fd)
        request = directory / ('%03d.request.json' % number)
        publish_record(request, dict(index=number, pid=os.getpid(), argv=list(command)))
        return number
    raise ValueError('global manager command budget exceeds160')


def collect_commands(directory):
    rows = []
    for slot in sorted(directory.glob('*.slot')):
        index = int(slot.stem)
        request = directory / ('%03d.request.json' % index)
        value = json.loads(request.read_text()) if request.exists() else dict(index=index, pid=None, argv=None)
        response = directory / ('%03d.response.json' % value['index'])
        value['response'] = json.loads(response.read_text()) if response.exists() else dict(outcome='UNKNOWN')
        rows.append(value)
    need(len(rows) <= 160, 'global manager command budget drift')
    return rows


class ManagerChecks:
    def __init__(self):
        self.source = ROOT / 'source'
        self.audit = ROOT / 'audit'; self.audit.mkdir(mode=0o700)
        self.logs = ROOT / 'manager-commands'; self.logs.mkdir(mode=0o700)
        self.health = load('c1_actual_health', self.source / 'ota-health-result.py')
        self.offline = load('c1_actual_offline', self.source / 'offline-runtime-upgrade.py')
        self.original_run = subprocess.run
        self.units = unit_names(); self.commands = []; self.rows = []; self.unit_starts = {}
        self.result = dict(format='nativeboot-C1-manager-v1', checks=self.rows,
                           realLifecycleCalled=False, units=self.units, commands=self.commands,
                           adapters=[], managerMount=None)
        self.donor_pid = None

    def run(self, command, **kwargs):
        # All actual manager mutations are limited to our enumerated transient units.
        need(command[0] in ('/usr/bin/systemctl', '/usr/bin/systemd-run'), 'unexpected manager executable')
        if command[0].endswith('systemd-run'):
            unit = next(v.split('=', 1)[1] for v in command if v.startswith('--unit='))
            unit = unit if unit.endswith('.service') else unit + '.service'
            need(unit in self.units, 'unapproved transient unit')
        else:
            need(command[1] in ('show', 'stop', 'reset-failed'), 'unapproved manager command')
            need(command[2] in self.units, 'unapproved unit reference')
        kwargs.update(stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)
        kwargs.pop('check', None)
        number = reserve_command(self.logs, command)
        try:
            result = self.original_run(command, **kwargs)
        except BaseException as error:
            # Reservation and unknown dispatch survive; never fabricate exit0.
            partial_out = getattr(error, 'stdout', None) or b''
            partial_err = getattr(error, 'stderr', None) or b''
            if isinstance(partial_out, str): partial_out = partial_out.encode()
            if isinstance(partial_err, str): partial_err = partial_err.encode()
            raw = partial_out[:8192] + b'\n--- stderr ---\n' + partial_err[:8192]
            path = self.logs / ('%03d.log' % number); write(path, raw)
            publish_record(self.logs / ('%03d.response.json' % number), dict(outcome='UNKNOWN',
                  errorType=type(error).__name__, errorMessage=str(error), log=str(path), bytes=len(raw),
                  sha256=hashlib.sha256(raw).hexdigest()))
            raise
        out = result.stdout.encode() if isinstance(result.stdout, str) else result.stdout
        err = result.stderr.encode() if isinstance(result.stderr, str) else result.stderr
        need(len(out) + len(err) <= 16384, 'manager output exceeds limit')
        raw = out + b'\n--- stderr ---\n' + err
        path = self.logs / ('%03d.log' % number); write(path, raw)
        publish_record(self.logs / ('%03d.response.json' % number), dict(outcome='RETURNED',
              exitCode=result.returncode, bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest(), log=str(path)))
        self.commands[:] = collect_commands(self.logs)
        self.unit_starts = {u: 1 for u in self.units if (self.logs / ('dispatch-' + u)).exists()}
        if result.returncode:
            raise subprocess.CalledProcessError(result.returncode, command, output=result.stdout, stderr=result.stderr)
        return result

    def props(self, unit):
        raw = self.run(['/usr/bin/systemctl', 'show', unit, '--no-pager',
                        '--property=Id,LoadState,ActiveState,SubState,MainPID,ControlGroup,InvocationID,Job,Result,ExecMainStatus,After,Requires,ExecStartPre'], encoding='utf-8').stdout
        row = dict(line.split('=', 1) for line in raw.splitlines())
        need(row.get('Id') == unit, 'manager returned different unit')
        return row

    def check(self, case, **facts):
        need(case == CHECKS[len(self.rows)], 'case order drift')
        self.rows.append(dict(case=case, passed=True, **facts))

    def transient(self, unit, action, home, properties=()):
        command = ['/usr/bin/systemd-run', '--quiet', '--no-block', '--unit=' + unit,
                   '--property=Type=oneshot', '--property=RemainAfterExit=yes', '--property=Restart=no',
                   '--property=KillMode=control-group', '--property=TimeoutStartSec=30s', '--property=TimeoutStopSec=5s']
        command += ['--property=' + v for v in properties]
        self.run(command + ['/usr/bin/python3', '-I', '-B', str(self.source / 'private-entry.py'), action, str(home)])

    def gates(self):
        good = self.audit / 'gate-ok'; good.mkdir(mode=0o700)
        bad = self.audit / 'gate-fail'; bad.mkdir(mode=0o700)
        gate, reader, failgate, failreader, guardreader, worker = self.units[:6]
        original_gate = self.offline.production_gate(good, reader).decode()
        need(original_gate.count('gateway-ota-recover.service') == 2 and '%n' in original_gate,
             'product gate body changed; re-review adapter')
        mapped = original_gate.replace('gateway-ota-recover.service', gate)
        sections = configparser.ConfigParser(interpolation=None)
        sections.read_string(mapped)
        need(sections.sections() == ['Unit', 'Service'] and set(sections['Unit']) == {'requires', 'after'} and
             set(sections['Service']) == {'execstartpre'}, 'product gate property set changed')
        ordering = ('Requires=' + sections['Unit']['Requires'], 'After=' + sections['Unit']['After'])
        self.result['adapters'].append(dict(function='production_gate', original=original_gate, mapped=mapped,
                                           boundary='Only dependencies are exercised unchanged; positive ExecStartPre is a private observer. Negative uses actual guard. No full baseline validation.'))
        self.transient(gate, 'gate', good)
        wait(lambda: (good / 'gate-enter.json').exists(), 'gate enters')
        self.transient(reader, 'reader', good, ordering + (
                       'ExecStartPre=/usr/bin/python3 -I -B ' + str(self.source / 'private-entry.py') + ' pre-reader ' + str(good),))
        dispatch = good / 'dispatch.json'
        value = dict(format='c1-dispatch-adapter-v1', phase='PREPARED', entryPath=str(self.source), home=str(good))
        write(dispatch, (json.dumps(value, sort_keys=True) + '\n').encode())
        def dispatch_run(command, **kwargs):
            need(command[3] == '--unit=' + worker[:-8] and '--property=After=gateway-ota-recover.service' in command,
                 'product dispatch argv changed')
            mapped_command = [('--property=After=' + gate) if v == '--property=After=gateway-ota-recover.service' else
                              str(self.source / 'private-entry.py') if v == str(self.source / 'offline-runtime-upgrade.py') else v for v in command]
            self.result['adapters'].append(dict(function='production_boot_dispatch', original=command, mapped=mapped_command,
                boundary='Publication authority supplied by private fixture; actual DISPATCH_INTENT/DISPATCHED save and manager submission run. Finish is observation-only.'))
            return self.run(mapped_command, **kwargs)
        with mock.patch.object(self.offline, 'production_boot_publish', lambda *a: (dispatch, value)), \
             mock.patch.object(self.offline, 'subprocess', types.SimpleNamespace(run=dispatch_run, PIPE=subprocess.PIPE)):
            self.offline.production_boot_dispatch(types.SimpleNamespace(home=good), {})
        queued = dict(reader=self.props(reader), worker=self.props(worker))
        need(all(r['MainPID'] == '0' and r['Job'] for r in queued.values()), 'dependent unexpectedly started or unqueued')
        need(not (good / 'reader.json').exists() and not (good / 'worker.json').exists(), 'dependent observation before gate')
        self.check('gate-queues-readers-and-worker', properties=queued, gate=self.props(gate))
        write(good / 'gate-release', b'approved private gate release\n')
        wait(lambda: (good / 'worker.json').exists() and (good / 'reader.json').exists(), 'ordered observations')
        exit_time = json.loads((good / 'gate-exit.json').read_text())['at']
        worker_fact = json.loads((good / 'worker.json').read_text())
        need(worker_fact['at'] >= exit_time, 'worker preceded gate completion')
        self.check('worker-after-gate', observation=worker_fact, properties=self.props(worker), dispatch=json.loads(dispatch.read_text()))
        # A second actual dispatch invocation must NOT submit StartUnit again.
        with mock.patch.object(self.offline, 'production_boot_publish', lambda *a: (dispatch, json.loads(dispatch.read_text()))), \
             mock.patch.object(self.offline, 'subprocess', types.SimpleNamespace(run=dispatch_run, PIPE=subprocess.PIPE)):
            self.offline.production_boot_dispatch(types.SimpleNamespace(home=good), {})
        need(self.unit_starts[worker] == 1, 'gate replayed continuation')
        reader_fact = json.loads((good / 'reader.json').read_text())
        need(reader_fact['at'] >= exit_time, 'reader preceded gate')
        self.check('reader-after-gate', observation=reader_fact, properties=self.props(reader))
        self.transient(failgate, 'gate', bad)
        wait(lambda: (bad / 'gate-enter.json').exists(), 'failed gate enters')
        self.transient(failreader, 'reader', bad, ('Requires=' + failgate, 'After=' + failgate))
        write(bad / 'gate-release', b'intentional private gate rejection\n')
        wait(lambda: self.props(failgate)['ActiveState'] == 'failed', 'failed gate observed')
        wait(lambda: not self.props(failreader)['Job'], 'failed dependency job ends')
        closed = self.props(failreader)
        need(closed['MainPID'] == '0' and not (bad / 'reader.json').exists(), 'failed gate reader escaped')
        self.check('failed-gate-keeps-reader-closed', gate=self.props(failgate), reader=closed)
        # Actual product guard against missing private baseline, not a success stub.
        guard_home = self.audit / 'unadopted'; guard_home.mkdir(mode=0o700)
        self.transient(guardreader, 'reader', guard_home, ('Requires=' + gate, 'After=' + gate,
            'ExecStartPre=/usr/bin/python3 -B ' + str(self.source / 'runtime-upgrade-guard.py') + ' production-unit ' + str(guard_home) + ' ' + guardreader))
        wait(lambda: self.props(guardreader)['ActiveState'] == 'failed', 'real guard rejects')
        need(not (guard_home / 'reader.json').exists(), 'unadopted home started reader')
        self.check('real-guard-rejects-private-unadopted-home', properties=self.props(guardreader))

    def make_job(self, job):
        home = self.audit / job; home.mkdir(mode=0o700)
        staging = home / 'data/ota'; staging.mkdir(mode=0o700, parents=True)
        artifact = staging / 'artifact.bin'; write(artifact, b'private observation artifact; never installed\n')
        lease_path = staging / ('ota_job_' + job + '.lock')
        fd = os.open(str(lease_path), os.O_CREAT | os.O_EXCL | os.O_RDWR | os.O_NOFOLLOW, 0o600)
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB); info = os.fstat(fd)
        intent = dict(format='ota-job-v1', jobId=job, machineCode='PRIVATE_C1', version='0.0-C1', previousVersion='0',
                      token=TOKEN, lifecycle='systemd-handoff-v1', artifactPath=str(artifact),
                      artifactSize=str(artifact.stat().st_size), artifactSha256=hashlib.sha256(artifact.read_bytes()).hexdigest(),
                      leaseDev=str(info.st_dev), leaseInode=str(info.st_ino))
        write(staging / ('ota_job_' + job + '.json'), (json.dumps(intent) + '\n').encode())
        write(staging / 'ota_active_job.txt', ('jobId=' + job + '\ntoken=' + TOKEN + '\n').encode())
        entry = home / 'entry'; entry.mkdir(mode=0o700)
        for name in ('ota-health-result.py', 'runtime-upgrade-guard.py'):
            shutil.copyfile(self.source / name, entry / name); (entry / name).chmod(0o400)
        shutil.copyfile(self.source / 'private-entry.py', entry / 'offline-runtime-upgrade.py')
        (entry / 'offline-runtime-upgrade.py').chmod(0o400)
        return self.health.RuntimeHandoff(staging, job, TOKEN), fd, entry

    def observe_owned(self, protocol):
        wait(lambda: (protocol.root / 'owned-observed.json').exists(), 'owner observation')
        record = protocol.read(); need(record['phase'] == 'OWNER_OWNED' and not record['runtimeMayChange'], 'unexpected owner phase')
        properties = protocol.verify_owner(record)
        need(protocol.unit_process(record, properties) == record['owner'], 'exact owner command binding')
        try:
            with protocol.lease():
                raise ValueError('owned lease unexpectedly free')
        except BlockingIOError:
            pass
        return record

    def cancel_original(self, protocol):
        protocol.recover()  # Official unchanged cancellation only; no runtime mutation branch.
        record = protocol.read(); protocol.no_unit_tasks(record)
        need(record['phase'] == 'CANCELLED' and not record['runtimeMayChange'], 'original cancel not terminal')
        health = protocol.json_file(protocol.root / ('health_' + protocol.job + '.txt'))
        need(health['status'] == 'failed' and health['rollback'] == 'unchanged', 'false health decision')
        return dict(record=record, health=health, properties=protocol.properties())

    def handoffs(self):
        protocol, fd, entry = self.make_job(JOBS[0])
        try:
            protocol.launch(entry / 'offline-runtime-upgrade.py', fd)
            ready = protocol.read(); protocol.verify_owner(ready)
            need(ready['phase'] == 'OWNER_READY' and not (protocol.root / 'owned-observed.json').exists(), 'lease handoff premature')
            self.check('ready-while-donor-holds-lease', record=ready, properties=protocol.properties())
        finally:
            os.close(fd)
        owned = self.observe_owned(protocol)
        negatives = []
        for field, value in (('invocationId', '0' * 32), ('startTicks', '0'), ('cgroup', '/not-our-cgroup')):
            damaged = dict(owned, owner=dict(owned['owner']))
            if field == 'invocationId': damaged[field] = value
            else: damaged['owner'][field] = value
            try:
                protocol.verify_owner(damaged)
            except ValueError as error: negatives.append(dict(field=field, rejection=str(error)))
            else: raise ValueError('owner identity negative accepted: ' + field)
        cancelled = self.cancel_original(protocol)
        self.check('owned-live-bindings-and-negative-identity', owned=owned, negatives=negatives, cancelled=cancelled)
        protocol, fd, entry = self.make_job(JOBS[1])
        donor = os.fork(); self.donor_pid = donor
        if donor == 0:
            try:
                protocol.launch(entry / 'offline-runtime-upgrade.py', fd)
                write(protocol.root / 'donor-ready', str(os.getpid()).encode())
                wait(lambda: (protocol.root / 'donor-exit').exists(), 'donor exit request')
                write(protocol.root / 'donor-result.json', (json.dumps(dict(pid=os.getpid(), exitCode=0,
                      commands=collect_commands(self.logs))) + '\n').encode())
                os._exit(0)
            except BaseException as error:
                write(protocol.root / 'donor-result.json', (json.dumps(dict(pid=os.getpid(), exitCode=1,
                      errorType=type(error).__name__, errorMessage=str(error)[:1024],
                      commands=collect_commands(self.logs))) + '\n').encode())
                os._exit(1)
        os.close(fd)  # Close only; do not unlock the child's inherited donor lease.
        wait(lambda: (protocol.root / 'donor-ready').exists(), 'donor ready')
        ready = protocol.read(); need(ready['phase'] == 'OWNER_READY', 'donor window not ready')
        write(protocol.root / 'donor-exit', b'owned donor only\n')
        wait(lambda: not Path('/proc', str(donor)).exists() or 'Z' == Path('/proc', str(donor), 'stat').read_text().rsplit(')', 1)[1].split()[0], 'donor exit')
        waited, status = os.waitpid(donor, 0); self.donor_pid = None
        need(waited == donor and status == 0, 'donor failed')
        owned = self.observe_owned(protocol)
        self.check('donor-exit-transfers-original-lease', donorPid=donor, ready=ready, owned=owned,
                   cancelled=self.cancel_original(protocol))
        protocol, fd, entry = self.make_job(JOBS[2])
        original = protocol.command; attempts = []
        def uncertain(*args):
            output = original(*args)
            if args[0] == '/usr/bin/systemd-run':
                attempts.append(args)
                raise subprocess.TimeoutExpired(args, 15, output=output)
            return output
        try:
            with mock.patch.object(protocol, 'command', uncertain):
                try: protocol.launch(entry / 'offline-runtime-upgrade.py', fd)
                except subprocess.TimeoutExpired: pass
                else: raise ValueError('uncertain-dispatch injection missing')
                try: protocol.launch(entry / 'offline-runtime-upgrade.py', fd)
                except ValueError as error: need('already consumed' in str(error), 'wrong replay rejection')
                else: raise ValueError('uncertain handoff replay accepted')
            need(len(attempts) == 1, 'uncertain dispatch resent')
        finally:
            os.close(fd)
        owned = self.observe_owned(protocol)
        self.check('uncertain-dispatch-no-replay-original-cancel', managerCalls=len(attempts), owned=owned,
                   cancelled=self.cancel_original(protocol))

    def execute(self):
        try:
            # Actual product class methods use the real manager, kernel proc and locks.
            with mock.patch.object(self.health.subprocess, 'run', self.run):
                for unit in self.units:
                    props = self.props(unit)
                    need(props['LoadState'] == 'not-found' and props['MainPID'] == '0' and not props['Job'], 'unit namespace occupied')
                mount = self.health.RuntimeHandoff.cgroup_mount()
                self.result['managerMount'] = mount
                self.check('manager-cgroup-binding', mount=mount,
                           controller=self.health.RuntimeHandoff.process(os.getpid()))
                self.gates(); self.handoffs()
                need([r['case'] for r in self.rows] == list(CHECKS), 'incomplete checks')
                self.result['passed'] = True
        except BaseException as error:
            self.result.update(passed=False, errorType=type(error).__name__, errorMessage=str(error))
        finally:
            # Outer runner independently repeats stop/absence proof for fixed units,
            # including if this process is killed. No business unit is eligible.
            self.commands[:] = collect_commands(self.logs)
            self.result['dispatchCounts'] = {u: 1 for u in self.units if (self.logs / ('dispatch-' + u)).exists()}
            if any(row['response']['outcome'] == 'UNKNOWN' for row in self.commands):
                self.result['passed'] = False
                self.result['unknownManagerResult'] = True
            write(ROOT / 'manager-result.json', (json.dumps(self.result, sort_keys=True) + '\n').encode())
            print(json.dumps(self.result, sort_keys=True))
        return 0 if self.result.get('passed') else 1


if __name__ == '__main__':
    need(len(sys.argv) == 3 and Path(sys.argv[1]) == ROOT and os.geteuid() == 0, 'fixed root/root UID required')
    # sys.argv[2] is the parent's namespace proof retained by the reused outer runner.
    raise SystemExit(ManagerChecks().execute())
