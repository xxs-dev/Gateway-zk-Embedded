#!/usr/bin/env python3
"""Verify the stop condition with harmless, uniquely named real systemd units.

Does not start/stop gateway units. Does not reboot/reexec the host manager.
"""
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import uuid


def run(*args):
    result = subprocess.run(['systemctl', *args], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, universal_newlines=True, timeout=30)
    if result.returncode:
        raise RuntimeError(result.stdout)
    return result.stdout.strip()


def value(unit, prop):
    return run('show', '--property=' + prop, '--value', unit)


def main():
    if os.geteuid() != 0:
        raise RuntimeError('requires root for unique runtime-only systemd fixture units')
    source = Path(__file__).resolve().parents[1] / 'deploy/gateway-reprovision.py'
    spec = importlib.util.spec_from_file_location('fence_fixture', str(source))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    runtime = Path('/run/systemd/system')
    name = 'gateway-reprovision-fixture-' + uuid.uuid4().hex
    template = name + '@.service'
    instances = [name + '@one.service', name + '@two.service']
    fragment = runtime / template
    overrides = runtime / (template + '.d')
    fence = overrides / module.FENCE_NAME
    owned = []
    with tempfile.TemporaryDirectory(prefix='gateway-reprovision-fence-', dir='/opt') as temporary:
        root = Path(temporary)
        stop = root / 'STOP'
        marker = root / 'ran'
        stop.write_bytes(module.STOP_BODY)
        try:
            body = ('[Unit]\nDescription=Harmless reprovision fence fixture\n'
                    '[Service]\nType=oneshot\nRemainAfterExit=yes\nExecStart=/usr/bin/touch ' + str(marker) + '\n'
                    'PrivateNetwork=yes\nPrivateDevices=yes\nProtectSystem=strict\n'
                    'ReadWritePaths=' + str(root) + '\nNoNewPrivileges=yes\n'
                    'CapabilityBoundingSet=\nAmbientCapabilities=\n').encode()
            module.lifecycle.write_new(fragment, body, 0o644)
            owned.append(fragment)
            overrides.mkdir(mode=0o755)
            owned.append(overrides)
            stop_body = module.fence_body().replace(os.fsencode(str(module.ROOT / 'STOP')),
                                                   os.fsencode(str(stop)))
            module.lifecycle.write_new(fence, stop_body, 0o644)
            owned.append(fence)
            run('daemon-reload')
            for unit in instances:
                for _ in range(2):
                    run('start', unit)
                    assert value(unit, 'ConditionResult') == 'no'
                    assert value(unit, 'MainPID') == '0'
                    assert value(unit, 'ActiveState') == 'inactive'
                    assert str(fence) in value(unit, 'DropInPaths').split()
                    assert value(unit, 'NeedDaemonReload') == 'no'
                    assert not marker.exists()
                    run('daemon-reload')
            # Prove the skipped service actually would execute without the marker.
            stop.unlink()
            run('start', instances[0])
            assert value(instances[0], 'ConditionResult') == 'yes'
            assert marker.is_file()
            print('REAL_SYSTEMD_TEMPLATE_INSTANCE_FENCE_RELOAD_PASS', flush=True)
        finally:
            for unit in instances:
                if value(unit, 'LoadState') != 'not-found':
                    run('stop', unit)
                    if value(unit, 'MainPID') != '0':
                        raise RuntimeError('fixture process not stopped; retain evidence')
            for path in reversed(owned):
                if path.is_dir():
                    path.rmdir()
                else:
                    path.unlink()
            run('daemon-reload')


if __name__ == '__main__':
    main()
