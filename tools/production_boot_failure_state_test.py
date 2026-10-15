"""Real boot failure handler with private state; not systemd/hardware proof."""
import ast
import json
from pathlib import Path
import tempfile
import types
import unittest


class BootFailureStateTest(unittest.TestCase):
    def exercise(self, stop_fails):
        source = Path(__file__).resolve().parents[1] / 'deploy/offline-runtime-upgrade.py'
        names = {'production_boot_start_current', 'production_boot_begin_start'}
        tree = ast.parse(source.read_bytes())
        functions = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in names]
        self.assertEqual(len(functions), len(names))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / 'resume.json'
            path.write_text(json.dumps({'phase': 'DISPATCHED'}))
            gate = root / 'gate.conf'
            gate.write_bytes(b'approved')
            phases, stopped = [], []
            baseline = dict(allowedUnits=['test.service'], gates={'test.service': str(gate)},
                            unitFiles={str(gate): 'pin'})
            def require(condition, message):
                if not condition:
                    raise ValueError(message)
            def read(runtime, path):
                return json.loads(path.read_bytes())
            def save(path, value):
                phases.append(value['phase'])
                path.write_text(json.dumps(value))
            def health(baseline):
                raise ValueError('injected endpoint health failure')
            def stop(baseline):
                stopped.append(True)
                if stop_fails:
                    raise ValueError('retired source recreated on new boot: gateway_point_store')
            runtime = types.SimpleNamespace(home=root, health=health, stop=stop)
            guard = types.SimpleNamespace(production_verify=lambda *a: None,
                production_unit=lambda *a: None, production_pin=lambda p: 'pin')
            ns = dict(Path=Path, require=require, guard=guard, save=save,
                production_boot_work_read=read, production_owner_proof=lambda: {'pid': 'test'},
                activation_capacity_preflight=lambda *a: None, production_start_units=lambda b: ['test.service'],
                systemctl=lambda *a: None, production_inhibit=lambda h: b'inhibited',
                replace=lambda p, body, mode: p.write_bytes(body))
            exec(compile(ast.Module(body=functions, type_ignores=[]), str(source), 'exec'), ns)
            expected = 'retired source recreated' if stop_fails else 'endpoint health failure'
            with self.assertRaisesRegex(ValueError, expected):
                ns['production_boot_start_current'](runtime, baseline, path)
            self.assertEqual(stopped, [True])
            self.assertEqual(gate.read_bytes(), b'inhibited')
            final = 'FAILED_STOP_UNCONFIRMED' if stop_fails else 'FAILED_STOPPED'
            self.assertEqual(read(runtime, path)['phase'], final)
            self.assertIn('FAILED_STOP_UNCONFIRMED', phases)
            if stop_fails:
                with self.assertRaisesRegex(ValueError, 'unconfirmed'):
                    ns['production_boot_start_current'](runtime, baseline, path)
                self.assertEqual(stopped, [True], 'uncertain stop must not retry start')

    def test_failed_stop_is_terminal_not_starting(self):
        self.exercise(True)

    def test_confirmed_stop_remains_retryable(self):
        self.exercise(False)


if __name__ == '__main__':
    unittest.main()
