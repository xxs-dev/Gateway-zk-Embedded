"""Sequential legacy-only handover with independent device-side rollback."""
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import time

EXE = '/usr/local/bin/easytier-core'
CFG = '/etc/easytier/et.conf'
RC = Path('/etc/rc.local')
UNIT = Path('/etc/systemd/system/easytier.service')
NAME = 'easytier.service'
LINE = b'    /usr/local/bin/easytier-core -c /etc/easytier/et.conf &'
COMMENT = b'    # EasyTier is managed only by easytier.service.'
ENV_KEEP = ('HOME', 'USER', 'LOGNAME', 'PATH', 'LANG', 'SHELL', 'PWD')
ENV_ALLOWED = set(ENV_KEEP) | {'DBUS_SESSION_BUS_ADDRESS', 'MOTD_SHOWN', 'SHLVL', 'SSH_CLIENT', 'SSH_CONNECTION',
                            'XDG_RUNTIME_DIR', 'XDG_SESSION_CLASS', 'XDG_SESSION_ID', 'XDG_SESSION_TYPE',
                            '_', 'INVOCATION_ID', 'JOURNAL_STREAM', 'TERM'}


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def cmd(*args):
    return subprocess.check_output(args, stderr=subprocess.DEVNULL, timeout=20).decode().strip()


def unit_info(name):
    keys = ['MainPID', 'NRestarts', 'ActiveState', 'SubState', 'UnitFileState', 'LoadState', 'FragmentPath']
    return dict(x.split('=', 1) for x in cmd('systemctl', 'show', name, *['-p'+k for k in keys]).splitlines() if '=' in x)


def process(pid):
    d = Path('/proc') / str(pid)
    env = dict(x.split(b'=', 1) for x in (d/'environ').read_bytes().split(b'\0') if b'=' in x)
    keys = {k.decode() for k in env}
    status = dict(x.split(':', 1) for x in (d/'status').read_text().splitlines() if ':' in x)
    return {'pid': int(pid), 'start': (d/'stat').read_text().rsplit(')', 1)[1].split()[19],
            'argv': (d/'cmdline').read_bytes().rstrip(b'\0').split(b'\0'),
            'exe': os.path.realpath(str(d/'exe')), 'cwd': os.path.realpath(str(d/'cwd')),
            'cgroup': (d/'cgroup').read_text(), 'unknownEnvKeys': sorted(keys-ENV_ALLOWED),
            'standardEnv': {k: env[k.encode()].decode() for k in ENV_KEEP if k.encode() in env},
            'uid': status['Uid'].split(), 'gid': status['Gid'].split(), 'umask': status['Umask'].strip(),
            'namespaces': {k: os.readlink(str(d/'ns'/k)) == os.readlink('/proc/1/ns/'+k) for k in ('mnt','net','ipc','uts','user')}}


def same_process(p):
    try:
        a = process(p['pid'])
        return a['start'] == p['start'] and a['exe'] == EXE and a['argv'] == [EXE.encode(), b'-c', CFG.encode()]
    except (FileNotFoundError, ProcessLookupError):
        return False


def port_owners():
    return sorted(set(int(x) for x in re.findall(r'pid=(\d+)', cmd('ss', '-H', '-lntup', 'sport = :11010'))))


