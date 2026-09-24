"""Read sealed local ARM files only; never build or connect to a device."""
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
MANIFEST = Path('/mnt/d/workspace/GatewaySuite-workspaces/realese1.0/coordinator/evidence/GW-20260809-002/ems-cluster-20260924/stage16-recovery-arm/program-manifest.json')
RAW = ROOT.parent.parent / 'acceptance-evidence-20260924/evidence/raw/GW-20260809-002'
archives = [RAW / 'helper-r4-20260924T102700Z/r3-candidate-cd6b529.recovered.tar.gz',
            RAW / 'arm-r5-20260924/r5-reelection-9ccfce8-delta.tar.gz']
manifest = json.loads(MANIFEST.read_text())
assert hashlib.sha256(MANIFEST.read_bytes()).hexdigest() == '75c7018d4d78d93e67f8239c041b5f1003d460f0266f549e0c25053cc9795ca6'
selected = {c['target']: c for c in manifest['components'] if c['target'] in
            ('EmsClusterCoordinator', 'ComputeEngine', 'MqttDriver', 'MqttForwarder')}
rows = {}
with tempfile.TemporaryDirectory(prefix='arm-link-scope-') as directory:
    for path in archives:
        with tarfile.open(path) as archive:
            names = set(archive.getnames())
            for target, component in selected.items():
                if component['archivePath'] not in names:
                    continue
                raw = archive.extractfile(component['archivePath']).read()
                if hashlib.sha256(raw).hexdigest() != component['sha256']:
                    continue
                binary = Path(directory) / target
                binary.write_bytes(raw)
                headers = subprocess.check_output(['readelf', '-h', str(binary)], text=True)
                symbols = subprocess.check_output(['readelf', '-Ws', '--wide', str(binary)], text=True)
                decoded = subprocess.run(['c++filt'], input=symbols, text=True,
                                         stdout=subprocess.PIPE, check=True).stdout
                rows[target] = {'sha256': component['sha256'],
                    'aarch64': 'AArch64' in headers,
                    'hasSymtab': "Symbol table '.symtab'" in symbols,
                    'relevantSymbols': [line.strip() for line in decoded.splitlines() if any(
                        token in line for token in ('EmsClusterPointBridge::', 'LoadSampler::',
                                                    'emsClusterPointRoutes(', 'addEmsClusterPointRoutes('))]}
assert set(rows) == set(selected)
out = HERE / 'arm-link-scope.json'
assert not out.exists()
out.write_text(json.dumps({'remoteConnections': 0, 'builds': 0,
    'sourceScope': 'P1 sampleCapability and P2 LoadSampler only; routing methods unchanged',
    'files': rows}, indent=2) + '\n')
for target, row in rows.items():
    print(target, 'symtab=', row['hasSymtab'], 'bridge=', any('EmsClusterPointBridge::' in s for s in row['relevantSymbols']),
          'load=', any('LoadSampler::' in s for s in row['relevantSymbols']))
