"""Device-side, bounded EasyTier handover; consumes reviewed stage metadata."""
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time

STAGE = Path(__file__).resolve().parent
ROOT = Path('/etc')
RC = ROOT / 'rc.local'
DROP = ROOT / 'systemd/system/easytier.service.d/90-ems-single-owner.conf'
EXE = '/usr/local/bin/easytier-core'
CONFIG = '/etc/easytier/et.conf'
UNIT = 'easytier.service'
LINE = b'    /usr/local/bin/easytier-core -c /etc/easytier/et.conf &'
REPLACEMENT = b'    # EasyTier is started only by easytier.service.'
DROP_BYTES = b'[Unit]\nAfter=rc-local.service\n'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(*args):
    return subprocess.check_output(args, timeout=35).decode().strip()


def process_matches(pid, start):
    try:
        p = Path('/proc') / str(pid)
        fields = (p / 'stat').read_text().rsplit(')', 1)[1].split()
        command = (p / 'cmdline').read_bytes().rstrip(b'\0').split(b'\0')
        return (fields[19] == str(start) and
                os.path.realpath(str(p / 'exe')) == EXE and
                command == [EXE.encode(), b'-c', CONFIG.encode()])
    except FileNotFoundError:
        return False


def replace(path, data, mode):
    tmp = path.with_name(path.name + '.ems-next')
    with tmp.open('xb') as f:
        f.write(data)
        f.flush()
        os.fchmod(f.fileno(), mode)
        os.fsync(f.fileno())
    os.replace(str(tmp), str(path))
    fd = os.open(str(path.parent), os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def migrate_rc(data):
    if data.splitlines().count(LINE) != 1:
        raise ValueError('Unexpected rc.local entry')
    return data.replace(LINE, REPLACEMENT, 1)


def apply():
    m = json.loads((STAGE / 'metadata.json').read_text())
    identity = json.loads(Path('/opt/modbus-gateway/config/runtime/device_identity.json').read_text())
    assert identity['machineCode'] == 'COMM202600104'
    assert Path('/proc/sys/kernel/random/boot_id').read_text().strip() == m['bootId']
    assert sha(RC) == m['rcSha'] and sha(CONFIG) == m['configSha'] and sha(EXE) == m['exeSha']
    assert sha('/etc/systemd/system/easytier.service') == m['unitSha']
    assert not RC.is_symlink() and not DROP.exists()
    assert os.access(str(RC), os.X_OK)
    assert run('systemctl', 'show', 'rc-local.service', '-p', 'ActiveState', '--value') == 'active'
    assert run('systemctl', 'is-enabled', UNIT) == 'enabled'
    assert process_matches(m['pid'], m['startTicks'])
    original = RC.read_bytes()
    updated = migrate_rc(original)
    subprocess.run(['/bin/bash', '-n'], input=updated, check=True, timeout=10)
    assert not RC.with_name(RC.name + '.ems-next').exists()
    assert not DROP.with_name(DROP.name + '.ems-next').exists()
    mode = RC.stat().st_mode & 0o7777
    shutil.copy2(str(RC), str(STAGE / 'rc.local.before'))
    with (STAGE / 'rc.local.before').open('rb') as f:
        os.fsync(f.fileno())
    changed_rc = False
    changed_drop = False
    deadline = time.monotonic() + 180
    try:
        run('systemctl', 'stop', UNIT)
        run('systemctl', 'reset-failed', UNIT)
        assert run('systemctl', 'show', UNIT, '-p', 'MainPID', '--value') == '0'
        assert run('systemctl', 'show', UNIT, '-p', 'ActiveState', '--value') == 'inactive'
        assert process_matches(m['pid'], m['startTicks'])
        assert sha(RC) == m['rcSha'] and sha(CONFIG) == m['configSha']
        changed_rc = True
        replace(RC, updated, mode)
        DROP.parent.mkdir(exist_ok=True)
        changed_drop = True
        replace(DROP, DROP_BYTES, 0o644)
        run('systemctl', 'daemon-reload')
        assert process_matches(m['pid'], m['startTicks'])
        listeners = run('ss', '-H', '-lntp', 'sport = :11010')
        assert set(re.findall(r'pid=(\d+)', listeners)) == {str(m['pid'])}
        os.kill(m['pid'], signal.SIGTERM)
        for _ in range(100):
            if not process_matches(m['pid'], m['startTicks']):
                break
            time.sleep(0.1)
        assert not process_matches(m['pid'], m['startTicks']), 'Old process did not exit'
        run('systemctl', 'start', UNIT)
        print('WAITING_FOR_FRESH_SSH_ACK', flush=True)
        while time.monotonic() < deadline:
            ack = STAGE / 'ack.json'
            if ack.exists():
                a = json.loads(ack.read_text())
                assert a['nonce'] == m['nonce']
                pid = int(run('systemctl', 'show', UNIT, '-p', 'MainPID', '--value'))
                assert a['pid'] == pid and pid > 0
                assert run('systemctl', 'is-active', UNIT) == 'active'
                assert sha(CONFIG) == m['configSha'] and sha(RC) == hashlib.sha256(updated).hexdigest()
                (STAGE / 'result.json').write_text(json.dumps({'status': 'COMMITTED', 'pid': pid}))
                print('COMMITTED', flush=True)
                return
            time.sleep(2)
        raise TimeoutError('Fresh SSH acknowledgment not received')
    except BaseException as error:
        print('ROLLBACK_REASON_' + type(error).__name__, flush=True)
        try:
            run('systemctl', 'stop', UNIT)
            if changed_rc:
                current = RC.read_bytes()
                assert current in (original, updated), 'Concurrent rc.local edit; manual recovery required'
                if current == updated:
                    replace(RC, original, mode)
            if changed_drop and DROP.exists():
                assert DROP.read_bytes() == DROP_BYTES
                DROP.unlink()
            run('systemctl', 'daemon-reload')
            if not process_matches(m['pid'], m['startTicks']):
                with (STAGE / 'legacy-recovery.log').open('ab') as log:
                    subprocess.Popen([EXE, '-c', CONFIG], stdin=subprocess.DEVNULL,
                                     stdout=log, stderr=log, start_new_session=True)
            # Keep the conflicting unit stopped; its pre-existing enabled state is unchanged.
            (STAGE / 'result.json').write_text(json.dumps({'status': 'ROLLED_BACK', 'reason': type(error).__name__}))
        except BaseException as rollback_error:
            (STAGE / 'result.json').write_text(json.dumps({'status': 'ROLLBACK_FAILED', 'reason': type(rollback_error).__name__}))
        raise


if __name__ == '__main__':
    apply()
