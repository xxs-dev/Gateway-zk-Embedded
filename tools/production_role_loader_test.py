"""Independent guard process must execute pinned source bytes, never a pyc."""

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import py_compile
import struct
import subprocess
import sys
import tempfile
import unittest


GUARD = Path(__file__).resolve().parents[1] / 'deploy/runtime-upgrade-guard.py'
PROTECTED = {'machineCode': 'SIM', 'identitySha256': 'a' * 64,
             'otaPublicKeySha256': 'b' * 64}


@unittest.skipUnless(hasattr(os, 'geteuid') and os.geteuid() == 0,
                     'root-owned private policy fixture required')
class ProductionRoleLoaderTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='production-role-loader-')
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name)
        self.helper = self.home / 'bin/production_role_policy.py'
        self.helper.parent.mkdir()
        self.profile = {'format': 'board-simulator-profile-v1', 'machineCode': 'SIM',
                        'ramMiB': 2048, 'capabilities': [], 'simulatorBindings': {},
                        'allowedUnits': [], 'segmentNames': []}
        raw = (json.dumps(self.profile, sort_keys=True) + '\n').encode()
        digest = hashlib.sha256(raw).hexdigest()
        path = self.home / 'data/runtime-production/profiles' / (digest + '.json')
        path.parent.mkdir(parents=True)
        path.write_bytes(raw)
        path.chmod(0o600)
        self.profile_sha = digest

    def source(self, marker):
        return ("def resolve(*args, **kwargs):\n"
                "    return {'marker': '" + marker + "', 'protected': " +
                repr(PROTECTED) + ", 'roles': []}\n").encode()

    def child(self):
        command = '''import hashlib, json, pathlib, runpy, sys
assert sys.dont_write_bytecode is False
assert sys.pycache_prefix is None
guard = runpy.run_path(sys.argv[1])
home = pathlib.Path(sys.argv[2])
helper = home / 'bin/production_role_policy.py'
raw = helper.read_bytes()
expected = {'marker': 'NEW!', 'protected': json.loads(sys.argv[3]), 'roles': []}
baseline = {'format': 'runtime-baseline-v2', 'machineCode': 'SIM',
            'configSha256': {'device_identity.json': 'a' * 64},
            'trust': {'publicKeySha256': 'b' * 64}, 'allowedUnits': [],
            'segments': [], 'units': [], 'unitFiles': {},
            'files': {'bin/production_role_policy.py':
                {'sha256': hashlib.sha256(raw).hexdigest(), 'bytes': len(raw), 'mode': 0o755}},
            'roleAuthority': {'profileSha256': sys.argv[4], 'resolved': expected}}
inventory = guard['production_files'](home)
for _ in range(2):
    assert guard['production_roles'](home, baseline) == []
    assert guard['production_files'](home) == inventory
print('PINNED_SOURCE_OK')'''
        return self.run_child(command, str(GUARD), str(self.home),
                              json.dumps(PROTECTED), self.profile_sha)

    def run_child(self, command, *args):
        env = os.environ.copy()
        env.pop('PYTHONDONTWRITEBYTECODE', None)
        env.pop('PYTHONPYCACHEPREFIX', None)
        env.pop('SOURCE_DATE_EPOCH', None)
        return subprocess.run([sys.executable, '-c', command, *args], env=env,
                              capture_output=True, text=True, timeout=20)

    def test_clean_guard_creates_no_cache_and_repeats_without_inventory_drift(self):
        self.helper.write_bytes(self.source('NEW!'))
        self.helper.chmod(0o755)
        control = self.home / 'old-loader-control.py'
        control.write_bytes(self.source('NEW!'))
        negative = self.run_child('''import importlib.util, pathlib, sys
assert sys.dont_write_bytecode is False and sys.pycache_prefix is None
path = pathlib.Path(sys.argv[1])
spec = importlib.util.spec_from_file_location('old_loader_control', path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
assert module.resolve()['marker'] == 'NEW!'
assert pathlib.Path(importlib.util.cache_from_source(str(path))).is_file()
''', str(control))
        self.assertEqual(negative.returncode, 0, negative.stderr)
        before = {str(p.relative_to(self.home)) for p in self.home.rglob('*') if p.is_file()}
        result = self.child()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('PINNED_SOURCE_OK', result.stdout)
        after = {str(p.relative_to(self.home)) for p in self.home.rglob('*') if p.is_file()}
        self.assertEqual(before, after)
        self.assertFalse((self.helper.parent / '__pycache__').exists())

    def test_stale_same_mtime_size_bytecode_is_never_executed(self):
        self.helper.write_bytes(self.source('OLD!'))
        self.helper.chmod(0o755)
        stamp = self.helper.stat().st_mtime_ns
        pyc = Path(py_compile.compile(str(self.helper), doraise=True,
                   invalidation_mode=py_compile.PycInvalidationMode.TIMESTAMP))
        self.assertEqual(len(self.source('OLD!')), len(self.source('NEW!')))
        self.helper.write_bytes(self.source('NEW!'))
        os.utime(self.helper, ns=(stamp, stamp))
        header = pyc.read_bytes()[:16]
        self.assertEqual(header[:4], importlib.util.MAGIC_NUMBER)
        self.assertEqual(struct.unpack_from('<I', header, 4)[0], 0)
        self.assertEqual(struct.unpack_from('<II', header, 8),
                         (int(self.helper.stat().st_mtime) & 0xffffffff,
                          len(self.source('NEW!'))))
        negative = self.run_child('''import importlib.util, pathlib, sys
assert sys.dont_write_bytecode is False and sys.pycache_prefix is None
path = pathlib.Path(sys.argv[1])
spec = importlib.util.spec_from_file_location('old_loader_control', path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
assert module.resolve()['marker'] == 'OLD!'
''', str(self.helper))
        self.assertEqual(negative.returncode, 0, negative.stderr)
        before = {str(p.relative_to(self.home)) for p in self.home.rglob('*') if p.is_file()}
        result = self.child()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('PINNED_SOURCE_OK', result.stdout)
        self.assertEqual(before, {str(p.relative_to(self.home)) for p in self.home.rglob('*') if p.is_file()})


if __name__ == '__main__':
    unittest.main()
