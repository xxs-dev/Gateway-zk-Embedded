"""Missing recording identity must fail before storage or relay communication.

Run only in an isolated mount/net/ipc namespace with private runtime paths.
Arguments are the executable command, optionally prefixed by QEMU and its loader.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

for ns in ('mnt', 'net', 'ipc'):
    assert os.readlink('/proc/self/ns/' + ns) != os.readlink('/proc/1/ns/' + ns)
assert len(sys.argv) > 1
with tempfile.TemporaryDirectory(prefix='soe-identity-') as folder:
    root = Path(folder)
    app = root / 'app.json'
    app.write_text(json.dumps({'identityConfigFile': ''}))
    for missing in ('machineCode', 'meterCode'):
        db = root / (missing + '.db')
        config = {
            'enabled': True, 'machineCode': 'ISOLATED_TEST', 'meterCode': 'AM5_TEST',
            'protocol': {'type': 'iec103',
                         'tcp': {'host': '127.0.0.1', 'port': 1, 'connectTimeoutMs': 1},
                         'iec': {'transportMode': 'am5se_active_tcp',
                                 'soe': {'enabled': True, 'sqlitePath': str(db),
                                         'recordings': {'enabled': True}}}},
            'meters': [], 'points': []}
        del config[missing]
        path = root / 'device.json'
        path.write_text(json.dumps(config))
        result = subprocess.run(sys.argv[1:] + ['--once', '--config', str(path),
                                                '--app-config', str(app)],
                                cwd=folder, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=10)
        assert result.returncode != 0, result.stdout
        assert b'SOE recording association requires machineCode and meterCode' in result.stdout, result.stdout
        assert not db.exists(), 'identity rejected only after creating the database'
print('SOE recording identity rejected before storage/communication: both cases passed')
