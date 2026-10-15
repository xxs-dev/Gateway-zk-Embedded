"""Bootstrap template/instance gates must both be visible to systemd."""
import importlib.util
import os
from pathlib import Path
import subprocess
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[1]


def helper():
    path = REPO / 'deploy/runtime-bootstrap-activate.py'
    spec = importlib.util.spec_from_file_location('bootstrap_unit_gates', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@unittest.skipUnless(os.name == 'posix', 'Linux lifecycle helpers')
class UnitGateTest(unittest.TestCase):
    def test_template_and_instance_names_do_not_shadow(self):
        m = helper()
        units = ['system-monitor@.service', 'system-monitor@monitor-service.service']
        gates, additions = m.unit_additions('FIRST', units, {'roles': []})
        self.assertEqual(len({Path(p).name for p in gates.values()}), 2)
        for unit in units:
            self.assertEqual(additions[gates[unit]], m.lifecycle.production_gate(m.prepare.HOME, unit))

    @unittest.skipUnless(os.environ.get('GATEWAY_BOOTSTRAP_GATE_SYSTEMD_TEST') == '1',
                         'opt-in real systemd metadata test; no service is started')
    def test_real_manager_loads_every_pinned_gate(self):
        self.assertEqual(os.geteuid(), 0)
        m = helper()
        root = Path('/run/systemd/system')
        prefix = 'gateway-bootstrap-gate-probe-' + str(os.getpid())
        template, instance = prefix + '@.service', prefix + '@one.service'
        fragment = root / template
        directories, files = [], []
        try:
            with fragment.open('x') as out:
                out.write('[Service]\nType=oneshot\nExecStart=/bin/true\n')
            files.append(fragment)
            with mock.patch.object(m.reprovision, 'UNIT_DIR', root):
                gates, additions = m.unit_additions('PROBE', [template, instance], {'roles': []})
            for name, raw in additions.items():
                path = Path(name)
                path.parent.mkdir(mode=0o755)
                directories.append(path.parent)
                with path.open('xb') as out:
                    out.write(raw)
                files.append(path)
            subprocess.run(['systemctl', 'daemon-reload'], check=True, timeout=30)
            def show(key):
                return subprocess.check_output(['systemctl', 'show', instance, '-p', key, '--value'],
                                               timeout=15, text=True).strip()
            self.assertEqual(show('ActiveState'), 'inactive')
            self.assertEqual(show('MainPID'), '0')
            self.assertEqual(set(show('DropInPaths').split()), set(gates.values()))
        finally:
            for path in reversed(files):
                path.unlink()
            for path in reversed(directories):
                path.rmdir()
            subprocess.run(['systemctl', 'daemon-reload'], check=True, timeout=30)


if __name__ == '__main__':
    unittest.main()