def probe(code):
    identity = json.loads(Path('/opt/modbus-gateway/config/runtime/device_identity.json').read_text())
    assert identity['machineCode'] == code, 'Identity mismatch'
    processes = []
    for p in Path('/proc').iterdir():
        if not p.name.isdigit():
            continue
        try:
            if (p/'comm').read_text().strip() != 'easytier-core':
                continue
            info = process(int(p.name))
            info['exactArgv'] = info.pop('argv') == [EXE.encode(), b'-c', CFG.encode()]
            processes.append(info)
        except (FileNotFoundError, ProcessLookupError):
            pass
    raw = RC.read_bytes()
    business = {}
    for line in cmd('systemctl', 'list-units', '--type=service', '--state=running', '--no-legend', '--plain', '--no-pager').splitlines():
        name = line.split()[0]
        if name == NAME or not any(k in name for k in ('mqtt', 'modbus', 'compute', 'ky-ems', 'event-', 'system-monitor', 'can-driver', 'dlt645')):
            continue
        u = unit_info(name)
        business[name] = {k: u[k] for k in ('MainPID', 'NRestarts', 'ActiveState')}
    return {'code': code, 'boot': Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
            'configSha': sha(CFG), 'exeSha': sha(EXE), 'rcSha': sha(RC),
            'rcMode': RC.stat().st_mode & 0o7777, 'rcSymlink': RC.is_symlink(),
            'rcExecutable': os.access(str(RC), os.X_OK), 'rcLines': raw.splitlines().count(LINE),
            'patchedSha': hashlib.sha256(raw.replace(LINE, COMMENT, 1)).hexdigest(),
            'rcActive': unit_info('rc-local.service')['ActiveState'],
            'unit': unit_info(NAME), 'unitExists': UNIT.exists() or UNIT.is_symlink(),
            'dropins': [str(p) for p in Path('/etc/systemd/system/easytier.service.d').glob('*')],
            'linkExists': Path('/etc/systemd/system/multi-user.target.wants/easytier.service').is_symlink(),
            'processes': processes, 'ports': port_owners(), 'business': business}


def unit_bytes(cwd, env=None):
    assert re.fullmatch(r'/[A-Za-z0-9_./-]*', cwd) and Path(cwd).is_dir(), 'Unsupported working directory'
    environment = ''
    for key, value in (env or {}).items():
        assert key in ENV_KEEP and re.fullmatch(r'[A-Za-z0-9_./:=\-]*', value), 'Unsupported environment value'
        environment += 'Environment="'+key+'='+value+'"\n'
    return ('[Unit]\nDescription=EasyTier edge network\n'
            'Wants=rc-local.service\nAfter=network.target rc-local.service\nStartLimitIntervalSec=0\n'
            '[Service]\nType=simple\nUser=root\nGroup=root\nUMask=0022\n'+environment+'WorkingDirectory='+cwd+'\nExecStart='+EXE+' -c '+CFG+'\n'
            'Restart=always\nRestartSec=5\nTimeoutStopSec=20\nLimitNOFILE=1048576\n'
            '[Install]\nWantedBy=multi-user.target\n').encode()


def initial_ok(s):
    assert not s['unitExists'] and not s['dropins'] and not s['linkExists']
    assert s['unit']['LoadState'] == 'not-found'
    assert not s['rcSymlink'] and s['rcExecutable'] and s['rcActive'] == 'active' and s['rcLines'] == 1
    assert len(s['processes']) == 1
    p = s['processes'][0]
    assert p['exe'] == EXE and p['exactArgv'] and s['ports'] == [p['pid']]
    assert not p['unknownEnvKeys'] and p['uid'] == ['0']*4 and p['gid'] == ['0']*4 and p['umask'] == '0022'
    assert all(p['namespaces'].values())


