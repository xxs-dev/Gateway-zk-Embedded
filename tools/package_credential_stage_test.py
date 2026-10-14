#!/usr/bin/env python3
"""Exercise actual builders in nonroot private trees, without running payloads."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1]
CREDENTIALS = frozenset(('password', 'secretkey', 'accesskey', 'token', 'apikey',
                        'enrollmenttoken', 'privatekey', 'clientsecret'))


def cleared(value):
    if isinstance(value, dict):
        return {key: '' if key.lower() in CREDENTIALS else cleared(item)
                for key, item in value.items()}
    if isinstance(value, list):
        return [cleared(item) for item in value]
    return value


class PackageCredentialTest(unittest.TestCase):
    def setUp(self):
        self.assertNotEqual(0, os.geteuid(), 'nonroot test only')
        self.temp = tempfile.TemporaryDirectory(prefix='package-credential-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / 'source'
        shutil.copytree(SOURCE / 'deploy', self.source / 'deploy',
                        ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
        (self.source / 'build-aarch64').mkdir()
        for name in ('SystemMonitor', 'MqttDriver', 'MqttForwarder', 'pointctl', 'AgcAvcController'):
            (self.source / 'build-aarch64' / name).write_bytes(b'synthetic, not executable\n')
        self.config = {
            'runtimeMode': 'gateway', 'deviceConfigFiles': [],
            'mqtt': {'username': 'operator', 'password': 'synthetic-only', 'tls': {'enabled': True}},
            'ota': {'enabled': True, 'requireSignature': True, 'checksumRequired': True,
                    'querySigningPrivateKeyPath': '/etc/modbus-gateway/query.key',
                    'storage': {'minio': {'accessKey': 'synthetic-only', 'secretKey': 'synthetic-only'}}},
            'cameraService': {'enabled': True, 'media': {'auth': {'enabled': True, 'password': 'synthetic-only'}},
                              'cameras': [{'sourceAuth': {'password': 'synthetic-only'}}]},
            'nested': [{'clientSecret': 'synthetic-only', 'apiKey': 'synthetic-only', 'enabled': True}],
        }
        self.paths = []
        for name in ('factory/runtime/apps/monitor-service.json', 'factory/runtime/apps/mqtt-service.json',
                     'factory/runtime/apps/agc-avc-service.json',
                     'factory/runtime/devices/device_agc_avc_virtual.json',
                     'examples/camera-service-public-example.json'):
            path = self.source / 'config' / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(json.dumps(self.config), encoding='utf-8')
            self.paths.append(path)
        widgets = self.source / 'config/generic-scada-widgets.json'
        shutil.copyfile(SOURCE / 'config/generic-scada-widgets.json', widgets)
        self.paths.append(widgets)

    def build(self, kind):
        out = self.root / (kind + '.tar.gz')
        argv = ['sh', str(self.source / 'deploy' / ('build-' + kind + '-package.sh')), str(out)]
        if kind == 'factory':
            argv += ['--profile', 'base']
        result = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30,
                                env=dict(os.environ, TMPDIR=str(self.root)))
        return out, result

    def check_clean_package(self, kind):
        before = {str(path): path.read_bytes() for path in self.paths}
        out, result = self.build(kind)
        self.assertEqual(0, result.returncode, 'builder failed; credential-bearing output omitted')
        with tarfile.open(out) as archive:
            count = 0
            for member in archive:
                if member.name == 'gateway-factory-defaults/config/generic-scada-widgets.json':
                    self.assertEqual((SOURCE / 'config/generic-scada-widgets.json').read_bytes(),
                                     archive.extractfile(member).read())
                    continue
                if member.isfile() and member.name.startswith('gateway-factory-defaults/config/') and member.name.endswith('.json'):
                    data = json.loads(archive.extractfile(member).read())
                    self.assertEqual(cleared(self.config), data,
                                     'staged credential/configuration contract mismatch (values omitted)')
                    count += 1
            self.assertEqual(3 if kind == 'factory' else 2, count)
        self.assertEqual(before, {str(path): path.read_bytes() for path in self.paths},
                         'source configuration changed (values omitted)')

    def test_factory_clears_values_preserves_security_and_source(self):
        self.check_clean_package('factory')

    def test_agc_clears_values_preserves_security_and_source(self):
        self.check_clean_package('agc-avc-runtime')

    def check_malformed_rejected(self, kind):
        self.paths[2 if kind == 'agc-avc-runtime' else 0].write_text('{invalid', encoding='ascii')
        out, result = self.build(kind)
        self.assertNotEqual(0, result.returncode, 'malformed staged config must reject packaging')
        self.assertFalse(out.exists())
        self.assertIn(b'PACKAGE_CONFIG_INVALID', result.stderr)

    def test_factory_malformed_config_rejected(self):
        self.check_malformed_rejected('factory')

    def test_agc_malformed_config_rejected(self):
        self.check_malformed_rejected('agc-avc-runtime')


if __name__ == '__main__':
    unittest.main()
