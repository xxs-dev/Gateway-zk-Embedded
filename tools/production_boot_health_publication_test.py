"""AST-isolated boot health retry on private files, not Linux/native/systemd proof."""
import ast
import hashlib
import json
from pathlib import Path
import tempfile
import types
import unittest

SOURCE = Path(__file__).resolve().parents[1] / 'deploy/offline-runtime-upgrade.py'


class BootHealthPublicationTest(unittest.TestCase):
    def test_published_attempt_survives_sync_error_retry_proves_new_pid(self):
        names = {'production_boot_begin_start', 'production_boot_start_current', 'production_boot_record_health',
                 'production_boot_repair_gates', 'production_inhibit'}
        tree = ast.parse(SOURCE.read_bytes())
        functions = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in names]
        self.assertEqual(len(functions), len(names))
        with tempfile.TemporaryDirectory(prefix='production-boot-health-') as directory:
            root = Path(directory)
            work_path = root / 'BOOT-resume.json'
            gate = root / 'owned.conf'
            gate.write_bytes(b'approved owned gate')
            work = dict(format='runtime-boot-resume-v1', bootId='BOOT', transaction='TX', phase='PREPARED')
            work_path.write_text(json.dumps(work))
            baseline = dict(units=['gateway-services.service'], allowedUnits=['gateway-services.service'],
                            segments=[], retiredSources={}, gates={'gateway-services.service': str(gate)},
                            unitFiles={str(gate): dict(sha256=hashlib.sha256(gate.read_bytes()).hexdigest(), mode=0o644, bytes=gate.stat().st_size)})
            running, pid, fail_sync = [False], [100], [True]
            def require(condition, message):
                if not condition: raise ValueError(message)
            def read(path): return json.loads(path.read_bytes())
            def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()
            def save(path, value):
                temporary = path.with_name(path.name + '.private-tmp')
                temporary.write_bytes((json.dumps(value, sort_keys=True) + '\n').encode())
                temporary.replace(path)
            def publish(path, raw):
                require(not path.exists(), 'unexpected final')
                temporary = path.with_name(path.name + '.private-tmp')
                temporary.write_bytes(raw); temporary.rename(path)
                if '-health-' in path.name and fail_sync[0]:
                    fail_sync[0] = False
                    raise OSError('injected directory fsync after publication')
            def systemctl(*args):
                if args[0] == 'start':
                    pid[0] += 1; running[0] = True
                elif args[0] == 'is-active': require(running[0], 'not started')
                else: self.assertEqual(args[0], 'daemon-reload')
            def pin(path):
                return dict(sha256=digest(path), mode=0o644, bytes=path.stat().st_size)
            def verify(home, document, units=True):
                if units: require(gate.read_bytes() == b'approved owned gate', 'fenced')
            class Runtime:
                home = root
                def stop(self, document): running[0] = False
                def convert_gates(self, transaction, document): gate.write_bytes(b'approved owned gate')
                def health(self, document):
                    require(running[0], 'actual substituted service was not started')
                    return dict(bootId='BOOT', readers={'gateway-services.service': dict(pid=str(pid[0]))})
            guard = types.SimpleNamespace(PRODUCTION_READERS={}, production_verify=verify,
                    production_unit=lambda home, unit: require(gate.read_bytes() == b'approved owned gate', 'fenced'),
                    production_pin=pin, production_json=read,
                    production_units_present=lambda units: require(units == baseline['units'], 'unknown units'),
                    production_unit_files=lambda units: {str(gate): pin(gate)}, production_effective=lambda document: None)
            ns = dict(Path=Path, hashlib=hashlib, json=json, require=require, guard=guard, save=save,
                      regular=lambda path: path, digest=digest, publish_new=publish, sync_dir=lambda path: None,
                      production_boot_work_read=lambda runtime, path: read(path),
                      production_owner_proof=lambda: dict(pid='PRIVATE_OWNER'), systemctl=systemctl,
                      activation_capacity_preflight=lambda *args: None,
                      replace=lambda path, raw, mode: path.write_bytes(raw),
                      units_stopped=lambda units: require(not running[0], 'participant still running'),
                      no_processes=lambda *args: require(not running[0], 'runtime still running'), no_mappings=lambda *args, **kw: None)
            exec(compile(ast.Module(body=functions, type_ignores=[]), str(SOURCE), 'exec'), ns)
            runtime = Runtime()
            with self.assertRaisesRegex(OSError, 'directory fsync'):
                ns['production_boot_start_current'](runtime, baseline, work_path)
            self.assertFalse(running[0])
            self.assertEqual((read(work_path)['phase'], read(work_path)['startAttempt']), ('FAILED_STOPPED', 1))
            first = list(root.glob('*-attempt-1-health-*.json'))
            self.assertEqual(len(first), 1)
            first_bytes = first[0].read_bytes()
            self.assertEqual(read(first[0])['health']['readers']['gateway-services.service']['pid'], '101')
            ns['production_boot_start_current'](runtime, baseline, work_path)
            latest = read(work_path)
            self.assertEqual((latest['startAttempt'], latest['healthAttempt']), (2, 2))
            self.assertEqual(read(root / latest['healthRecord'])['health']['readers']['gateway-services.service']['pid'], '102')
            self.assertEqual(first[0].read_bytes(), first_bytes)
            self.assertNotEqual(latest['healthRecord'], first[0].name)
            self.assertTrue(running[0])


if __name__ == '__main__':
    unittest.main()