def sync_dir(p):
    fd = os.open(str(p), os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def atomic(path, data, mode=0o600):
    tmp = path.with_name(path.name+'.next')
    with tmp.open('xb') as f:
        f.write(data); f.flush(); os.fchmod(f.fileno(), mode); os.fsync(f.fileno())
    os.replace(str(tmp), str(path)); sync_dir(path.parent)


def terminal(stage, status, **extra):
    atomic(stage/'result.json', json.dumps(dict(status=status, **extra)).encode())


def healthy(s, m):
    b = m['before']; u = s['unit']
    if any(s[k] != b[k] for k in ('code', 'boot', 'configSha', 'exeSha', 'business')):
        return False
    if s['rcSha'] != b['patchedSha'] or s['rcLines'] != 0 or len(s['processes']) != 1:
        return False
    p = s['processes'][0]
    return (u['ActiveState'] == 'active' and u['SubState'] == 'running' and u['NRestarts'] == '0'
            and u['UnitFileState'] == 'enabled' and str(p['pid']) == u['MainPID']
            and p['exactArgv'] and p['exe'] == EXE and p['cwd'] == b['processes'][0]['cwd']
            and all(p['standardEnv'].get(k) == v for k,v in b['processes'][0]['standardEnv'].items())
            and p['uid'] == ['0']*4 and p['gid'] == ['0']*4 and p['umask'] == '0022' and all(p['namespaces'].values())
            and '/easytier.service' in p['cgroup'] and s['ports'] == [p['pid']])


def runner(stage):
    m = json.loads((stage/'metadata.json').read_text()); b = m['before']
    current = probe(b['code']); initial_ok(current)
    for key in ('code', 'boot', 'configSha', 'exeSha', 'rcSha', 'rcMode', 'business', 'processes'):
        assert current[key] == b[key], 'Preflight drift: '+key
    original = RC.read_bytes(); updated = original.replace(LINE, COMMENT, 1)
    subprocess.run(['/bin/bash', '-n'], input=updated, check=True, timeout=10)
    p = b['processes'][0]; data = unit_bytes(p['cwd'], p['standardEnv'])
    atomic(stage/'rc.local.before', original, b['rcMode'])
    atomic(stage/'unit.candidate', data)
    assert not RC.with_name(RC.name+'.next').exists() and not UNIT.with_name(UNIT.name+'.next').exists()
    touched_rc = touched_unit = False
    deadline = time.monotonic()+180
    try:
        touched_rc = True; atomic(RC, updated, b['rcMode'])
        touched_unit = True; atomic(UNIT, data, 0o644)
        cmd('systemctl', 'daemon-reload')
        assert same_process(p) and port_owners() == [p['pid']]
        os.kill(p['pid'], signal.SIGTERM)
        for _ in range(100):
            if not same_process(p): break
            time.sleep(0.1)
        assert not same_process(p) and not port_owners(), 'Old process or unknown listener remains'
        cmd('systemctl', 'enable', NAME)
        cmd('systemctl', 'start', NAME)
        while time.monotonic() < deadline:
            if (stage/'ack.json').exists():
                a = json.loads((stage/'ack.json').read_text()); s = probe(b['code'])
                assert a['nonce'] == m['nonce'] and healthy(s, m) and a['pid'] == s['processes'][0]['pid']
                assert UNIT.read_bytes() == data
                terminal(stage, 'COMMITTED', pid=a['pid']); return
            time.sleep(2)
        raise TimeoutError('No fresh SSH acknowledgment')
    except BaseException as error:
        try:
            if touched_unit and UNIT.exists():
                assert UNIT.read_bytes() == data, 'Concurrent unit change'
                cmd('systemctl', 'daemon-reload')
                cmd('systemctl', 'stop', NAME)
                assert unit_info(NAME)['MainPID'] == '0'
                cmd('systemctl', 'disable', NAME)
                UNIT.unlink(); sync_dir(UNIT.parent)
            if touched_rc:
                assert RC.read_bytes() in (original, updated), 'Concurrent rc.local change'
                if RC.read_bytes() == updated: atomic(RC, original, b['rcMode'])
            cmd('systemctl', 'daemon-reload')
            if not same_process(p):
                assert not port_owners(), 'Unknown listener; no duplicate recovery start'
                with (stage/'recovery.log').open('ab') as f:
                    subprocess.Popen([EXE, '-c', CFG], cwd=p['cwd'], stdin=subprocess.DEVNULL,
                                     stdout=f, stderr=f, start_new_session=True, env=dict(os.environ, **p['standardEnv']))
            terminal(stage, 'ROLLED_BACK', reason=type(error).__name__)
        except BaseException as e:
            terminal(stage, 'ROLLBACK_FAILED', reason=type(e).__name__)


def local(host, code, approved):
    import paramiko
    import secrets
    source = Path(__file__).read_bytes()
    assert approved == hashlib.sha256(source).hexdigest(), 'Unreviewed source'
    nonce = secrets.token_hex(12)
    stage = '/opt/modbus-gateway/backups/easytier-single-'+nonce
    out = Path('artifacts/easytier-fleet-20260912') / (code+'-'+nonce)
    out.mkdir(parents=True)

    def connect():
        c = paramiko.SSHClient(); c.load_system_host_keys()
        try:
            c.connect(host, username='root', password=os.environ['EDGE_PASSWORD'], timeout=6,
                      auth_timeout=6, banner_timeout=6, look_for_keys=False, allow_agent=False)
            return c
        except BaseException:
            c.close(); raise

    def remote(c, command):
        _, o, e = c.exec_command(command, timeout=30)
        data = o.read(); error = e.read()
        assert o.channel.recv_exit_status() == 0, 'Remote read/preflight failed'
        return data

    def read(c):
        return json.loads(remote(c, 'python3 '+shlex.quote(stage+'/runner.py')+' --probe '+shlex.quote(code)))

    def upload(c, name, data):
        with c.open_sftp() as s:
            s.get_channel().settimeout(15)
            with s.open(name+'.upload', 'wx') as f: f.write(data)
            s.chmod(name+'.upload', 0o600); s.rename(name+'.upload', name)

    def save(name, value):
        (out/name).write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding='utf-8')

    c = connect()
    try:
        identity = json.loads(remote(c, 'cat /opt/modbus-gateway/config/runtime/device_identity.json'))
        assert identity['machineCode'] == code
        remote(c, 'mkdir -p -m 700 '+shlex.quote(stage))
        upload(c, stage+'/runner.py', source)
        assert remote(c, 'sha256sum '+shlex.quote(stage+'/runner.py')).decode().split()[0] == approved
        b = read(c); initial_ok(b); save('before.json', b)
        m = {'nonce': nonce, 'before': b}
        upload(c, stage+'/metadata.json', json.dumps(m).encode())
        save('state.json', {'stage': stage, 'host': host, 'code': code, 'sourceSha': approved, 'launchAttempted': True})
        started = time.monotonic()
        try:
            remote(c, 'nohup setsid python3 '+shlex.quote(stage+'/runner.py')+' --run '+shlex.quote(stage)+
                   ' </dev/null >'+shlex.quote(stage+'/runner.log')+' 2>&1 &')
        except Exception: pass
    finally: c.close()
    previous = None; observations = []; acknowledged = False
    while time.monotonic()-started < 90:
        c = None
        try:
            c = connect(); s = read(c); observations.append(s)
            if healthy(s, m):
                pid = s['processes'][0]['pid']
                if previous and previous[0] == pid and time.monotonic()-previous[1] >= 5:
                    if time.monotonic()-started >= 90: break
                    upload(c, stage+'/ack.json', json.dumps({'nonce': nonce, 'pid': pid}).encode())
                    acknowledged = True; break
                previous = (pid, time.monotonic())
            else: previous = None
        except Exception: previous = None
        finally:
            if c: c.close()
        time.sleep(5)
    save('observations.json', observations)
    while time.monotonic()-started < 300:
        c = None
        try:
            c = connect()
            result = json.loads(remote(c, 'cat '+shlex.quote(stage+'/result.json')))
            s = read(c); save('after.json', s)
            result.update(acknowledged=acknowledged, verified=healthy(s, m), stage=stage)
            save('result.json', result); print(code, json.dumps(result), flush=True)
            return 0 if result['status'] == 'COMMITTED' and result['verified'] and acknowledged else 1
        except Exception: pass
        finally:
            if c: c.close()
        time.sleep(5)
    save('result.json', {'status': 'UNKNOWN_DO_NOT_REAPPLY', 'stage': stage})
    return 2


if __name__ == '__main__':
    if sys.argv[1] == '--probe':
        print(json.dumps(probe(sys.argv[2])))
    elif sys.argv[1] == '--run':
        runner(Path(sys.argv[2]))
    elif sys.argv[1] == '--apply':
        sys.exit(local(sys.argv[2], sys.argv[3], sys.argv[4]))
    else:
        raise SystemExit('Explicit --probe, --run or --apply required')
