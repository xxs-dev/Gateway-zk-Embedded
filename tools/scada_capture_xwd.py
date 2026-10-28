"""Capture current COMM104 X11 root window without navigation or diagnostics API."""
import gzip
import hashlib
import json
import os
from pathlib import Path
import shlex
import sys
import time
import paramiko

output = Path(sys.argv[1])
output.mkdir(parents=True, exist_ok=False)
client = paramiko.SSHClient()
client.load_system_host_keys()
client.connect('10.126.126.11', username='root', password=os.environ['EDGE_PASSWORD'], timeout=10)

def run(command):
    _, stdout, stderr = client.exec_command(command, timeout=60)
    data, error = stdout.read(), stderr.read()
    if stdout.channel.recv_exit_status():
        raise RuntimeError(error.decode(errors='replace'))
    return data

try:
    with client.open_sftp() as sftp:
        with sftp.open('/opt/modbus-gateway/config/runtime/device_identity.json', 'rb') as stream:
            identity = json.loads(stream.read())
        if identity['machineCode'] != 'COMM202600104':
            raise ValueError('Wrong device')
        release = run('readlink -f /opt/modbus-gateway/scada/current').decode().strip()
        if release != '/opt/modbus-gateway/scada/releases/comm104-local-controls-20260912-103919':
            raise ValueError('Wrong release')
        remote = '/tmp/comm104-xwd-after-' + time.strftime('%Y%m%d-%H%M%S') + '.xwd'
        timestamp = run('date -Is').decode().strip()
        run('env DISPLAY=:0 XAUTHORITY=/run/user/1000/gdm/Xauthority timeout 20 xwd -root -silent -out ' + shlex.quote(remote))
        raw_hash = run('sha256sum ' + shlex.quote(remote)).decode().split()[0]
        run('gzip -n ' + shlex.quote(remote))
        digest = run('sha256sum ' + shlex.quote(remote + '.gz')).decode().split()[0]
        sftp.get(remote + '.gz', str(output / 'foreground.xwd.gz'))
        data = (output / 'foreground.xwd.gz').read_bytes()
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError('Transfer hash mismatch')
        raw = gzip.decompress(data)
        if hashlib.sha256(raw).hexdigest() != raw_hash:
            raise ValueError('XWD hash mismatch')
        (output / 'foreground.xwd').write_bytes(raw)
        (output / 'xwd-report.json').write_text(json.dumps(dict(machineCode=identity['machineCode'],
            release=release, capturedAt=timestamp, kind='actual-X11-root-window', navigationPerformed=False,
            gzipSha256=digest, xwdSha256=raw_hash, remote=remote + '.gz'), indent=2), encoding='utf-8')
        print('XWD transferred and verified: ' + str(output))
finally:
    client.close()
