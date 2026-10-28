"""Upload an explicit JSON-renderer source snapshot to 22.11, build and verify download."""
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tarfile
import time
import paramiko

root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=False)
source = out / 'source'
source.mkdir()
names = ['tools/scada_capture_readonly.cpp', 'tools/build_scada_capture_json_aarch64.sh',
         'src/scada_project_loader.cpp']
names += [p.name for p in root.glob('local_display_qt_*.hpp')]
names += ['local_display_qt_' + name + '.cpp' for name in
          ['scada_scene', 'access_control', 'pcs_power_control', 'value_map']]
names += [p.relative_to(root).as_posix() for p in (root / 'include').rglob('*.hpp')]
hashes = {}
for name in sorted(set(names)):
    data = (root / name).read_bytes()
    path = source / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    hashes[name] = hashlib.sha256(data).hexdigest()
(source / 'source.sha256').write_bytes(''.join(h + '  ' + n + '\n' for n, h in hashes.items()).encode('ascii'))
for args, name in [(['status', '--short', '--branch'], 'local-status.txt'), (['rev-parse', 'HEAD'], 'local-head.txt')]:
    (out / name).write_bytes(subprocess.check_output(['git', '-C', str(root), *args]))
with tarfile.open(out / 'source.tar.gz', 'w:gz') as archive:
    archive.add(source, arcname='source')
remote = '/tmp/scada-json-build-' + time.strftime('%Y%m%d-%H%M%S')
client = paramiko.SSHClient()
client.load_system_host_keys()
client.connect('192.168.22.11', username='root', password=os.environ['GRID_BUILD_PASSWORD'], timeout=10)

def run(command):
    _, stdout, stderr = client.exec_command(command, timeout=240)
    result, error = stdout.read(), stderr.read()
    if stdout.channel.recv_exit_status():
        raise RuntimeError(error.decode(errors='replace'))
    return result

try:
    (out / 'remote-status.txt').write_bytes(run('sudo -H -u tronlong git -C /srv/build/Gateway-zk status --short --branch'))
    (out / 'remote-head.txt').write_bytes(run('sudo -H -u tronlong git -C /srv/build/Gateway-zk rev-parse HEAD'))
    run('mkdir -m 755 ' + shlex.quote(remote))
    with client.open_sftp() as sftp:
        sftp.put(str(out / 'source.tar.gz'), remote + '/source.tar.gz')
        run('tar -xzf ' + remote + '/source.tar.gz -C ' + remote)
        (out / 'source-verify.txt').write_bytes(run('cd ' + remote + '/source && sha256sum -c source.sha256'))
        run('chown -R tronlong:tronlong ' + remote)
        log = run('sudo -H -u tronlong bash ' + remote + '/source/tools/build_scada_capture_json_aarch64.sh ' + remote + '/out')
        (out / 'build.log').write_bytes(log)
        for name in ['scada_capture_json', 'symbols.txt', 'elf-header.txt', 'binary.sha256']:
            sftp.get(remote + '/out/' + name, str(out / name))
        digest = hashlib.sha256((out / 'scada_capture_json').read_bytes()).hexdigest()
        if run('sha256sum ' + remote + '/out/scada_capture_json').decode().split()[0] != digest:
            raise RuntimeError('Downloaded binary hash mismatch')
        (out / 'build-report.json').write_text(json.dumps(dict(remote=remote, binarySha256=digest,
            sourceHashes=hashes, pointStoreLinked=False), indent=2), encoding='utf-8')
        print('AArch64 download verified SHA256 ' + digest)
finally:
    client.close()
